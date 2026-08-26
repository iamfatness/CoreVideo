#include "engine-talkback.h"
#include "engine-writer.h"   // EngineIpc::write -- an inline fn in a namespace,
                             // so it must be INCLUDED, never forward-declared
#include "talkback-tone.h"
#include "engine-json.h"     // zchar_to_utf8 / json_escape / json_str (Step 3a)
#include "talkback-ring.h"   // talkback_ring_drain / TalkbackRingSlotFn (Milestone 2)

#include <chrono>
#include <string>
#include <thread>

namespace {
// A hung AwaitingChannel/AwaitingInvite rung and a genuine permission denial
// look identical from the outside (both are "nothing happened yet") unless
// something bounds how long we wait -- see the timeout block in tick().
constexpr std::chrono::milliseconds kAwaitTimeout{10000};

// BeginBatchDestroyChannels/AddChannelToDestroy/ExecuteBatchDestroyChannels
// can themselves fail synchronously (distinct from the channel simply never
// confirming destruction). Give up on the first failure and the "always
// destroy" guarantee this file claims is false; retry a bounded number of
// times instead.
constexpr uint32_t kMaxDestroyAttempts = 5;
} // namespace

void EngineTalkback::report(const std::string &stage, const std::string &fields) const
{
    std::string line = R"({"cmd":"talkback_probe","stage":")" + stage + "\"";
    if (!fields.empty()) line += "," + fields;
    line += "}";
    EngineIpc::write(line);
}

// F6 review-round fix: same shape as report() above, tagged
// "cmd":"talkback_session" instead of "talkback_probe" -- see the header
// comment on report()/report_session() for why the two must not share a
// cmd. Every call site of this function is reachable only from
// session_start()/session_stop()/onCreateChannelResponse()'s Session branch
// or the Milestone 2 audio path (open_audio/drain_audio/close_audio), which
// the probe never calls.
void EngineTalkback::report_session(const std::string &stage, const std::string &fields) const
{
    std::string line = R"({"cmd":"talkback_session","stage":")" + stage + "\"";
    if (!fields.empty()) line += "," + fields;
    line += "}";
    EngineIpc::write(line);
}

// F2 review-round fix (CRITICAL): the confirmed-state line -- see the header
// comment. Deliberately a different shape from report_session() above (no
// "stage" key, a top-level "live" key instead) so
// ZoomEngineClient::handle_event() can tell the two apart on the same cmd
// without any ordering assumption between them.
// Task 2: nominate()'s progress line, tagged "cmd":"talkback_nominate" --
// same string as the P2E command that triggers it, mirroring report()'s
// convention above rather than report_session()'s (see the header comment
// on report_nomination() for why the two families differ here).
void EngineTalkback::report_nomination(const std::string &stage, const std::string &fields) const
{
    std::string line = R"({"cmd":"talkback_nominate","stage":")" + stage + "\"";
    if (!fields.empty()) line += "," + fields;
    line += "}";
    EngineIpc::write(line);
}

void EngineTalkback::report_session_state(bool live, const std::string &reason) const
{
    std::string line = R"({"cmd":"talkback_session","live":)" +
                        std::string(live ? "true" : "false") +
                        R"(,"reason":")" + json_escape(reason) + "\"}";
    EngineIpc::write(line);
}

bool EngineTalkback::probe_refused_without_ladder()
{
    // CONSTRAINT: a tick()-driving thread may exist ONLY while there is SDK
    // work that thread owns -- a probe ladder it must advance. Every probe()
    // exit that did not issue a CreateChannel routes through here, and here
    // returns false, because main.cpp spawns that thread on probe()'s return
    // value alone.
    //
    // Why the constraint, not just the tidiness: the driving thread is the
    // ONLY other thread that drives the batch-destroy API (see the inventory
    // at the top of tick()), and four branches of onCreateChannelResponse
    // now destroy directly on the command-loop thread. Those two can only
    // collide if a driving thread exists while a Session- or
    // Nomination-owned create response is in flight -- which is exactly the
    // state a refused probe leaves behind, since the refusal reason IS that
    // someone else owns the arbiter. A probe that genuinely issued a create
    // owns the arbiter itself, so no Session/Nomination response can be
    // delivered for as long as its thread lives, and the two callers cannot
    // overlap. That is the whole reason "no ladder -> no thread" is a
    // constraint rather than a preference: it is what makes the batch-destroy
    // serialization argument hold.
    //
    // Phase::Done, not Idle: the ladder settled without starting, and
    // main.cpp's own "already in progress" gate reads is_idle().
    m_phase.store(Phase::Done, std::memory_order_release);

    // Drain here, synchronously, instead of leaving it to the thread we are
    // deliberately not spawning. A queued stray keeps has_pending_work()
    // true, and has_pending_work() gates the top of BOTH nominate() and
    // session_start() -- the C1 wedge shape -- so "no thread" must not mean
    // "nobody drains". Safe on this thread precisely because there is no
    // driving thread: main.cpp joins it before every probe() call, so this
    // is the sole batch-destroy caller for the duration. A null or
    // unsupported controller leaves the queue intact for a later probe;
    // drain_stray_channels() self-guards for that.
    drain_stray_channels();
    return false;
}

bool EngineTalkback::probe(ZOOMSDK::IMeetingService *svc,
                           const std::string &participant_name)
{
    // Re-entrancy guard: Task 5 wires this to a control-API command a human
    // can send twice. A second call while a channel is live would otherwise
    // unconditionally reset m_channel_id_z out from under the live ladder,
    // discarding the only handle we have to destroy it -- refusing is
    // correct for a probe; there is nothing sensible to queue or cancel.
    // Returning false here (rather than starting a ladder) is also the
    // caller's signal not to spawn a second tick()-driving thread -- see the
    // return-value comment on this function's declaration.
    //
    // Deliberately NOT routed through probe_refused_without_ladder(): a
    // ladder IS in flight here, so a driving thread is alive and may be
    // inside its own Begin/Add/Execute right now. That helper drains strays
    // synchronously, which is safe only when this thread is the sole
    // batch-destroy caller -- true on every other refusal path, false on
    // this one. The thread already owns the drain while it runs.
    const Phase current = m_phase.load(std::memory_order_acquire);
    if (current != Phase::Idle && current != Phase::Done) {
        report("busy", R"("phase":)" + std::to_string(static_cast<int>(current)));
        return false;
    }

    // R1 review-round fix (mutual exclusion): the probe and the persistent
    // session must never run concurrently. Before this, session_start()
    // reassigned m_svc/m_ctrl -- the exact same fields tick() dereferences on
    // its OWN driving thread while a probe is in flight -- with no gate at
    // all: a genuine cross-thread pointer race (Critical 2). Refusing here
    // rather than queueing also means tick()'s batch-destroy (for the
    // probe's channel) and session_stop()'s batch-destroy (for the session's
    // channel) can never overlap on the same controller object (Important
    // 4). A probe is a ~3s diagnostic; refusing it for the life of an active
    // talkback session costs nothing real -- do not "relax" this to allow
    // concurrent use. See the matching guard in session_start().
    //
    // Also not routed through probe_refused_without_ladder(), for a
    // different reason than the guard above: no driving thread can exist
    // here (this guard is what stops one starting, and session_start()
    // joins any earlier one before going live), so draining would be safe
    // -- but it would be a NEW batch-destroy on a live show's command loop,
    // which is the session path this task is explicitly not reworking.
    // Unchanged behaviour: a stray queued during a live session waits for
    // the next probe. The return value, which is what this round is about,
    // is false either way.
    if (m_session_live) {
        report("busy", R"("reason":"session_live")");
        return false;
    }

    m_svc = svc;
    m_participant_name = participant_name;
    m_phase.store(Phase::Idle, std::memory_order_release);
    {
        // Cross-thread: a late callback from the PREVIOUS probe can still be
        // reading these on the SDK thread right now -- see the m_chan_mtx
        // comment in the header.
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        m_channel_id.clear();
        m_channel_id_z.clear();
    }
    m_participant_id = 0;
    m_tone_index = 0;
    m_buffers_sent = 0;
    m_destroy_attempts = 0;

    if (!m_svc) {
        report("controller", R"("ok":false,"reason":"no_meeting_service")");
        // The one created-nothing exit that deliberately does NOT drain:
        // m_ctrl is only reassigned at RUNG 1 below, which this exit
        // precedes, so with no meeting service it may still point at a
        // PREVIOUS meeting's controller -- and there is no meeting to
        // destroy channels in anyway. Leave the queue for a probe that has
        // a live controller; that is exactly what drain_stray_channels()'
        // own null-guard does for the neighbouring case. The return value
        // is the same false: no ladder, no driving thread.
        m_phase.store(Phase::Done, std::memory_order_release);
        return false;
    }

    // RUNG 1: does the controller exist at all on this SDK/account?
    m_ctrl = m_svc->GetMeetingTalkbackController();
    report("controller", std::string(R"("ok":)") + (m_ctrl ? "true" : "false"));
    if (!m_ctrl) return probe_refused_without_ladder();

    // RUNG 2: the meeting-level gate. This is the one we expect Enhanced Media
    // to satisfy, and the one that decides whether the feature is viable.
    const bool supported = m_ctrl->IsMeetingSupportTalkBack();
    report("meeting_supported",
           std::string(R"("supported":)") + (supported ? "true" : "false"));
    if (!supported) return probe_refused_without_ladder();

    const ZOOMSDK::SDKError set_err = m_ctrl->SetEvent(this);
    report("set_event", R"("code":)" + std::to_string(static_cast<int>(set_err)));
    if (set_err != ZOOMSDK::SDKERR_SUCCESS) return probe_refused_without_ladder();

    // RUNG 3: create exactly one channel. Max 16 exist; we make one and
    // destroy it, so a failed probe cannot leak channel budget into the
    // meeting.
    //
    // Gate behind the same arbiter session_start() uses: exactly one create
    // may be outstanding across the probe and the session (see
    // src/talkback-channel-owner.h). Refuse rather than queue -- there is
    // nothing sensible to queue, and a queued create would arrive with the
    // other subsystem's response still in flight. m_pending_create is
    // guarded by m_chan_mtx (see the header comment on it) -- copy the
    // decision out under the lock, release, THEN call the SDK, same
    // discipline as every other m_chan_mtx access in this file.
    bool create_gate_ok;
    TalkbackChannelOwner expired_owner;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        // Follow-up to the F1 review-round fix, extended for Task 2: lazily
        // unwedge a stale Session- or Nomination-owned pending create before
        // evaluating the gate -- see expire_stale_pending_create_locked()'s
        // doc comment.
        expired_owner = expire_stale_pending_create_locked();
        create_gate_ok = talkback_may_request_create(m_pending_create);
    }
    handle_expired_create(expired_owner);
    if (!create_gate_ok) {
        // THIS is the refusal that made the two-batch-destroy-callers window
        // reachable while it still reported success -- see
        // probe_refused_without_ladder() and the inventory at the top of
        // tick(). The gate is closed precisely because a Session- or
        // Nomination-owned create is outstanding, i.e. precisely when a
        // response that destroys directly is about to land on the command
        // loop; a driving thread spawned here would be the second
        // batch-destroy caller.
        report("busy", R"("reason":"create_busy")");
        return probe_refused_without_ladder();
    }
    const ZOOMSDK::SDKError create_err = m_ctrl->CreateChannel(1);
    report("create_channel", R"("code":)" +
           std::to_string(static_cast<int>(create_err)));
    if (create_err != ZOOMSDK::SDKERR_SUCCESS) return probe_refused_without_ladder();
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        m_pending_create = TalkbackChannelOwner::Probe;
    }
    m_phase_deadline.store(
        (std::chrono::steady_clock::now() + kAwaitTimeout).time_since_epoch().count(),
        std::memory_order_release);
    m_phase.store(Phase::AwaitingChannel, std::memory_order_release);   // continues in onCreateChannelResponse
    return true;
}

void EngineTalkback::drain_stray_channels()
{
    // TWO callers, and the constraint they share is that the caller must be
    // the only batch-destroy caller alive at that moment (see the inventory
    // at the top of tick()): tick(), on the probe's driving thread, which
    // exists only for a ladder that thread owns; and
    // probe_refused_without_ladder(), on the command-loop thread, reached
    // only when no driving thread exists at all (main.cpp joins it before
    // every probe()). Do not add a third without re-deriving that. Swap the
    // queue out under lock, then never touch the SDK while holding
    // m_chan_mtx.
    //
    // F6 review-round fix: m_ctrl can be null here. probe()'s RUNG 1
    // reassigns m_ctrl on EVERY call (`m_ctrl = m_svc->GetMeetingTalkback
    // Controller();`), so a LATER probe that fails to obtain a controller
    // nulls it out while an EARLIER ladder's stray, queued by a successful
    // probe, is still sitting in m_stray_channels waiting to be drained.
    // Guard before touching the queue at all, so a null m_ctrl leaves the
    // strays queued for a later tick() rather than swapping them out and
    // losing them silently -- losing a leaked-channel record silently is
    // exactly the failure mode this class exists to avoid.
    if (!m_ctrl) return;

    // R1-round-3 review fix (the has_pending_work() gap): has_pending_work()
    // used to infer "the driving thread might still touch the SDK" from
    // m_phase and m_stray_channels alone. Both can read "nothing to see
    // here" while THIS function is still live: the swap below empties the
    // m_stray_channels member immediately, and m_phase can independently
    // already be Done (a stray drained on the very tick() that settles the
    // ladder, or on a later call after the ladder already settled) -- so for
    // the entire window from here through the end of the SDK loop below,
    // has_pending_work() would report "safe to proceed" to session_start()
    // on the OTHER thread while this thread is still calling
    // m_ctrl->BeginBatchDestroyChannels() and friends. session_start()'s R1
    // mutual-exclusion gate depends on has_pending_work() being right about
    // that, so set the flag BEFORE the swap (not after -- the swap itself,
    // under m_chan_mtx, is part of the window a concurrent reader must see
    // as busy) and clear it via RAII so every exit from here on -- today
    // that's the empty-queue early return right below and falling off the
    // end of the loop, but a future edit adding another early return does
    // not have to remember to duplicate the clear.
    m_driving_thread_in_sdk_call.store(true, std::memory_order_release);
    struct ClearOnExit {
        std::atomic<bool> &flag;
        ~ClearOnExit() { flag.store(false, std::memory_order_release); }
    } clear_on_exit{m_driving_thread_in_sdk_call};

    std::vector<std::basic_string<zchar_t>> strays;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        if (m_stray_channels.empty()) return;
        strays.swap(m_stray_channels);
    }

    for (const auto &id : strays) {
        // Bounded, local retry -- deliberately not tied to m_destroy_attempts
        // (that counter is reserved for the main channel) and deliberately
        // not spread across future tick() calls, so one stray can never
        // grow into an unbounded retry loop.
        ZOOMSDK::SDKError e = ZOOMSDK::SDKERR_UNKNOWN;
        uint32_t attempt = 0;
        for (; attempt < kMaxDestroyAttempts; ++attempt) {
            e = m_ctrl->BeginBatchDestroyChannels();
            if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->AddChannelToDestroy(id.c_str());
            if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->ExecuteBatchDestroyChannels();
            if (e == ZOOMSDK::SDKERR_SUCCESS) break;
        }
        report("stray_destroy",
               R"("channel":")" + json_escape(zchar_to_utf8(id.c_str())) +
               R"(","code":)" + std::to_string(static_cast<int>(e)) +
               R"(,"attempts":)" + std::to_string(attempt + 1));
        if (e != ZOOMSDK::SDKERR_SUCCESS) {
            report("stray_destroy_abandoned",
                   R"("channel":")" + json_escape(zchar_to_utf8(id.c_str())));
        }
    }
}

TalkbackChannelOwner EngineTalkback::expire_stale_pending_create_locked()
{
    // Caller holds m_chan_mtx -- see the header comment on this function and
    // on m_session_create_deadline / m_nomination_create_deadline. Only
    // Session and Nomination ever need this: Probe's pending create already
    // has a clearer of its own (tick()'s AwaitingChannel timeout, running on
    // the driving thread), so a stale Probe entry here would be a bug in
    // that machinery, not something this function should paper over.
    std::atomic<std::chrono::steady_clock::rep> *deadline_field = nullptr;
    if (m_pending_create == TalkbackChannelOwner::Session)
        deadline_field = &m_session_create_deadline;
    else if (m_pending_create == TalkbackChannelOwner::Nomination)
        deadline_field = &m_nomination_create_deadline;
    else
        return TalkbackChannelOwner::None;

    const auto deadline = std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(
            deadline_field->load(std::memory_order_acquire)));
    if (std::chrono::steady_clock::now() < deadline) return TalkbackChannelOwner::None;

    // The response never arrived (or arrived and was somehow lost before
    // reaching onCreateChannelResponse -- either way, indistinguishable from
    // here). Forget the pending create so talkback_may_request_create()
    // unwedges for probe(), session_start(), AND nominate(). Whichever owner
    // this was gets reported by the caller -- see the header comment on this
    // function for why the report happens outside m_chan_mtx.
    //
    // Fix round 2, N1 (Major, introduced by fix round 1): the owner AND that
    // owner's own cancellation flag are now cleared TOGETHER by ONE shared
    // function, talkback_expire() (src/talkback-channel-owner.h), instead of
    // this function hand-rolling a separate if/else per owner. Round 1's
    // hand-rolled version cleared m_session_create_cancelled in the Session
    // arm but forgot the equivalent line for m_nomination_create_cancelled
    // in the Nomination arm: nominate() -> Leave (before the response is
    // pumped) -> nomination_reset() sets the flag and leaves the owner as
    // Nomination -> the response never arrives at all -> more than
    // kAwaitTimeout later, a fresh nominate() re-arms the owner -> THIS
    // function fired, cleared the owner, but the now-orphaned cancelled flag
    // survived -> the FRESH create's own response later arrived, was
    // claimed as Nomination, found the flag still true, and destroyed the
    // brand-new channel instead of adopting it -- the operator's next
    // nomination silently provisioned zero channels. Routing both owners
    // through the one pure function is what makes that class of asymmetry
    // impossible to reintroduce without also breaking
    // CoreVideoTalkbackCreateState's test of it.
    const TalkbackChannelOwner expired = m_pending_create;
    TalkbackCreateState state{m_pending_create, m_session_create_cancelled,
                              m_nomination_create_cancelled};
    state = talkback_expire(state);
    m_pending_create           = state.owner;
    m_session_create_cancelled = state.session_cancelled;
    m_nomination_create_cancelled = state.nomination_cancelled;

    if (expired == TalkbackChannelOwner::Session) {
        // Consequence, stated explicitly rather than left implicit: if that
        // CreateChannel response arrives AFTER this point,
        // onCreateChannelResponse will see owner == None (m_pending_create
        // no longer says Session) and this callback's id-comparison
        // fallback will not match m_channel_id_z or m_session_channel_z
        // either -- so it is queued onto m_stray_channels and handled by
        // the ordinary stray path instead of the Session/cancelled path
        // above. That is correct, not an oversight: by the time this
        // expires, nobody is tracking that create as "ours" anymore, so a
        // stray is the right disposition -- the same one an untracked
        // channel from any other source gets.
    } else {
        // A swallowed Nomination create response also means the rest of the
        // queued plan can never be provisioned by THIS ladder (there is
        // nothing to resume from -- we don't know if the channel exists).
        // Forget the queue so a later nominate() starts clean instead of
        // silently continuing to believe channels are still coming.
        //
        // Safe to do unconditionally, including when THIS very call is
        // nested inside a fresh nominate()'s own nomination_create_next():
        // nominate() (fix round 2) now calls nomination_create_next() --
        // and therefore this function -- BEFORE assigning its own plan into
        // m_nomination_pending, specifically so that if an expiry fires
        // here it can only ever be clearing a genuinely stale leftover
        // queue from an EARLIER ladder, never the queue the calling
        // nominate() is about to populate. Do not move this clear to run
        // AFTER that assignment without re-verifying that ordering still
        // holds -- see nominate()'s own comment on why the order matters.
        m_nomination_pending.clear();

        // Fix round 3, "expire-path double create": bump the nomination
        // generation too -- see src/talkback-channel-owner.h's "Generation
        // tracking" section. This create's response, if it ever arrives
        // before anything else is issued, must be recognized as stale
        // rather than confused with whatever a later nominate() issues.
        // Deliberately leaves the outstanding STAMP in place (fix round 4):
        // that is what makes this abandoned create's own response read
        // Stale, and the next talkback_generation_issue() overwrites the
        // stamp rather than queuing behind it, so nothing can accumulate.
        m_nomination_generation = talkback_generation_bump(m_nomination_generation);

        // Fix round 4: m_provisioned_channels is NOT cleared here -- the
        // caller destroys it via handle_expired_create() once the lock is
        // released, because those are real Zoom channels and clearing the
        // table without destroying them would leak channel budget for the
        // rest of the meeting. See handle_expired_create()'s comment for
        // why the expiry must not leave them standing either.
    }
    return expired;
}

void EngineTalkback::handle_expired_create(TalkbackChannelOwner expired_owner)
{
    // Fix round 4. Called by all four callers of
    // expire_stale_pending_create_locked() AFTER they release m_chan_mtx --
    // never with it held: the Nomination arm calls the SDK. See the
    // declaration comment for why the Nomination arm destroys.
    if (expired_owner == TalkbackChannelOwner::Session) {
        report_session("session_create_expired",
                       R"("reason":"swallowed_create_response")");
    } else if (expired_owner == TalkbackChannelOwner::Nomination) {
        report_nomination("create_expired",
                          R"("reason":"swallowed_create_response")");
        // The expiry has already forgotten the queue (m_nomination_pending)
        // but the channels this ladder DID create are still standing on
        // Zoom, still in m_provisioned_channels, and still refusing every
        // later nominate() with "already_provisioned" -- a Major found by
        // the round-3 re-review: one transient swallowed response cost the
        // operator re-nomination for the rest of the meeting, recoverable
        // only by a Leave. Destroy them so the next nominate() starts
        // clean, exactly as fix round 1's M2 does for an error response.
        // No-op when the table is empty, which is the common case.
        nomination_destroy_provisioned();
    }
}

void EngineTalkback::tick()
{
    // BATCH-DESTROY DISCIPLINE. The previous version of this comment said
    // "tick() is the ONLY caller of the batch-destroy API ... Callbacks ...
    // never call the API themselves". That stopped being true in Task 2 fix
    // round 1 and was left standing through two more rounds. Counted from
    // the code in fix round 4 (grep BeginBatchDestroyChannels), there are
    // FOUR Begin/Add/Execute sequences in this file, on TWO threads:
    //   * drain_stray_channels() (called only from here) and tick()'s own
    //     Destroying phase -- the PROBE'S DRIVING THREAD;
    //   * destroy_channel_retrying() -- five call sites: four branches of
    //     onCreateChannelResponse (session-cancelled, nomination-stale,
    //     nomination-cancelled, nomination-untracked) plus
    //     nomination_destroy_provisioned() -- and session_stop()'s own copy
    //     of the loop: the COMMAND-LOOP THREAD.
    // The hazard the old comment named is real and unchanged: the API shape
    // implies the controller holds implicit per-batch state, so two
    // Begin/Add/Execute sequences interleaving on different threads could
    // merge or corrupt batches. What keeps them apart is NOT a single-caller
    // rule -- it is a chain of three facts, each of which must hold:
    //   1. This thread exists ONLY for a probe that issued a CreateChannel.
    //      probe() returns false from every exit that created nothing, so
    //      main.cpp spawns nothing for it (see probe()'s return-value
    //      contract in the header, and probe_refused_without_ladder()).
    //   2. Such a probe HOLDS the arbiter (m_pending_create == Probe), which
    //      excludes any Session- or Nomination-owned create for as long as
    //      it does -- and the four command-loop destroy branches all sit
    //      inside `owner == Session` / `owner == Nomination`, so none of
    //      them can be reached while this thread is alive.
    //   3. Every other command-loop destroy (session_stop(),
    //      nomination_destroy_provisioned() via nominate()/
    //      handle_expired_create()) is reached only from command branches
    //      that JOIN this thread first -- main.cpp joins before
    //      session_start() and before nominate(), and probe() itself is
    //      called after a join (see session_stop()'s F3 comment, which
    //      traced exactly this for the session).
    // Fact 1 was FALSE until fix round 4: a probe refused because the
    // arbiter was held -- i.e. refused precisely when a Session/Nomination
    // response was about to land -- still returned true and still got a
    // driving thread, which could then be inside drain_stray_channels()'
    // batch while that response destroyed on the command loop. Breaking
    // fact 1 again breaks the whole chain, which is why probe()'s return
    // value is documented as a contract rather than a status.
    //
    // Do NOT instead serialize these with a mutex: blocking the SDK's
    // message-pump thread while the driving thread sits inside an SDK call
    // trades a rare batch corruption for a possible hard hang, and in this
    // engine a hang is the worse failure.
    drain_stray_channels();

    const Phase phase = m_phase.load(std::memory_order_acquire);

    if (phase == Phase::AwaitingChannel || phase == Phase::AwaitingInvite) {
        // SDKERR_SUCCESS on CreateChannel/ExecuteBatchInviteUsers is only a
        // promise the call was accepted, not that onCreateChannelResponse /
        // onChannelUserJoinResponse will ever fire. Without this deadline a
        // swallowed callback hangs here forever and reports nothing -- a
        // hang IS silence, and it is indistinguishable from a permission
        // failure at exactly the moment we need to tell the two apart.
        const auto deadline = std::chrono::steady_clock::time_point(
            std::chrono::steady_clock::duration(
                m_phase_deadline.load(std::memory_order_acquire)));
        if (std::chrono::steady_clock::now() >= deadline) {
            report("timeout", R"("phase":)" + std::to_string(static_cast<int>(phase)));
            // R3 review-round fix: an AwaitingChannel timeout means the
            // CreateChannel THIS probe issued was accepted but its response
            // never arrived -- that response is what would normally clear
            // m_pending_create (via the arbiter in onCreateChannelResponse).
            // Left set, it wedges every future CreateChannel from EITHER
            // subsystem for the rest of the process's life, and it wedges
            // at exactly the moment the SDK is already misbehaving -- the
            // one time this diagnostic most needs to stay usable. Only
            // AwaitingChannel, never AwaitingInvite: by AwaitingInvite the
            // create already succeeded and onCreateChannelResponse already
            // cleared this. This is a completion of the existing timeout,
            // not a change to its Phase transitions or the re-entrancy
            // guard -- both are untouched. Runs on the probe's OWN driving
            // thread (this function's caller), unlike every other writer of
            // m_pending_create, so it goes through m_chan_mtx like the
            // channel-id strings do -- see the header comment on the field.
            if (phase == Phase::AwaitingChannel) {
                std::lock_guard<std::mutex> lock(m_chan_mtx);
                if (m_pending_create == TalkbackChannelOwner::Probe)
                    m_pending_create = TalkbackChannelOwner::None;
            }
            m_phase.store(Phase::Destroying, std::memory_order_release);
        }
        return;
    }

    if (phase == Phase::Sending) {
        // 10ms of mono 48kHz, matching the buffer size Zoom itself uses on the
        // receive side. dataLength must be a multiple of 2 (it is: 480 * 2).
        constexpr uint32_t kRate    = 48000;
        constexpr std::size_t kCount = 480;
        constexpr uint32_t kBuffers  = 300;   // ~3 seconds

        int16_t pcm[kCount];
        m_tone_index = talkback_tone_fill(pcm, kCount, m_tone_index, kRate, 440.0, 0.5);

        std::basic_string<zchar_t> channel_copy;
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            channel_copy = m_channel_id_z;
        }

        const ZOOMSDK::SDKError e = m_ctrl->SendAudioDataToChannel(
            channel_copy.c_str(), reinterpret_cast<const char *>(pcm),
            static_cast<unsigned int>(kCount * sizeof(int16_t)), kRate,
            ZOOMSDK::ZoomSDKAudioChannel_Mono);

        // Report the first send and any failure, never all 300 -- 300 pipe
        // lines is the message-storm shape this codebase already has a live
        // incident about.
        if (m_buffers_sent == 0 || e != ZOOMSDK::SDKERR_SUCCESS) {
            report("send", R"("buffer":)" + std::to_string(m_buffers_sent) +
                   R"(,"code":)" + std::to_string(static_cast<int>(e)));
        }
        if (e != ZOOMSDK::SDKERR_SUCCESS) {
            m_phase.store(Phase::Destroying, std::memory_order_release);
            return;
        }
        if (++m_buffers_sent >= kBuffers) {
            report("sent", R"("buffers":)" + std::to_string(m_buffers_sent));
            m_phase.store(Phase::Destroying, std::memory_order_release);
        }
        return;
    }

    if (phase == Phase::Destroying) {
        // Always destroy, on every exit path: a leaked channel consumes one of
        // the meeting's 16 for as long as the meeting lasts. The synchronous
        // Begin/Add/Execute chain can itself fail (separate from the channel
        // never confirming destruction via onDestroyChannelResponse); tick()
        // is already called repeatedly, so retry a bounded number of times
        // rather than abandoning the channel on the first failure -- giving
        // up immediately was exactly the leak this comment claims to
        // prevent. This call site and drain_stray_channels() above are the
        // only two batch-destroy sequences that run on THIS thread (the
        // probe's driving thread); two more run on the command-loop thread
        // -- see the corrected inventory at the top of tick().
        std::basic_string<zchar_t> channel_copy;
        std::string channel_copy_utf8;
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            channel_copy = m_channel_id_z;
            channel_copy_utf8 = m_channel_id;
        }

        ZOOMSDK::SDKError e = m_ctrl->BeginBatchDestroyChannels();
        if (e == ZOOMSDK::SDKERR_SUCCESS && !channel_copy.empty())
            e = m_ctrl->AddChannelToDestroy(channel_copy.c_str());
        if (e == ZOOMSDK::SDKERR_SUCCESS)
            e = m_ctrl->ExecuteBatchDestroyChannels();
        report("destroy", R"("code":)" + std::to_string(static_cast<int>(e)) +
               R"(,"attempt":)" + std::to_string(m_destroy_attempts + 1));

        if (e == ZOOMSDK::SDKERR_SUCCESS) {
            // F8 review-round fix: m_channel_id_z used to be cleared only at
            // the start of the NEXT probe(), never here -- so the probe's
            // throwaway channel id outlived the channel itself. If the audio
            // path (open_audio/drain_audio) is live at the same time, every
            // subsequent SendAudioDataToChannel targets a destroyed channel,
            // fails every buffer, and (before the drain_audio rate-limit
            // fixed elsewhere in this round) reported it on every drain --
            // the same message-storm shape this codebase already has a live
            // incident about. Clear both representations together, under the
            // same lock discipline as every other m_channel_id_z access.
            {
                std::lock_guard<std::mutex> lock(m_chan_mtx);
                m_channel_id.clear();
                m_channel_id_z.clear();
            }
            m_phase.store(Phase::Done, std::memory_order_release);
            return;
        }

        if (++m_destroy_attempts >= kMaxDestroyAttempts) {
            // Out of retries: the channel is now stranded for the rest of the
            // meeting (one of 16 gone for good) and nothing will self-heal
            // it. Make that visible rather than silently giving up.
            report("destroy_abandoned",
                   R"("channel":")" + json_escape(channel_copy_utf8) +
                   R"(","attempts":)" + std::to_string(m_destroy_attempts));
            // F8 review-round fix: same clearing as the success path above.
            // "Abandoned" means WE stop tracking it -- Zoom's copy of the
            // channel may or may not still exist, but this probe has given
            // up on it either way, and a stale id here causes exactly the
            // same audio_send failure storm as the success path.
            {
                std::lock_guard<std::mutex> lock(m_chan_mtx);
                m_channel_id.clear();
                m_channel_id_z.clear();
            }
            m_phase.store(Phase::Done, std::memory_order_release);
        }
    }
}

unsigned int EngineTalkback::resolve_participant(const std::string &name,
                                                 ReportSink sink) const
{
    if (!m_svc) return 0;
    auto *part = m_svc->GetMeetingParticipantsController();
    if (!part) return 0;
    ZOOMSDK::IList<unsigned int> *ids = part->GetParticipantsList();
    if (!ids) return 0;
    for (int i = 0; i < ids->GetCount(); ++i) {
        const unsigned int uid = ids->GetItem(i);
        ZOOMSDK::IUserInfo *u = part->GetUserByUserID(uid);
        if (!u) continue;
        if (zchar_to_utf8(u->GetUserName()) == name) {
            // F2 review-round fix: IsSupportTalkback() is a PER-USER gate,
            // distinct from IsMeetingSupportTalkBack() (the meeting-level
            // gate reported at RUNG 2 in probe()) -- a meeting can support
            // talkback while this specific participant's client cannot
            // receive it. We report it rather than refuse to proceed on
            // false: refusing would collapse the exact distinction this
            // probe exists to draw. Without this, "all rungs green, sent
            // buffers=300, talent heard nothing" and "talent's client
            // cannot receive talkback at all" are indistinguishable from
            // the log -- reporting supported=false and still attempting
            // the send is what makes the contrast between this value and
            // what the human actually hears into data.
            const bool supported = u->IsSupportTalkback();
            const std::string fields =
                R"("name":")" + json_escape(name) + R"(","user_id":)" +
                std::to_string(uid) + R"(,"supported":)" +
                (supported ? "true" : "false");
            // Fix round 1, M3: this line used to always go through report()
            // ("cmd":"talkback_probe"), which meant a nomination's per-user
            // gate -- the exact one Step 3 of the brief and invariant 5
            // require to be surfaced -- fired into the dock's PROBE status
            // label instead of the nomination stream, up to once per member
            // per channel (30-40x for a 24-nominee plan), and never reached
            // anything actually listening for nomination progress. Route by
            // sink instead of hardcoding report() -- see ReportSink's doc
            // comment in the header for why Probe stays the default.
            if (sink == ReportSink::Nomination)
                report_nomination("participant_talkback_support", fields);
            else
                report("participant_talkback_support", fields);
            return uid;
        }
    }
    return 0;
}

void EngineTalkback::onCreateChannelResponse(const zchar_t *channelID, TalkbackError error)
{
    const std::string id = zchar_to_utf8(channelID);
    report("create_channel_response",
           R"("channel":")" + json_escape(id) + R"(","error":)" +
           std::to_string(static_cast<int>(error)));

    // Route by who asked. See src/talkback-channel-owner.h: the response
    // carries no indication of its requester, so the arbiter is the only
    // thing standing between the probe and the session adopting each other's
    // channels. Claim and clear happen in the SAME lock scope -- once this
    // response has been attributed to an owner, m_pending_create must not be
    // observable as still "theirs" by anyone else, even for the instant
    // between a separate claim and a separate clear.
    //
    // Fix round 4: the Nomination generation state is updated HERE, in the
    // same scope, for EVERY response -- not inside the Nomination branch
    // below. Round 3 updated it only when a response reached that branch,
    // so a response claimed by None/Probe/Session silently skipped the
    // update; with round 3's FIFO that was one of the two paths that
    // desynchronised it permanently (see the field's comment in the
    // header). talkback_generation_on_response() takes the owner and
    // returns NotNomination-plus-unchanged-state for the others, so "what
    // happens to this state when someone else's response arrives" is
    // answered by one tested function instead of by a call that isn't
    // there. Pure -- no SDK call under m_chan_mtx.
    TalkbackChannelOwner owner;
    TalkbackResponseFreshness freshness;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        owner = talkback_claim_create(m_pending_create);
        if (owner != TalkbackChannelOwner::None)
            m_pending_create = TalkbackChannelOwner::None;
        const auto check = talkback_generation_on_response(m_nomination_generation, owner);
        freshness = check.freshness;
        m_nomination_generation = check.next;
    }
    if (owner == TalkbackChannelOwner::Session) {
        // F1 review-round fix (CRITICAL): session_stop() may have run while
        // THIS create was still outstanding -- see the m_session_create_
        // cancelled comment in the header. Check-and-clear it before doing
        // anything else with this response: a cancelled session must never
        // be adopted as live, invited, or queued as a stray (nothing drains
        // strays without a probe's driving thread running). Destroy the
        // channel immediately instead, using the same Begin/Add/Execute
        // sequence session_stop() itself uses, on this same thread -- this
        // callback runs on the command-loop thread, per the THREADING
        // comment above open_audio() below, exactly like session_stop().
        // Fix round 3, item B: routed through the shared
        // talkback_check_and_clear_cancelled() (src/talkback-channel-owner.h)
        // instead of hand-reading/clearing m_session_create_cancelled
        // directly -- the round-2 re-review found this exact site was still
        // a per-owner copy (the SETTER and the EXPIRE clearer were already
        // shared; this, the RESPONSE clearer, was not) and named it the same
        // defect class as N1. Session and Nomination now both go through
        // this one function.
        bool cancelled;
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            TalkbackCreateState state{m_pending_create, m_session_create_cancelled,
                                      m_nomination_create_cancelled};
            const auto result = talkback_check_and_clear_cancelled(state, owner);
            cancelled = result.cancelled;
            m_session_create_cancelled    = result.next.session_cancelled;
            m_nomination_create_cancelled = result.next.nomination_cancelled;
        }
        // Fix round 2: routed through the shared talkback_create_disposition()
        // (src/talkback-channel-owner.h) instead of branching on `cancelled`
        // directly -- the same function the Nomination branch below now
        // uses, so a future change to "what a cancelled response does" is
        // made once, tested once, and cannot diverge per owner the way
        // fix round 1's expiry logic did (N1).
        if (talkback_create_disposition(owner, cancelled) ==
            TalkbackCreateDisposition::DestroyCancelled) {
            if (error != TALKBACK_ERROR_OK || channelID == nullptr) {
                report_session("session_channel_cancelled",
                               R"("ok":true,"reason":"no_channel_to_destroy","error":)" +
                               std::to_string(static_cast<int>(error)));
                return;
            }
            uint32_t attempts = 0;
            const ZOOMSDK::SDKError e = destroy_channel_retrying(channelID, &attempts);
            report_session("session_channel_cancelled",
                           R"("channel":")" + json_escape(id) + R"(","code":)" +
                           std::to_string(static_cast<int>(e)) + R"(,"attempts":)" +
                           std::to_string(attempts));
            if (e != ZOOMSDK::SDKERR_SUCCESS) {
                report_session("session_channel_cancelled_abandoned",
                               R"("channel":")" + json_escape(id) + "\"");
            }
            return;
        }
        if (error != TALKBACK_ERROR_OK || channelID == nullptr) {
            report_session("session_channel", R"("ok":false,"error":)" +
                           std::to_string(static_cast<int>(error)));
            report_session_state(false, "create_failed");
            return;
        }
        // Minor review-round fix: keep the value from THIS lock scope
        // instead of re-locking immediately after just to read back what was
        // just written -- channel_copy is exactly m_session_channel_z at
        // this point, so a second lock/unlock pair bought nothing.
        std::basic_string<zchar_t> channel_copy;
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            m_session_channel_z.assign(channelID);
            m_session_channel = zchar_to_utf8(channelID);
            channel_copy = m_session_channel_z;
        }
        // Invite by NAME, resolved now: Zoom user ids are meeting-scoped, so
        // a stored id points at nobody after a rejoin and at the wrong face
        // once ids are recycled.
        m_session_user_id = resolve_participant(m_session_participant);
        if (m_session_user_id == 0) {
            report_session("session_invite", R"("ok":false,"reason":"no_participant_named",)"
                           R"("name":")" + json_escape(m_session_participant) + "\"");
            report_session_state(false, "no_participant_named");
            session_stop();
            return;
        }
        ZOOMSDK::SDKError e = m_ctrl->BeginBatchInviteUsers(channel_copy.c_str());
        if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->AddUserToInvite(m_session_user_id);
        if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->ExecuteBatchInviteUsers();
        report_session("session_invite", R"("user_id":)" + std::to_string(m_session_user_id) +
                       R"(,"code":)" + std::to_string(static_cast<int>(e)));
        if (e != ZOOMSDK::SDKERR_SUCCESS) {
            report_session_state(false, "invite_failed");
            session_stop();
            return;
        }
        m_ctrl->SetChannelBackgroundVolume(channel_copy.c_str(), 0.3f);
        m_session_live = true;
        report_session("session_live", R"("channel":")" + json_escape(m_session_channel) + "\"");
        report_session_state(true, "live");
        return;
    }
    if (owner == TalkbackChannelOwner::Nomination) {
        // Fix round 3, "expire-path double create" (Major, found by the
        // round-2 re-review); mechanism rebuilt in fix round 4 -- see
        // src/talkback-channel-owner.h's "Generation tracking" section and
        // m_nomination_generation's header comment. A create that merely
        // expired (nobody cancelled it) can still have its response arrive
        // after a fresh nomination re-armed this same owner and issued a
        // SECOND CreateChannel; Zoom's callback carries no id to tell the
        // two responses apart, so identity is tracked here instead.
        // `freshness` was computed in the arbiter scope above, in the same
        // lock as the claim.
        //
        // Only `Stale` -- the state positively saying "the outstanding
        // create was issued under a generation we have moved past" -- takes
        // the destroy path. `Current` AND `Unexpected` both fall through to
        // the ordinary handling below: fail OPEN. A wrongly-kept channel
        // costs one of the meeting's 16; a wrongly-destroyed one costs the
        // operator talkback mid-show, which is exactly what round 3's
        // fail-closed FIFO did every time its queue desynchronised.
        if (freshness == TalkbackResponseFreshness::Stale) {
            // This response belongs to a create the ladder gave up on.
            // Destroy the channel THIS RESPONSE NAMES if Zoom actually
            // created one -- it is genuinely orphaned on Zoom's side and
            // nothing else will ever clean it up -- and change nothing
            // else: not m_nomination_pending, not m_provisioned_channels,
            // not m_pending_create, and never nomination_create_next()
            // (which would issue a second create while one may still be in
            // flight, the one invariant the arbiter exists to hold).
            //
            // This branch must NOT re-claim the owner (round 3 did:
            // m_pending_create = Nomination). Stale means the state holds no
            // record of a current outstanding create, so claiming the
            // arbiter for one would wedge probe()/session_start()/nominate()
            // until the deadline expired -- and it would falsify the
            // "onCreateChannelResponse claims-then-clears regardless of what
            // the branch then does" invariant that
            // src/talkback-channel-owner.h documents as load-bearing.
            //
            // REACHABILITY -- read before "fixing" this branch. It is not
            // reachable today, and that is the design working, not a hole in
            // it. `owner == Nomination` implies the most recent
            // talkback_generation_issue() stamped `current`; the only two
            // sites that bump `current` (nominate(), the Nomination arm of
            // expire_stale_pending_create_locked()) can each run only with
            // the owner released. So `outstanding_generation != current`
            // cannot coexist with `owner == Nomination`. The counter's value
            // is therefore STRUCTURAL -- it makes the desynchronised state
            // that wedged fix round 3 inexpressible -- not behavioural: it
            // is not expected to catch anything at runtime.
            //
            // Do not "restore" reachability on the theory that a check which
            // never fires must be doing nothing. Making a late response
            // distinguishable from the current one is impossible with what
            // Zoom provides (onCreateChannelResponse carries no correlation
            // id), so any change that routes more responses here is
            // guessing, and guessing wrong here DESTROYS a channel the
            // ladder is legitimately waiting on. The deliberate disposition
            // for an ambiguous response is the opposite one -- fail open,
            // adopt it, let the extra response fall out down
            // channel_untracked. Keep this branch defensive and keep it
            // conservative: destroy only what the response names, change no
            // state, advance nothing.
            if (error == TALKBACK_ERROR_OK && channelID != nullptr) {
                uint32_t attempts = 0;
                const ZOOMSDK::SDKError e = destroy_channel_retrying(channelID, &attempts);
                report_nomination("channel_stale",
                                  R"("channel":")" + json_escape(id) + R"(","code":)" +
                                  std::to_string(static_cast<int>(e)) + R"(,"attempts":)" +
                                  std::to_string(attempts));
                if (e != ZOOMSDK::SDKERR_SUCCESS) {
                    report_nomination("channel_stale_destroy_abandoned",
                                      R"("channel":")" + json_escape(id) + "\"");
                }
            } else {
                report_nomination("channel_stale",
                                  R"("ok":true,"reason":"no_channel_to_destroy","error":)" +
                                  std::to_string(static_cast<int>(error)));
            }
            return;
        }

        // Fix round 1, C1 (CRITICAL): check-and-clear the cancellation flag
        // BEFORE doing anything else with this response -- see
        // nomination_reset()'s and m_nomination_create_cancelled's comments.
        // A cancelled create must never be adopted (pushed into
        // m_provisioned_channels), invited, or queued as a stray (nothing
        // drains m_stray_channels without a probe's driving thread running,
        // and queuing here would reproduce the exact wedge this fix exists
        // to close) -- destroy it immediately instead, same Begin/Add/
        // Execute sequence and retry bound as every other command-loop-
        // thread destroy in this file (session_stop(), the Session-cancelled
        // branch above, nomination_destroy_provisioned() below).
        //
        // Fix round 3, item B: routed through the shared
        // talkback_check_and_clear_cancelled() instead of hand-reading/
        // clearing m_nomination_create_cancelled directly -- see the
        // Session branch above's identical comment.
        bool cancelled;
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            TalkbackCreateState state{m_pending_create, m_session_create_cancelled,
                                      m_nomination_create_cancelled};
            const auto result = talkback_check_and_clear_cancelled(state, owner);
            cancelled = result.cancelled;
            m_session_create_cancelled    = result.next.session_cancelled;
            m_nomination_create_cancelled = result.next.nomination_cancelled;
        }
        // Fix round 2: shared talkback_create_disposition() -- see the
        // Session branch above's identical comment.
        if (talkback_create_disposition(owner, cancelled) ==
            TalkbackCreateDisposition::DestroyCancelled) {
            if (error != TALKBACK_ERROR_OK || channelID == nullptr) {
                report_nomination("channel_cancelled",
                                  R"("ok":true,"reason":"no_channel_to_destroy","error":)" +
                                  std::to_string(static_cast<int>(error)));
                return;
            }
            uint32_t attempts = 0;
            const ZOOMSDK::SDKError e = destroy_channel_retrying(channelID, &attempts);
            report_nomination("channel_cancelled",
                              R"("channel":")" + json_escape(id) + R"(","code":)" +
                              std::to_string(static_cast<int>(e)) + R"(,"attempts":)" +
                              std::to_string(attempts));
            if (e != ZOOMSDK::SDKERR_SUCCESS) {
                report_nomination("channel_cancelled_abandoned",
                                  R"("channel":")" + json_escape(id) + "\"");
            }
            return;
        }

        if (error != TALKBACK_ERROR_OK || channelID == nullptr) {
            report_nomination("channel_failed",
                              R"("error":)" + std::to_string(static_cast<int>(error)));
            // The rest of the queued plan can never be provisioned by THIS
            // ladder -- there is nothing to resume from, and issuing the
            // next queued CreateChannel now would be indistinguishable from
            // a fresh attempt that happens to reuse a stale queue. Forget
            // it; a later nominate() call starts clean.
            {
                std::lock_guard<std::mutex> lock(m_chan_mtx);
                m_nomination_pending.clear();
            }
            // Fix round 1, M2: destroy whatever this ladder already
            // provisioned before this failure -- without this, channel k
            // failing left channels 1..k-1 standing forever (consuming
            // budget, unreachable by any key) and nominate()'s
            // "already_provisioned" gate refused every retry for the rest
            // of the meeting.
            nomination_destroy_provisioned();
            return;
        }

        TalkbackPlannedChannel planned;
        bool have_planned;
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            have_planned = !m_nomination_pending.empty();
            if (have_planned) {
                planned = m_nomination_pending.front();
                m_nomination_pending.erase(m_nomination_pending.begin());
                TalkbackProvisionedChannel pc;
                pc.channel_id_z.assign(channelID);
                pc.channel_id = id;
                pc.members = planned.members;
                pc.is_all_talent = planned.is_all_talent;
                m_provisioned_channels.push_back(std::move(pc));
            }
        }
        if (!have_planned) {
            // Fix round 4: this is where FAILING OPEN lands, and it is a
            // genuinely reachable path -- not the "believed unreachable"
            // branch fix round 1's M5 called out, and not the narrow
            // invariant-violation branch fix round 3 rewrote it into (round
            // 3 argued the freshness check would catch the stale cases
            // first; with one scalar slot it deliberately does not, because
            // a late response for an abandoned create is indistinguishable
            // from the current one and is adopted rather than destroyed).
            // What arrives here is the extra response that adoption
            // produces: one more response than the plan has entries. The
            // channel Zoom created for it is real and belongs to nobody, so
            // destroy it. That -- one extra channel created and immediately
            // destroyed -- is the whole bounded cost of failing open, versus
            // the permanent feature wedge failing closed produced. Queuing it onto m_stray_channels (the
            // pre-fix-round-1 behaviour) would reproduce the exact
            // has_pending_work() wedge C1 was fixed for -- nothing drains
            // that queue without a probe's driving thread running.
            // report_nomination() is called only AFTER the lock above is
            // released, matching this file's "report outside m_chan_mtx"
            // discipline.
            report_nomination("channel_untracked", R"("channel":")" + json_escape(id) + "\"");
            uint32_t attempts = 0;
            const ZOOMSDK::SDKError e = destroy_channel_retrying(channelID, &attempts);
            report_nomination("channel_untracked_destroy",
                              R"("channel":")" + json_escape(id) + R"(","code":)" +
                              std::to_string(static_cast<int>(e)) + R"(,"attempts":)" +
                              std::to_string(attempts));
            if (e != ZOOMSDK::SDKERR_SUCCESS) {
                report_nomination("channel_untracked_destroy_abandoned",
                                  R"("channel":")" + json_escape(id) + "\"");
            }
            return;
        }

        report_nomination("channel_created",
                          R"("channel":")" + json_escape(id) + R"(","is_all_talent":)" +
                          (planned.is_all_talent ? "true" : "false") + R"(,"members":)" +
                          std::to_string(planned.members.size()));

        // Invite by NAME, resolved now: Zoom user ids are meeting-scoped, so
        // a stored id would point at nobody after a rejoin and at the wrong
        // face once ids are recycled -- same rule the probe and the session
        // already follow.
        const std::basic_string<zchar_t> channel_copy(channelID);
        for (const auto &name : planned.members)
            invite_nominee(channel_copy, id, name);

        bool more;
        std::size_t provisioned_count = 0;
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            more = !m_nomination_pending.empty();
            if (!more) provisioned_count = m_provisioned_channels.size();
        }
        if (more)
            nomination_create_next();
        else
            report_nomination("nominate_done",
                              R"("channels":)" + std::to_string(provisioned_count));
        return;
    }
    // owner == Probe was already cleared above; owner == None means nothing
    // was outstanding (a stray/duplicate, handled by id-comparison below --
    // never by m_pending_create, which by definition has no opinion here).

    if (!channelID) {
        // F1 review-round fix: guard before ANY comparison or assignment
        // touches channelID as a raw pointer -- std::basic_string's
        // operator== / assign() against a null zchar_t* is
        // char_traits::length(nullptr), an access violation. zchar_to_utf8
        // above already null-guards internally (that's the precedent this
        // follows), but the `m_channel_id_z == channelID` comparison and
        // the `m_stray_channels.emplace_back(channelID)` construction a few
        // lines down do not, and a null id is reachable here on any error
        // code, not only TALKBACK_ERROR_OK. Report it distinctly -- we want
        // to KNOW the SDK did this -- rather than crash or silently no-op.
        report("create_channel_response_null_channel",
               R"("error":)" + std::to_string(static_cast<int>(error)));
        if (m_phase.load(std::memory_order_acquire) == Phase::AwaitingChannel) {
            // This was the rung we were waiting on and it came back with
            // nothing usable -- same disposition as any other
            // create_channel failure. If it was a stray/duplicate for a
            // channel that isn't the live one, there is no id to queue for
            // cleanup, so there is nothing else to do.
            m_phase.store(Phase::Done, std::memory_order_release);
        }
        return;
    }

    if (m_phase.load(std::memory_order_acquire) != Phase::AwaitingChannel) {
        // Not the callback we're waiting on -- either a stray/duplicate, or
        // (now that tick() times AwaitingChannel out) a genuinely late
        // response that arrived after we already gave up on this rung and
        // moved on, in which case a real channel exists that our own ladder
        // no longer tracks.
        //
        // THIS path queues rather than destroying, and fix round 4 narrowed
        // the reason to the one that is actually true. The old wording --
        // "this callback ... must never call BeginBatchDestroyChannels/etc
        // directly, even for cleanup" -- was contradicted by the callback
        // itself: four of its branches above destroy directly, via
        // destroy_channel_retrying(). The distinction is WHICH THREAD MAY BE
        // MID-BATCH. Everything reachable here is reachable while the
        // probe's driving thread is alive and may be inside its own
        // Begin/Add/Execute (this is the probe's phase machine; we are here
        // precisely because m_phase is not AwaitingChannel), so destroying
        // from this branch could interleave with it and merge or corrupt
        // batches. The Nomination/Session branches above are the paths where
        // no probe ladder is running -- see the inventory and the recorded
        // residual gap at the top of tick(). So: here, queue and never call.
        // The comparison against m_channel_id_z below is exactly
        // the access that must go through m_chan_mtx -- reachable while
        // phase is Idle/Done, i.e. while a fresh probe() is allowed to be
        // clearing/reassigning that member on another thread right now; see
        // the m_chan_mtx comment in the header.
        //
        // R2 review-round fix: a channel we OWN -- the probe's live one OR
        // the session's -- is never a stray, no matter what m_pending_create
        // said at routing time above. This is the fix for a redelivered
        // response: the SDK can and does redeliver onCreateChannelResponse
        // (that's what the *_duplicate / *_stray handling in this whole
        // block is for), and a redelivery can arrive long after the request
        // that caused it was already resolved -- at which point
        // m_pending_create has moved on to None or to the OTHER subsystem,
        // so the arbiter above no longer has an opinion about it. Comparing
        // ONLY against m_channel_id_z (as this branch used to) meant a
        // redelivered SESSION response landed here, failed that comparison,
        // and got queued as a stray -- so tick() destroyed the LIVE session
        // channel out from under drain_audio() on the probe's own thread.
        // That was the exact race this whole task exists to make
        // structurally impossible; this id check is what actually closes it
        // for the redelivery case (R1's mutual exclusion above closes the
        // DIFFERENT hazard of a concurrent request, not this one).
        if (error == TALKBACK_ERROR_OK) {
            bool is_probe_channel;
            bool is_session_channel;
            {
                std::lock_guard<std::mutex> lock(m_chan_mtx);
                is_probe_channel   = (m_channel_id_z == channelID);
                is_session_channel = !is_probe_channel &&
                                     (m_session_channel_z == channelID);
                if (!is_probe_channel && !is_session_channel) {
                    // A genuinely different, untracked channel now exists.
                    // Queue it (still under the lock, so the check and the
                    // push are one atomic decision); drain_stray_channels()
                    // (called from tick()) owns actually destroying it.
                    m_stray_channels.emplace_back(channelID);
                }
            }
            if (is_session_channel) {
                // Matches the session's live channel -- a duplicate/
                // redelivered callback for the create the session already
                // finished handling. Queuing it for destroy would tear down
                // the channel drain_audio() is actively sending on. Report
                // and do NOTHING else.
                report("create_channel_response_session_duplicate",
                       R"("channel":")" + json_escape(id) + "\"");
            } else if (is_probe_channel) {
                // This id matches OUR live channel -- a duplicate/redelivered
                // callback for the channel the ladder already moved past
                // AwaitingChannel with (e.g. now mid-invite or mid-send).
                // Queuing it for destroy would tear down a channel a running
                // probe still depends on. Report and do NOTHING else.
                report("create_channel_response_duplicate",
                       R"("channel":")" + json_escape(id) + "\"");
            } else {
                report("create_channel_response_stray",
                       R"("channel":")" + json_escape(id) + R"(","queued":true)");
            }
        }
        return;
    }
    if (error != TALKBACK_ERROR_OK) {
        m_phase.store(Phase::Done, std::memory_order_release);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        m_channel_id = id;              // UTF-8, reporting only
        m_channel_id_z.assign(channelID); // SDK identifier, verbatim -- see header
    }

    // RUNG 4: invite one participant, resolved from a NAME. A raw id would
    // point at nobody after a rejoin and at the wrong person once ids are
    // recycled -- the defect the Companion module fixed in v0.1.44.
    m_participant_id = resolve_participant(m_participant_name);
    if (m_participant_id == 0) {
        report("invite", R"("error":"no_participant_named","name":")" +
               json_escape(m_participant_name) + "\"");
        m_phase.store(Phase::Destroying, std::memory_order_release);
        return;
    }

    std::basic_string<zchar_t> channel_copy;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        channel_copy = m_channel_id_z;
    }
    ZOOMSDK::SDKError e = m_ctrl->BeginBatchInviteUsers(channel_copy.c_str());
    if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->AddUserToInvite(m_participant_id);
    if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->ExecuteBatchInviteUsers();
    report("invite", R"("user_id":)" + std::to_string(m_participant_id) +
           R"(,"code":)" + std::to_string(static_cast<int>(e)));
    if (e != ZOOMSDK::SDKERR_SUCCESS) {
        m_phase.store(Phase::Destroying, std::memory_order_release);
        return;
    }
    m_phase_deadline.store(
        (std::chrono::steady_clock::now() + kAwaitTimeout).time_since_epoch().count(),
        std::memory_order_release);
    m_phase.store(Phase::AwaitingInvite, std::memory_order_release);
}

void EngineTalkback::onDestroyChannelResponse(const zchar_t *channelID, TalkbackError error)
{
    report("destroyed",
           R"("channel":")" + json_escape(zchar_to_utf8(channelID)) +
           R"(","error":)" + std::to_string(static_cast<int>(error)));
}

void EngineTalkback::onChannelUserJoinResponse(const zchar_t *channelID,
                                               unsigned int userID, TalkbackError error)
{
    report("invite_response",
           R"("channel":")" + json_escape(zchar_to_utf8(channelID)) +
           R"(","user_id":)" + std::to_string(userID) +
           R"(,"error":)" + std::to_string(static_cast<int>(error)));
    if (m_phase.load(std::memory_order_acquire) != Phase::AwaitingInvite) return;

    if (!channelID) {
        // F1 review-round fix: this line is reached for EVERY error code,
        // including TALKBACK_ERROR_NOPERMISSION -- exactly the error this
        // probe exists to exercise -- and the SDK can hand back a null
        // channelID on that path. Without this guard, `channel_copy !=
        // channelID` below is std::basic_string comparing against a null
        // zchar_t*, i.e. char_traits::length(nullptr): an access violation
        // that replaces our diagnostic with a crash-recovery event at
        // exactly the moment we're trying to learn something. Report it
        // distinctly (we want to KNOW the SDK did that) and destroy the
        // channel we know we created -- m_channel_id_z is still valid; only
        // this callback's echo of it is null.
        report("invite_response_null_channel",
               R"("user_id":)" + std::to_string(userID) +
               R"(,"error":)" + std::to_string(static_cast<int>(error)));
        m_phase.store(Phase::Destroying, std::memory_order_release);
        return;
    }

    // Copy both channel-id representations out under the lock before using
    // either -- same discipline as everywhere else m_channel_id_z is
    // touched, see the m_chan_mtx comment in the header.
    std::basic_string<zchar_t> channel_copy;
    std::string channel_copy_utf8;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        channel_copy = m_channel_id_z;
        channel_copy_utf8 = m_channel_id;
    }

    // Phase alone can't tell "our invite landed" from "some other invite
    // response landed while we happened to be in this phase" -- correlate
    // the callback to the exact channel and user we are waiting on.
    if (channel_copy != channelID || userID != m_participant_id) {
        report("invite_response_mismatch",
               R"("expected_channel":")" + json_escape(channel_copy_utf8) +
               R"(","expected_user":)" + std::to_string(m_participant_id));
        return;
    }
    if (error != TALKBACK_ERROR_OK) {
        m_phase.store(Phase::Destroying, std::memory_order_release);
        return;
    }

    // Duck the main meeting for the person being spoken to, so the tone is
    // unambiguous rather than competing with meeting audio.
    const ZOOMSDK::SDKError vol =
        m_ctrl->SetChannelBackgroundVolume(channel_copy.c_str(), 0.3f);
    report("background_volume", R"("code":)" +
           std::to_string(static_cast<int>(vol)));

    m_tone_index = 0;
    m_buffers_sent = 0;
    // Unlike m_phase, m_channel_id_z needs no ordering argument here: tick()
    // will re-read it through m_chan_mtx itself once it observes Sending
    // (see the m_chan_mtx comment in the header), so this store only needs
    // to publish the phase transition, not the string.
    m_phase.store(Phase::Sending, std::memory_order_release);   // tick() takes it from here
}

void EngineTalkback::onChannelUserLeaveResponse(const zchar_t *, unsigned int, TalkbackError) {}
void EngineTalkback::onJoinTalkbackChannel(unsigned int) {}
void EngineTalkback::onLeaveTalkbackChannel(unsigned int) {}
void EngineTalkback::onInviterAudioLevel(unsigned int, unsigned int) {}

// ── Pre-provisioned channels (Task 2, 2026-08-25) ───────────────────────────
//
// Moves channel creation from key time to nomination time -- see nominate()'s
// header declaration comment for the live-measured reason (buffers discarded
// on every key press while the create+invite round trip was in flight).
bool EngineTalkback::nominate(ZOOMSDK::IMeetingService *svc,
                              const std::vector<std::string> &nominees)
{
    // R1-style mutual exclusion, same reasoning as probe()'s and
    // session_start()'s matching guards: nominate() is about to reassign
    // m_svc/m_ctrl, the exact fields the probe's driving thread dereferences
    // for as long as has_pending_work() is true, and a live session already
    // holds its own channel through those same fields. This is the
    // plan-level ruling that "nomination must not break [probe/session
    // mutual exclusion]" -- refusing is correct here for the same reason it
    // is correct in the other two: there is nothing sensible to queue, and
    // queuing would let nomination's CreateChannel arrive while the OTHER
    // subsystem's response is still in flight, the exact ambiguity the
    // arbiter exists to remove.
    if (m_session_live) {
        report_nomination("nominate", R"("ok":false,"reason":"session_live")");
        return false;
    }
    if (has_pending_work()) {
        report_nomination("nominate", R"("ok":false,"reason":"probe_busy")");
        return false;
    }
    if (!svc) {
        report_nomination("nominate", R"("ok":false,"reason":"not_in_meeting")");
        return false;
    }
    m_svc  = svc;
    m_ctrl = m_svc->GetMeetingTalkbackController();
    if (!m_ctrl) {
        report_nomination("nominate", R"("ok":false,"reason":"no_controller")");
        return false;
    }
    if (!m_ctrl->IsMeetingSupportTalkBack()) {
        report_nomination("nominate", R"("ok":false,"reason":"not_supported")");
        return false;
    }
    m_ctrl->SetEvent(this);

    // Fix round 4 (Major, found by the round-3 re-review): run the lazy
    // self-heal BEFORE the already_provisioned gate, not after it. The gate
    // used to sit at the very top of this function and return, while the
    // only call to expire_stale_pending_create_locked() on this path is
    // inside nomination_create_next(), hundreds of lines later -- so a
    // ladder whose channel-k create response was swallowed (no Leave, no
    // cancellation: the SDK simply never called back) stayed refused with
    // "already_provisioned" until some unrelated probe()/session_start()
    // happened to run the expiry, and stayed refused EVEN THEN, because the
    // expiry cleared only m_nomination_pending and left channels 1..k-1 in
    // m_provisioned_channels forever. One transient SDK hiccup therefore
    // cost the operator re-nomination for the rest of the show, recoverable
    // only by a Leave. handle_expired_create() now destroys that partial
    // set (see its comment), so the gate below sees a clean table.
    //
    // Deliberately placed AFTER m_ctrl is resolved above: the destroy it
    // may perform needs a controller for the CURRENT meeting, and reaching
    // for the previous ladder's m_ctrl would be exactly the stale-pointer
    // shape this file's mutual-exclusion guards exist to avoid. That is why
    // the gate moved down here rather than the expiry moving up. probe()
    // and session_start() order their own expiry the same way.
    TalkbackChannelOwner expired_owner;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        expired_owner = expire_stale_pending_create_locked();
    }
    handle_expired_create(expired_owner);

    bool already_provisioned;
    {
        // No un-nominate in this task: a second nominate() call while an
        // earlier one's channels are still standing would leak the earlier
        // set (nothing here destroys them) rather than reconfigure
        // anything. Refuse loudly instead of silently orphaning live Zoom
        // channels -- Task 3, which reworks the session path against this
        // table, owns deciding what a re-nomination should do. This refuses
        // a genuinely LIVE nomination; a DEAD one has just been cleaned up
        // by the self-heal above.
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        already_provisioned = !m_provisioned_channels.empty() || !m_nomination_pending.empty();
    }
    if (already_provisioned) {
        // Fix round 1: found alongside m1 while re-checking the file for the
        // same "report while holding m_chan_mtx" violation the review's m1
        // finding cited at the (now rewritten) channel_untracked branch --
        // this call site had the identical shape and was not called out by
        // name, but the discipline it breaks is the same one, so it gets
        // the same fix: read the decision out under the lock, release,
        // THEN report.
        report_nomination("nominate", R"("ok":false,"reason":"already_provisioned")");
        return false;
    }

    const TalkbackPlan plan = talkback_plan(nominees);

    // Gates are surfaced, never swallowed: name every nominee the planner
    // could not fully reach BEFORE creating a single channel, not only if
    // something later fails. See src/talkback-plan.h for what each list
    // means.
    for (const auto &name : plan.uncovered_private)
        report_nomination("uncovered_private", R"("name":")" + json_escape(name) + "\"");
    for (const auto &name : plan.unreachable)
        report_nomination("unreachable", R"("name":")" + json_escape(name) + "\"");
    report_nomination("plan", R"("channels":)" + std::to_string(plan.channels.size()) +
                      R"(,"all_talent_complete":)" +
                      (plan.all_talent_complete ? "true" : "false"));

    if (plan.channels.empty()) {
        // Nothing to provision (e.g. an empty nominee list) -- not a
        // failure, just a plan with no channels. Report completion so a
        // caller waiting on "nominate_done" doesn't wait forever.
        report_nomination("nominate_done", R"("channels":0)");
        return true;
    }

    // Fix round 2, N1: call nomination_create_next() -- which does its own
    // gate-check-and-expire via expire_stale_pending_create_locked() -- and
    // only assign THIS plan into m_nomination_pending AFTER it returns,
    // never before. The original order (assign, then call
    // nomination_create_next()) meant that if a stale Nomination create
    // from an EARLIER ladder happened to expire during THIS call's own gate
    // check -- exactly what happens when a create outlives Leave() and its
    // response never arrives at all -- expire_stale_pending_create_locked()
    // would clear m_nomination_pending to forget that earlier ladder's
    // queue, but by then this call had already overwritten it with the NEW
    // plan two lines earlier, so the wipe hit the wrong plan: this
    // nomination's own channels, not the stale one's. Checking the gate
    // first guarantees any queue expire_...() clears here is genuinely a
    // leftover from an earlier ladder (or, on the common path, an
    // already-empty vector -- nomination_reset() clears it eagerly at
    // Leave/quit) and never the plan this call is about to run.
    // nomination_create_next() does not read m_nomination_pending's
    // CONTENT to decide whether to issue CreateChannel -- only whether the
    // arbiter gate is open -- so calling it before the assignment is safe;
    // the response for the create it issues cannot be delivered until this
    // function returns and control goes back to the command loop / message
    // pump, so the queue is populated well before anything could consult it.
    //
    // Fix round 3, "expire-path double create": bump the nomination
    // generation HERE, before issuing this ladder's first create -- see
    // src/talkback-channel-owner.h's "Generation tracking" section. A fresh
    // ladder must not be confused with an older one whose create may still
    // be floating around unresolved.
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        m_nomination_generation = talkback_generation_bump(m_nomination_generation);
    }
    if (!nomination_create_next())
        return false;

    std::lock_guard<std::mutex> lock(m_chan_mtx);
    m_nomination_pending.assign(plan.channels.begin(), plan.channels.end());
    return true;
}

bool EngineTalkback::nomination_create_next()
{
    // Fix round 1, m6: converts the "onCreateChannelResponse runs on the
    // command-loop thread" premise -- newly load-bearing here, since this
    // is the first CreateChannel call site reached from inside a callback
    // dispatch rather than only from a top-level pipe-command handler --
    // from an assumption into evidence. See the header comment.
    assert_command_loop_thread("nomination_create_next");

    // Gate through the same arbiter probe()/session_start() use -- copy the
    // decision out under the lock, release, THEN call the SDK, same
    // discipline as every other m_chan_mtx access in this file.
    bool create_gate_ok;
    TalkbackChannelOwner expired_owner;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        expired_owner = expire_stale_pending_create_locked();
        create_gate_ok = talkback_may_request_create(m_pending_create);
    }
    handle_expired_create(expired_owner);
    if (!create_gate_ok) {
        // The probe or the session is mid-create. Refuse rather than wait:
        // there is no sensible way to hold this queue open across an
        // unrelated create's whole round trip, and the plan-level ruling
        // already says nomination must not block on the arbiter.
        report_nomination("nominate", R"("ok":false,"reason":"create_busy")");
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            m_nomination_pending.clear();
        }
        // Fix round 1, M2: destroy whatever was already provisioned before
        // this refusal -- see nomination_destroy_provisioned()'s comment.
        // Without this a plan that got as far as channel k before hitting a
        // busy arbiter left 1..k-1 standing forever with no way to retry.
        nomination_destroy_provisioned();
        return false;
    }

    const ZOOMSDK::SDKError e = m_ctrl->CreateChannel(1);
    report_nomination("create_channel", R"("code":)" + std::to_string(static_cast<int>(e)));
    if (e != ZOOMSDK::SDKERR_SUCCESS) {
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            m_nomination_pending.clear();
        }
        nomination_destroy_provisioned();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        m_pending_create = TalkbackChannelOwner::Nomination;
        m_nomination_create_deadline.store(
            (std::chrono::steady_clock::now() + kAwaitTimeout).time_since_epoch().count(),
            std::memory_order_release);
        // Fix round 3: stamp this create with the CURRENT generation --
        // see src/talkback-channel-owner.h's "Generation tracking" section.
        // Fix round 4: this OVERWRITES the one outstanding slot rather than
        // pushing onto a queue. Anything already stamped there belonged to
        // a create the ladder abandoned, so there is nothing to reconcile
        // -- and nothing that can desynchronise.
        m_nomination_generation = talkback_generation_issue(m_nomination_generation);
    }
    return true;
}

void EngineTalkback::assert_command_loop_thread(const char *where) const
{
    // Fix round 1, m6: records the thread id of the FIRST call (which is
    // always genuinely the command loop -- nomination_create_next()'s only
    // entry points are nominate(), called directly from main.cpp's command
    // dispatch, and onCreateChannelResponse, which this file already argues
    // runs there too) and reports every later call from a different thread.
    // Does not gate or refuse anything: a wrong assumption here is a race to
    // diagnose, not one this function can safely correct by itself, and the
    // arbiter's own correctness does not depend on this check -- it depends
    // on the premise actually being true, which is exactly what this makes
    // verifiable instead of merely stated.
    static const std::thread::id command_loop_id = std::this_thread::get_id();
    if (std::this_thread::get_id() != command_loop_id) {
        report_nomination("thread_assert_failed", R"("where":")" + std::string(where) + "\"");
    }
}

ZOOMSDK::SDKError EngineTalkback::destroy_channel_retrying(const zchar_t *channelID,
                                                            uint32_t *attempts)
{
    // Fix round 3: the bounded Begin/Add/Execute retry, extracted from four
    // identical hand-copies (the Nomination cancelled branch, the untracked
    // branch, nomination_destroy_provisioned()'s loop, and the new
    // stale-response branch below) -- see the header comment on why. Never
    // called with m_chan_mtx held; m_ctrl is dereferenced here.
    ZOOMSDK::SDKError e = ZOOMSDK::SDKERR_UNKNOWN;
    uint32_t attempt = 0;
    for (; attempt < kMaxDestroyAttempts; ++attempt) {
        e = m_ctrl->BeginBatchDestroyChannels();
        if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->AddChannelToDestroy(channelID);
        if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->ExecuteBatchDestroyChannels();
        if (e == ZOOMSDK::SDKERR_SUCCESS) break;
    }
    if (attempts) *attempts = attempt + 1;
    return e;
}

void EngineTalkback::nomination_destroy_provisioned()
{
    // Fix round 1, M2. Copy the ids out and clear the table under the lock,
    // release, THEN call the SDK -- same discipline as every other
    // m_chan_mtx access in this file. m_ctrl should never be null here: it
    // is set once in nominate() before any channel is created and nothing
    // in this class nulls it mid-ladder. Guarded anyway (report and leave
    // the table alone, rather than clear it and then have nothing to
    // destroy with) so a future change that violates that invariant fails
    // loudly instead of silently dropping the record of live channels.
    if (!m_ctrl) {
        report_nomination("channels_destroy_skipped", R"("reason":"no_controller")");
        return;
    }
    std::vector<std::basic_string<zchar_t>> ids;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        ids.reserve(m_provisioned_channels.size());
        for (auto &pc : m_provisioned_channels) ids.push_back(std::move(pc.channel_id_z));
        m_provisioned_channels.clear();
    }
    for (const auto &channel_id_z : ids) {
        const std::string channel_id_utf8 = zchar_to_utf8(channel_id_z.c_str());
        uint32_t attempts = 0;
        const ZOOMSDK::SDKError e = destroy_channel_retrying(channel_id_z.c_str(), &attempts);
        report_nomination("channel_destroyed",
                          R"("channel":")" + json_escape(channel_id_utf8) + R"(","code":)" +
                          std::to_string(static_cast<int>(e)) + R"(,"attempts":)" +
                          std::to_string(attempts));
        if (e != ZOOMSDK::SDKERR_SUCCESS) {
            report_nomination("channel_destroy_abandoned",
                              R"("channel":")" + json_escape(channel_id_utf8) + "\"");
        }
    }
}

void EngineTalkback::invite_nominee(const std::basic_string<zchar_t> &channel_id_z,
                                    const std::string &channel_id_utf8,
                                    const std::string &name)
{
    // resolve_participant() reports IsSupportTalkback() itself (the
    // per-user gate) whenever it finds a match -- see its own comment. A
    // nominee not currently in the meeting resolves to 0: reported and
    // skipped here, not an error -- a future roster-driven re-resolution
    // (Task 4) is what is expected to pick them up once they join.
    const unsigned int uid = resolve_participant(name, ReportSink::Nomination);
    if (uid == 0) {
        report_nomination("member_not_in_meeting",
                          R"("name":")" + json_escape(name) + R"(","channel":")" +
                          json_escape(channel_id_utf8) + "\"");
        return;
    }
    ZOOMSDK::SDKError e = m_ctrl->BeginBatchInviteUsers(channel_id_z.c_str());
    if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->AddUserToInvite(uid);
    if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->ExecuteBatchInviteUsers();
    report_nomination("invite",
                      R"("name":")" + json_escape(name) + R"(","user_id":)" +
                      std::to_string(uid) + R"(,"channel":")" +
                      json_escape(channel_id_utf8) + R"(","code":)" +
                      std::to_string(static_cast<int>(e)));
}

void EngineTalkback::nomination_reset()
{
    // Fix round 1, C1 (CRITICAL, previously mis-diagnosed as this
    // implementer's own "concern 2" and rated Minor): this used to clear
    // m_pending_create unconditionally when it was Nomination and return --
    // but if a Nomination-owned CreateChannel was still outstanding (this is
    // exactly the branch that runs then: main.cpp's Leave path calls this
    // BEFORE meeting_svc->Leave(), and a nominate() issued moments earlier
    // may not have its response pumped yet), the CreateChannel had already
    // gone to Zoom. Clearing the arbiter's record of it here does not cancel
    // that request: when the response eventually arrives,
    // talkback_claim_create(None) returns None, the id matches no tracked
    // channel, and it is queued onto m_stray_channels -- which nothing
    // drains without a probe's driving thread running
    // (drain_stray_channels() has exactly one caller, tick(), which has
    // exactly one caller, that thread). has_pending_work() then reads true
    // forever, and it gates the top of BOTH nominate() and session_start()
    // -- so this one sequence (nominate, then leave before the create
    // response is pumped) permanently disabled the whole talkback feature,
    // citing a probe that never ran. This is precisely the bug F1 already
    // fixed for Session (see m_session_create_cancelled's header comment);
    // Nomination had reintroduced the unfixed version.
    //
    // Fix: leave m_pending_create AS Nomination (so the eventual response is
    // still routed to the Nomination branch in onCreateChannelResponse, not
    // lost to "owner == None") and set m_nomination_create_cancelled
    // instead; that branch destroys the channel immediately on arrival
    // rather than adopting it or queuing it as a stray. The nomination
    // table/queue are still cleared unconditionally here -- that part is
    // genuinely bookkeeping-only (provisioned channels and their membership
    // are meeting-scoped, so once the meeting is gone there is nothing left
    // on Zoom's side to select) and does not depend on whether a create was
    // outstanding.
    //
    // Fix round 2: routed through the shared talkback_cancel()
    // (src/talkback-channel-owner.h) instead of writing
    // m_nomination_create_cancelled directly -- the same function
    // session_stop()'s early branch now calls for Session, so the two
    // owners' cancellation logic can no longer diverge the way their
    // EXPIRY logic did (N1).
    std::lock_guard<std::mutex> lock(m_chan_mtx);
    TalkbackCreateState state{m_pending_create, m_session_create_cancelled,
                              m_nomination_create_cancelled};
    state = talkback_cancel(state, TalkbackChannelOwner::Nomination);
    m_session_create_cancelled    = state.session_cancelled;
    m_nomination_create_cancelled = state.nomination_cancelled;
    m_nomination_pending.clear();
    m_provisioned_channels.clear();
}

// ── Talkback audio path (Milestone 2) ───────────────────────────────────────
//
// The probe above (Milestone 1) only asked "can this account open a channel
// and put audio in it". This is the consuming half: the plugin's OBS tap
// writes director audio into a ring (src/talkback-ring.h), and this class
// maps that region, drains it, and forwards every buffer to Zoom via
// SendAudioDataToChannel on whatever channel this class already holds.
//
// THREADING: open_audio/drain_audio/close_audio are called ONLY from
// engine/src/main.cpp's command loop, which on Windows is ALSO the SDK's
// message-pump thread (see ipc_read_line_with_message_pump there). That is
// deliberate: every SDK call stays on the thread the SDK already uses. This
// is a correction of the Milestone 1 probe, which introduced a separate
// driving thread (still used by tick(), an accepted narrower exception) and
// was the first code in this engine to call SDK APIs off the pump. There is
// no need to repeat that here: the ring already decouples the OBS audio
// thread (the writer) from us, so nothing on this path needs to run
// off-thread to stay responsive -- it only needs to run on the thread the
// SDK expects.
bool EngineTalkback::open_audio(const std::string &region_name,
                                uint32_t sample_rate, uint16_t channels)
{
    close_audio();
    if (!shm_region_open_readwrite(
            m_audio_region, region_name,
            shm_audio_region_bytes(kTalkbackSlotBytes))) {
        report_session("audio_open", R"("ok":false,"reason":"map_failed","region":")" +
                       json_escape(region_name) + "\"");
        report_session_state(false, "map_failed");
        return false;
    }

    auto *hdr = static_cast<ShmAudioHeader *>(m_audio_region.ptr);

    // F3 review-round fix: the ring's physical layout (slot_count,
    // slot_bytes) is whatever the WRITER laid down when it created the
    // region, but shm_audio_region_bytes() above sized OUR mapping from the
    // READER's own kTalkbackSlotBytes constant. shm_audio_slot_offset(*hdr,
    // index) multiplies by the WRITER's hdr->slot_bytes -- so a half-applied
    // install (CLAUDE.md: "a DLL-only copy silently half-applies" is routine
    // here) pairing an old engine against a plugin built with a larger
    // kTalkbackSlotBytes would walk that offset past the end of a mapping
    // sized for the smaller constant: an access violation, not a
    // diagnosable error. The header carries no version field, so this check
    // is the only thing standing between that version skew and a crash.
    // Requiring the exact slot_count also rules out slot_count == 0, which
    // would otherwise make every `% hdr->slot_count` in the drain path
    // (talkback-ring.h) a division by zero.
    if (hdr->slot_count != kAudioRingSlots || hdr->slot_bytes != kTalkbackSlotBytes) {
        report_session("audio_open",
                       R"("ok":false,"reason":"layout_mismatch","slot_count":)" +
                       std::to_string(hdr->slot_count) + R"(,"expected_slot_count":)" +
                       std::to_string(kAudioRingSlots) + R"(,"slot_bytes":)" +
                       std::to_string(hdr->slot_bytes) + R"(,"expected_slot_bytes":)" +
                       std::to_string(kTalkbackSlotBytes) + R"(,"region":")" +
                       json_escape(region_name) + "\"");
        report_session_state(false, "layout_mismatch");
        // F5 review-round fix: this used to shm_region_destroy()/reset the
        // mapping immediately on every rejection below. The writer
        // (TalkbackTap) may already be publishing into this ring by the
        // time a rejection is discovered -- the pipe round-trip guarantees
        // talkback_open was sent, and the writer's capture callback
        // attached, before this response is even processed -- and this ring
        // has NO keepalive (see the F1 review-round comment above on
        // audio_ring_reader_abandon(), and the comment in drain_audio()).
        // Unmapping here nulls m_audio_region.ptr, which makes every
        // subsequent drain_audio() call for this rejected session hit its
        // FIRST bail (region not mapped at all) instead of its SECOND one
        // (mapped but not open) -- and only the second one abandons the
        // notify flag. Leaving the mapping open keeps that second bail
        // reachable, so a buffer published in the race window still gets the
        // flag handed back to the writer instead of orphaned forever.
        // close_audio() (called at the top of the NEXT open_audio(), or by
        // an explicit talkback_close) is what actually releases this
        // mapping -- see its F5 review-round fix.
        return false;
    }

    // F7 review-round fix: rate/channels used to come straight from the
    // pipe's talkback_open JSON, unvalidated -- json_uint() returns 0 for a
    // missing key, so a malformed talkback_open would send sampleRate = 0
    // straight to Zoom. The ring header is the AUTHORITATIVE copy: it is
    // written by talkback_ring_init() in the same call that lays out the
    // region (src/talkback-tap.cpp's open()), which already validated rate
    // and channel count before creating the region at all. Treat the pipe's
    // values as a cross-check against that authority, not a second source of
    // truth -- a mismatch means the plugin and the talkback_open command
    // disagree about which region/session this is, which is exactly the
    // kind of stale-handshake bug that must refuse loudly rather than send
    // audio at a value nobody actually asked for.
    const uint32_t hdr_rate = hdr->sample_rate;
    const uint16_t hdr_channels = static_cast<uint16_t>(hdr->channels);

    if (!talkback_pcm_rate_supported(hdr_rate)) {
        report_session("audio_open",
                       R"("ok":false,"reason":"unsupported_rate","rate":)" +
                       std::to_string(hdr_rate) + R"(,"region":")" +
                       json_escape(region_name) + "\"");
        report_session_state(false, "unsupported_rate");
        // F5 review-round fix: leave the mapping open -- see the comment on
        // the layout_mismatch rejection above.
        return false;
    }
    if (hdr_channels != 1 && hdr_channels != 2) {
        report_session("audio_open",
                       R"("ok":false,"reason":"unsupported_channels","channels":)" +
                       std::to_string(hdr_channels) + R"(,"region":")" +
                       json_escape(region_name) + "\"");
        report_session_state(false, "unsupported_channels");
        // F5 review-round fix: leave the mapping open -- see the comment on
        // the layout_mismatch rejection above.
        return false;
    }
    if (hdr_rate != sample_rate || hdr_channels != channels) {
        report_session("audio_open",
                       R"("ok":false,"reason":"pipe_header_mismatch","pipe_rate":)" +
                       std::to_string(sample_rate) + R"(,"header_rate":)" +
                       std::to_string(hdr_rate) + R"(,"pipe_channels":)" +
                       std::to_string(channels) + R"(,"header_channels":)" +
                       std::to_string(hdr_channels) + R"(,"region":")" +
                       json_escape(region_name) + "\"");
        report_session_state(false, "pipe_header_mismatch");
        // F5 review-round fix: leave the mapping open -- see the comment on
        // the layout_mismatch rejection above.
        return false;
    }

    m_audio_region_name = region_name;
    m_audio_rate        = hdr_rate;
    m_audio_channels    = hdr_channels;
    // Start at the writer's CURRENT index, not 0: buffers published before we
    // mapped are stale by definition, and replaying them would put a burst of
    // old audio in the channel the moment a key opens.
    m_audio_read_index = hdr->write_index;

    // F1 review-round fix (CRITICAL): the tap's capture callback attaches
    // and starts publishing as soon as TalkbackTap::open() runs on the
    // plugin side, which is BEFORE this engine ever handles talkback_open --
    // the pipe round-trip guarantees that ordering. audio_ring_notify_after_
    // publish() (engine-ipc.h) only sends an event on the empty->non-empty
    // edge, so by the time we map this region `notify` is very likely
    // already 1 from a buffer published in that window. Unlike the MAIN
    // audio ring, this ring has NO keepalive (no ~2.5s / 250-buffer
    // self-heal) to fall back on: if we leave notify=1 here, the writer
    // never re-notifies (it only notifies on an edge it never sees again
    // while the ring keeps being non-empty), drain_audio() is never called,
    // and the ring silently fills and laps for the rest of the region's
    // life -- the director keys and nothing is heard, with no diagnostic
    // anywhere. Abandon unconditionally, snapshot FIRST: a buffer published
    // in the race window between the snapshot above and this call is still
    // ahead of m_audio_read_index and will still be picked up by the next
    // drain_audio(); abandoning only clears the flag so the NEXT publish
    // after this point is guaranteed to (re-)cross the edge and notify.
    audio_ring_reader_abandon(hdr);

    m_audio_open = true;
    m_audio_send_fail_count = 0; // F8: fresh session, fresh report budget
    report_session("audio_open", R"("ok":true,"rate":)" + std::to_string(m_audio_rate) +
                   R"(,"channels":)" + std::to_string(m_audio_channels));
    // Not a report_session_state(true, ...) call: "live" is the Zoom
    // channel's confirmed state (set in onCreateChannelResponse once the
    // invite is accepted), not the audio path's. This session can be fully
    // open_audio()-ready while the channel itself never came up, and vice
    // versa (channel live before the plugin ever calls talkback_open) --
    // conflating the two would let a working audio pipe into a nonexistent
    // channel read as "live".
    return true;
}

namespace {
struct SendCtx {
    ZOOMSDK::IMeetingTalkbackController *ctrl;
    const zchar_t *channel;
    uint32_t rate;
    ZOOMSDK::ZoomSDKAudioChannel chan;
    uint32_t sent;
    uint32_t no_channel_drops;   // buffers seen while ctrl/channel is unset
    int last_err;
};

void send_one(const void *pcm, uint32_t byte_len, uint64_t, void *ctx)
{
    auto *c = static_cast<SendCtx *>(ctx);
    // No channel yet (no probe has established one in this session, or a
    // future channel-selection milestone has not run) -- there is nowhere to
    // send this buffer. That is a real choice, not a bug, but per this
    // codebase's rule against silent audio loss it must still be counted,
    // not just dropped -- see the no_channel_drops report below.
    if (!c->ctrl || !c->channel) { ++c->no_channel_drops; return; }
    const ZOOMSDK::SDKError e = c->ctrl->SendAudioDataToChannel(
        c->channel, static_cast<const char *>(pcm), byte_len, c->rate, c->chan);
    if (e != ZOOMSDK::SDKERR_SUCCESS) c->last_err = static_cast<int>(e);
    ++c->sent;
}
} // namespace

void EngineTalkback::drain_audio()
{
    // Only bail before touching the ring when there is no valid mapping to
    // touch -- m_ctrl / an established channel being unset is NOT one of
    // these reasons. See below: a missing channel still drains (and
    // discards, loudly) rather than returning early, because returning
    // early here would leave `notify` set on a ring that DOES have pending
    // data -- the writer only re-notifies on the empty->non-empty edge, so
    // an early return here would silence talkback the moment a key opens
    // before the first channel is ever established, with no self-healing
    // short of the writer recreating the region. That is exactly the
    // failure class the edge-triggered protocol's helpers exist to rule
    // out (see ShmAudioHeader::notify in src/engine-ipc.h).
    if (m_audio_region.ptr == nullptr) return;

    // F1 review-round fix: !m_audio_open with a still-mapped region is a
    // separate case from "nothing to touch" above, and it must not be
    // handled the same bare-return way. A bare return here neither drains
    // nor abandons -- it just does nothing, which per the comment above is
    // exactly the "consumed a wakeup and left the flag set" shape: if
    // `notify` happens to already be 1 (a plausible state, e.g. reached via
    // any future path that maps the region before flipping m_audio_open),
    // this call silently eats the opportunity to clear it, and the writer's
    // edge-triggered protocol has no way to know a reader ever looked. Hand
    // the flag back explicitly instead of doing nothing.
    //
    // F5 review-round fix: this branch used to be effectively unreachable
    // for open_audio()'s own rejection paths (layout mismatch, unsupported
    // rate/channels, pipe/header mismatch), because open_audio() used to
    // shm_region_destroy() the mapping immediately on every rejection --
    // which nulled m_audio_region.ptr and made every later drain_audio()
    // call hit the bail above instead of this one. open_audio() now leaves a
    // rejected mapping open specifically so THIS bail is what runs for that
    // case: this is the fix, not a defensive branch for a case that cannot
    // happen. See the comment on open_audio()'s layout_mismatch rejection.
    if (!m_audio_open) {
        audio_ring_reader_abandon(static_cast<ShmAudioHeader *>(m_audio_region.ptr));
        return;
    }

    // The channel to talk on is the SESSION's channel (Milestone 5), not the
    // probe's: the probe's m_channel_id_z is destroyed by tick() from a
    // separate thread three seconds after it opens, so sending on it here
    // would race that destroy mid-SendAudioDataToChannel. The session owns
    // its own channel for exactly this reason -- see the comment on
    // session_start()/session_stop() -- so tick() can never touch what this
    // function sends on; the race is now structurally impossible rather than
    // merely unlikely. No channel id crosses the IPC boundary and nothing
    // here needs a UTF-8 -> zchar_t conversion.
    std::basic_string<zchar_t> channel_copy;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        channel_copy = m_session_channel_z;
    }

    auto *hdr = static_cast<ShmAudioHeader *>(m_audio_region.ptr);

    SendCtx ctx{m_ctrl, channel_copy.empty() ? nullptr : channel_copy.c_str(),
                m_audio_rate,
                m_audio_channels > 1 ? ZOOMSDK::ZoomSDKAudioChannel_Stereo
                                     : ZOOMSDK::ZoomSDKAudioChannel_Mono,
                0, 0, 0};

    // EVENTS ARE PROMPTS, NOT PAYLOADS: drain everything available, then use
    // the reader helpers to decide whether sleeping is safe. Any return path
    // that consumes a wakeup and leaves notify set silences talkback until the
    // writer's next edge -- the failure that silenced whole sources before the
    // helpers existed. This loop runs unconditionally, whether or not a
    // channel is currently held (see the comment above): draining is what
    // resolves `notify`, sending audio through it is a separate concern.
    uint32_t lost = 0;
    for (int pass = 0; pass < 4; ++pass) {
        talkback_ring_drain(m_audio_region.ptr, m_audio_read_index,
                            send_one, &ctx, &lost);
        if (audio_ring_reader_done(hdr, m_audio_read_index)) break;
        if (pass == 3) audio_ring_reader_abandon(hdr);
    }

    if (ctx.last_err != 0 || lost != 0 || ctx.no_channel_drops != 0) {
        // F8 review-round fix: this used to report on every drain_audio()
        // call that saw any failure. drain_audio() runs on every
        // talkback_audio pipe line -- with a stale channel id (the destroy-
        // path bug fixed elsewhere in this round) or a channel that simply
        // never got established, EVERY buffer fails and this fired
        // unbounded: ~50-100 pipe lines/sec, the same message-storm shape
        // this codebase already has a live incident about (the probe's own
        // "send" report in tick() guards against exactly this, for the same
        // reason). Report the first occurrence, then only periodically.
        ++m_audio_send_fail_count;
        if (m_audio_send_fail_count == 1 || (m_audio_send_fail_count % 100) == 0) {
            report_session("audio_send", R"("code":)" + std::to_string(ctx.last_err) +
                   R"(,"buffers":)" + std::to_string(ctx.sent) +
                   R"(,"lost":)" + std::to_string(lost) +
                   R"(,"no_channel_drops":)" + std::to_string(ctx.no_channel_drops) +
                   R"(,"occurrence":)" + std::to_string(m_audio_send_fail_count));
        }
    }
}

// ── Persistent talkback session (Milestone 5) ───────────────────────────────
//
// Deliberately NOT part of the probe's Phase machine above: that machine
// exists to tear itself down after one tone, which is the opposite of what a
// key held down needs. The session owns its OWN channel (m_session_channel_z,
// distinct from the probe's m_channel_id_z), so tick() -- which destroys the
// PROBE's channel from a separate thread -- can never touch it. That
// separation is what makes the SendAudioDataToChannel/destroy race
// structurally impossible rather than merely unlikely.
bool EngineTalkback::session_live() const { return m_session_live; }

bool EngineTalkback::session_start(ZOOMSDK::IMeetingService *svc,
                                   const std::string &participant_name)
{
    if (m_session_live) {
        // Refusing a redundant start does not end the existing live session
        // -- no report_session_state() here, unlike every other early return
        // in this function: they all report false because they mean the
        // session never got anywhere.
        report_session("session_start", R"("ok":false,"reason":"already_live")");
        return false;
    }

    // R1 review-round fix (mutual exclusion): refuse while the probe's
    // driving thread might still be running -- see the matching guard at the
    // top of probe(), which explains why. This check MUST come before
    // m_svc/m_ctrl are touched below: those are the exact fields tick()
    // dereferences on its own thread for as long as has_pending_work() is
    // true, and reassigning them here while that thread is mid-call was
    // Critical 2 -- a genuine cross-thread pointer race, not merely a
    // logical ordering nit. See has_pending_work()'s doc comment in the
    // header for why phase-alone (is_idle()) is not the right check: a
    // queued-but-undrained stray still means tick() has work left to do.
    if (has_pending_work()) {
        report_session("session_start", R"("ok":false,"reason":"probe_busy")");
        report_session_state(false, "probe_busy");
        return false;
    }

    if (!svc) {
        report_session("session_start", R"("ok":false,"reason":"not_in_meeting")");
        report_session_state(false, "not_in_meeting");
        return false;
    }
    m_svc  = svc;
    m_ctrl = m_svc->GetMeetingTalkbackController();
    if (!m_ctrl) {
        report_session("session_start", R"("ok":false,"reason":"no_controller")");
        report_session_state(false, "no_controller");
        return false;
    }
    if (!m_ctrl->IsMeetingSupportTalkBack()) {
        report_session("session_start", R"("ok":false,"reason":"not_supported")");
        report_session_state(false, "not_supported");
        return false;
    }
    m_ctrl->SetEvent(this);

    // m_pending_create is guarded by m_chan_mtx -- see the header comment on
    // it. Copy the decision out under the lock, release, THEN call the SDK.
    bool create_gate_ok;
    TalkbackChannelOwner expired_owner;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        // Follow-up to the F1 review-round fix, extended for Task 2: lazily
        // unwedge a stale Session-owned pending create (possibly from a
        // PREVIOUS session whose response never arrived) or a stale
        // Nomination-owned one before evaluating the gate -- see
        // expire_stale_pending_create_locked()'s doc comment.
        expired_owner = expire_stale_pending_create_locked();
        create_gate_ok = talkback_may_request_create(m_pending_create);
    }
    handle_expired_create(expired_owner);
    if (!create_gate_ok) {
        // The probe is mid-create. Refuse rather than queue: a queued create
        // would arrive with the other subsystem's response still in flight,
        // which is exactly the ambiguity the arbiter exists to remove.
        report_session("session_start", R"("ok":false,"reason":"create_busy")");
        report_session_state(false, "create_busy");
        return false;
    }

    m_session_participant = participant_name;
    const ZOOMSDK::SDKError e = m_ctrl->CreateChannel(1);
    report_session("session_start", R"("code":)" + std::to_string(static_cast<int>(e)) +
                   R"(,"participant":")" + json_escape(participant_name) + "\"");
    if (e != ZOOMSDK::SDKERR_SUCCESS) {
        report_session_state(false, "create_failed");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        m_pending_create = TalkbackChannelOwner::Session;
        // Follow-up to the F1 review-round fix: same value as the probe's
        // kAwaitTimeout, reused deliberately rather than inventing a second
        // constant -- both time out the SAME underlying wait (a Zoom
        // CreateChannel response), so there is no reason for the session's
        // bound to differ from the probe's already-proven-safe one. 10s is
        // comfortably longer than a healthy CreateChannel round-trip (which
        // this file's own probe path treats as normally sub-second) and
        // short enough that a genuinely swallowed response does not wedge
        // the arbiter for long. See expire_stale_pending_create_locked()
        // (renamed from expire_stale_session_create_locked() in Task 2 fix
        // round 1, when it grew a Nomination arm too).
        m_session_create_deadline.store(
            (std::chrono::steady_clock::now() + kAwaitTimeout).time_since_epoch().count(),
            std::memory_order_release);
    }
    return true;
}

void EngineTalkback::session_stop()
{
    if (!m_session_live && m_session_channel_z.empty()) {
        // F1 review-round fix (CRITICAL): this used to clear m_pending_create
        // here unconditionally and return -- but if a Session-owned
        // CreateChannel is still outstanding (this branch is exactly the one
        // that runs when nothing local is live yet, e.g. a push-to-talk tap
        // released before the create round-trip returns, a dead-man close in
        // that same window, key_on()'s tap-open failure path, Leave, or
        // quit), the CreateChannel already went out to Zoom. Clearing the
        // arbiter's record of it here does not cancel that request: when
        // onCreateChannelResponse eventually arrives, the arbiter would see
        // owner == None, the id would match neither m_channel_id_z nor
        // m_session_channel_z, and it would be queued onto m_stray_channels
        // -- which nothing drains without a probe's driving thread running
        // (drain_stray_channels() has exactly one caller, tick(), which has
        // exactly one caller, the probe's driving thread). One of the
        // meeting's 16 channels, gone for the meeting.
        //
        // Fix: record the cancellation and leave m_pending_create AS Session
        // -- the eventual response is still routed to the Session branch in
        // onCreateChannelResponse (not lost to "owner == None"), where the
        // cancelled flag makes it destroy the channel immediately instead of
        // adopting it as live or queuing it as a stray. This does mean
        // m_pending_create stays "Session" (refusing every other create)
        // until that response arrives -- but unlike an earlier version of
        // this fix, that is now BOUNDED: m_session_create_deadline (set in
        // session_start(), same value as probe()'s kAwaitTimeout) gives a
        // swallowed response the same self-healing treatment tick()'s
        // AwaitingChannel timeout already gives Probe -- see
        // expire_stale_pending_create_locked() (renamed from
        // expire_stale_session_create_locked() in Task 2 fix round 1),
        // checked lazily by probe()'s, session_start()'s, and
        // nomination_create_next()'s own gate checks. No new thread or
        // timer.
        //
        // Fix round 2: routed through the shared talkback_cancel()
        // (src/talkback-channel-owner.h) instead of writing
        // m_session_create_cancelled directly -- see that function's
        // comment for why sharing the transition with nomination_reset()'s
        // equivalent call matters (N1 was the two owners' WRAP-UP logic
        // diverging, not this exact line, but this and
        // expire_stale_pending_create_locked() are the same class of
        // per-owner duplication that let it happen).
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        TalkbackCreateState state{m_pending_create, m_session_create_cancelled,
                                  m_nomination_create_cancelled};
        state = talkback_cancel(state, TalkbackChannelOwner::Session);
        m_session_create_cancelled    = state.session_cancelled;
        m_nomination_create_cancelled = state.nomination_cancelled;
        return;
    }

    std::basic_string<zchar_t> channel_copy;
    std::string channel_copy_utf8;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        channel_copy      = m_session_channel_z;
        channel_copy_utf8 = m_session_channel;
        m_session_channel_z.clear();
        m_session_channel.clear();
        if (m_pending_create == TalkbackChannelOwner::Session)
            m_pending_create = TalkbackChannelOwner::None;
    }
    m_session_live    = false;
    m_session_user_id = 0;

    if (channel_copy.empty() || !m_ctrl) {
        report_session("session_stop", R"("ok":true,"reason":"no_channel")");
        return;
    }

    // The session destroys its OWN channel here, on the command-loop thread.
    // This is NOT the batch-destroy path tick() owns for the probe's stray
    // queue -- keeping them separate is what keeps the probe's stray queue
    // drained by exactly one thread. It is one of the four Begin/Add/Execute
    // sequences inventoried at the top of tick(); read that before adding a
    // fifth.
    //
    // F3 review-round fix: this comment used to claim that R1's mutual
    // exclusion (probe() refuses while m_session_live, session_start()
    // refuses while has_pending_work()) by itself guarantees tick() can
    // never be mid-Destroying (or mid-drain_stray_channels) while this runs.
    // That is incomplete: R1 is a single-instant check made when a probe or
    // a session STARTS, and says nothing about a driving thread that has
    // already started, passed has_pending_work(), and gone to sleep_for(10ms)
    // -- it can wake and call tick() again after a session has since started,
    // which is a real window where tick()'s Destroying/drain_stray_channels
    // and this function's own Begin/Add/Execute sequence could interleave on
    // the same controller object. What actually rules that out is
    // engine/src/main.cpp's TalkbackStart branch joining talkback_thread
    // BEFORE calling session_start() at all -- exactly like the TalkbackProbe
    // branch already joins it before calling probe() -- so by the time this
    // function (or session_start()) ever runs, no probe driving thread can
    // exist to race it.
    //
    // R4 review-round fix: this used to call Begin/Add/Execute exactly once
    // and report whatever code came back -- unlike every OTHER destroy path
    // in this file (tick()'s Destroying phase, drain_stray_channels()), both
    // of which retry up to kMaxDestroyAttempts times before giving up. This
    // file's own doctrine (stated at both of those call sites) is that a
    // leaked channel consumes one of the meeting's 16 for the rest of the
    // meeting; a single transient failure here silently leaked the
    // session's channel the same way. Match the existing retry pattern
    // instead of inventing a third variant.
    ZOOMSDK::SDKError e = ZOOMSDK::SDKERR_UNKNOWN;
    uint32_t attempt = 0;
    for (; attempt < kMaxDestroyAttempts; ++attempt) {
        e = m_ctrl->BeginBatchDestroyChannels();
        if (e == ZOOMSDK::SDKERR_SUCCESS)
            e = m_ctrl->AddChannelToDestroy(channel_copy.c_str());
        if (e == ZOOMSDK::SDKERR_SUCCESS)
            e = m_ctrl->ExecuteBatchDestroyChannels();
        if (e == ZOOMSDK::SDKERR_SUCCESS) break;
    }
    report_session("session_stop", R"("code":)" + std::to_string(static_cast<int>(e)) +
                   R"(,"attempts":)" + std::to_string(attempt + 1));
    if (e != ZOOMSDK::SDKERR_SUCCESS) {
        report_session("session_destroy_abandoned",
                       R"("channel":")" + json_escape(channel_copy_utf8) + "\"");
    }
}

void EngineTalkback::close_audio()
{
    // F5 review-round fix: this used to bail on !m_audio_open alone, which
    // was equivalent to bailing on "nothing mapped" back when the only way
    // to have a mapped region was m_audio_open == true. open_audio() now
    // leaves a REJECTED region mapped (m_audio_open stays false) so
    // drain_audio()'s not-open bail can still abandon the notify flag on it
    // -- see open_audio()'s layout_mismatch rejection comment -- which makes
    // "mapped but never opened" a real, reachable state. This function is
    // the only thing (besides a later open_audio(), which calls this first)
    // that ever releases that mapping, so bailing on !m_audio_open alone
    // would leak it: every subsequent open_audio() would open a NEW region
    // without the old one ever being unmapped. Bail only when there is
    // truly nothing to release.
    if (!m_audio_open && m_audio_region.ptr == nullptr) return;
    m_audio_open = false;
    if (m_audio_region.ptr) {
        // Hand the flag back so the writer re-notifies rather than assuming a
        // reader is still listening.
        audio_ring_reader_abandon(static_cast<ShmAudioHeader *>(m_audio_region.ptr));
    }
    shm_region_destroy(m_audio_region);
    m_audio_region = ShmRegion{};
    m_audio_read_index = 0;
    report_session("audio_close", R"("ok":true)");
}
