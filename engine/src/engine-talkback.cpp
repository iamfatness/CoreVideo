#include "engine-talkback.h"
#include "engine-writer.h"   // EngineIpc::write -- an inline fn in a namespace,
                             // so it must be INCLUDED, never forward-declared
#include "talkback-tone.h"
#include "engine-json.h"     // zchar_to_utf8 / json_escape / json_str (Step 3a)
#include "talkback-ring.h"   // talkback_ring_drain / TalkbackRingSlotFn (Milestone 2)

#include <algorithm>          // std::find / std::remove (Task 4 roster diffing)
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

// LIVE GATE RUN 1 (2026-08-26, real meeting, 20:04:37). ZOOM RATE-LIMITS
// BACK-TO-BACK CreateChannel CALLS. A two-channel plan issued channel 2's
// create synchronously from inside channel 1's onCreateChannelResponse -- the
// log shows both at 20:04:37.291, a 0ms gap -- and Zoom returned
// SDKERR_TOO_FREQUENT_CALL (enum position 18 in zoom_sdk_def.h). The ladder
// aborted terminally, correctly and uselessly: every real talent list plans
// more than one channel, so no nomination could ever have succeeded live.
//
// kNominationCreateSpacing: the minimum gap between one create's RESPONSE and
// the next create's ISSUE. 300ms is six turns of the command loop's 50ms idle
// pump (ipc_read_line_with_message_pump's MsgWaitForMultipleObjects timeout in
// main.cpp), so the pump's granularity cannot round it down to something near
// the 0ms Zoom refused; and it keeps a big plan tolerable -- the 13-channel
// 11-nominee case provisions in ~4s of otherwise idle wall time, once, at
// nomination rather than at key time. Zoom documents no rate for this, so the
// value is an engineering guess with margin, not a published limit; if a later
// gate still sees 18 at 300ms, raise this rather than leaning on the retry.
constexpr std::chrono::milliseconds kNominationCreateSpacing{300};

// The first backoff after an actual SDKERR_TOO_FREQUENT_CALL, doubled per
// retry: 500 / 1000 / 2000 / 4000ms. Starts above kNominationCreateSpacing
// because the spacing already proved insufficient by the time this fires.
constexpr std::chrono::milliseconds kNominationRateLimitBackoff{500};

// Per-channel cap on those retries. Four gives 7.5s of total backoff for one
// channel -- long enough to ride out a burst, short enough that an operator
// who nominated during a Zoom-side problem gets a terminal answer rather than
// a ladder that looks alive forever. Exhausting it aborts with reason
// "create_rate_limited" (not the generic create failure) so the log names the
// true cause.
constexpr uint32_t kMaxNominationCreateRetries = 4;

// Final-review C1 (CRITICAL): the ",\"attempt\":N" suffix that identifies
// which nominate attempt a report belongs to, so the plugin can match it to
// the staging slot it came from instead of assuming it is whatever is staged
// when it arrives. 0 means the requester did not identify its attempt (a
// raw-pipe caller, or a plugin older than this fix) and emits NOTHING -- such
// a report is then byte-identical to what a pre-C1 engine emitted, which is
// exactly what the plugin's tolerant "no attempt field means it matches" path
// expects.
//
// WHICH REPORTS CARRY IT (verification round: the first version of this
// comment justified omitting it from stage lines by saying the plugin's
// mapping ignores unmatched stage lines -- circular, since no stage line
// COULD be unmatched while this function was the reason none carried an id).
// The real rule is what the plugin's state machine CONSUMES
// (src/talkback-nomination-dispatch.h): the three STAGING stages
// (uncovered_private, unreachable, plan) and every TERMINAL. Staging stages
// need it as much as terminals do and for the same reason -- two nominates
// can sit in the pipe before this engine reads the first, so the plugin can
// already have staged the SECOND when the first's stage lines arrive, and
// unidentified they would fold one attempt's shortfall names into the other
// attempt's record. uncovered_private is read by
// talkback_target_known_unprovisioned() (src/talkback-plan.h), so a spurious
// name there is a key refused on a channel that is standing: F1's symptom,
// one door further in.
//
// Everything else this file reports (channel_created, replacing,
// create_channel, member_invited, channel_destroyed, ...) is a log-only trace
// line the dispatcher never dispatches on -- it matches by stage name, and
// those names are not in its table at all -- so an id there would be bytes
// with no consumer.
std::string attempt_field(uint32_t attempt)
{
    if (attempt == 0) return std::string();
    return R"(,"attempt":)" + std::to_string(attempt);
}
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
// session_start()/session_stop() or the Milestone 2 audio path
// (open_audio/drain_audio/close_audio), which the probe never calls.
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
    // all: a genuine cross-thread pointer race (Critical 2). The
    // batch-destroy half of the original reasoning (tick()'s destroy for the
    // probe's channel versus session_stop()'s for the session's, Important 4)
    // no longer applies -- Task 3's session_stop() destroys nothing -- but the
    // pointer race does, unchanged, and it is on its own sufficient. A probe
    // is a ~3s diagnostic; refusing it for the life of an active talkback
    // session costs nothing real -- do not "relax" this to allow concurrent
    // use. See the matching guard in session_start().
    //
    // Also not routed through probe_refused_without_ladder(), for a
    // different reason than the guard above: no driving thread can exist
    // here (this guard is what stops one starting, and main.cpp joins any
    // earlier one before session_start() runs), so draining would be safe
    // -- but it would be a NEW batch-destroy on a live show's command loop
    // while a key is down, which is not something a refused diagnostic
    // should introduce. Unchanged behaviour: a stray queued during a live
    // session waits for the next probe. The return value,
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
    // Gate behind the same arbiter nominate() uses: exactly one create may
    // be outstanding across the probe and nomination (see
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
        // unwedge a stale Nomination-owned pending create before evaluating
        // the gate -- see expire_stale_pending_create_locked()'s doc comment.
        expired_owner = expire_stale_pending_create_locked();
        create_gate_ok = talkback_may_request_create(m_pending_create.owner);
    }
    handle_expired_create(expired_owner);
    if (!create_gate_ok) {
        // THIS is the refusal that made the two-batch-destroy-callers window
        // reachable while it still reported success -- see
        // probe_refused_without_ladder() and the inventory at the top of
        // tick(). The gate is closed precisely because a Nomination-owned
        // create is outstanding, i.e. precisely when a response that destroys
        // directly is about to land on the command loop; a driving thread
        // spawned here would be the second batch-destroy caller.
        report("busy", R"("reason":"create_busy")");
        return probe_refused_without_ladder();
    }
    const ZOOMSDK::SDKError create_err = m_ctrl->CreateChannel(1);
    report("create_channel", R"("code":)" +
           std::to_string(static_cast<int>(create_err)));
    if (create_err != ZOOMSDK::SDKERR_SUCCESS) return probe_refused_without_ladder();
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        m_pending_create = talkback_create_issued(m_pending_create,
                                                  TalkbackChannelOwner::Probe);
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

bool EngineTalkback::channel_is_provisioned_locked(const zchar_t *channelID) const
{
    // Caller holds m_chan_mtx -- see the header. Null-safe by construction:
    // comparing a basic_string against a null zchar_t* would be
    // char_traits::length(nullptr), and a null id is reachable here on any
    // error code (the callback's own null guard says so).
    if (!channelID) return false;
    for (const auto &pc : m_provisioned_channels)
        if (pc.channel_id_z == channelID) return true;
    return false;
}

bool EngineTalkback::adopt_probe_channel(const zchar_t *channelID,
                                         const std::string &id_utf8)
{
    // Fix round 1, M1. ONE lock scope containing both the check and the
    // assignment, so "is this already somebody's live channel" cannot be
    // asked by one adoption path and skipped by another -- which is exactly
    // what Task 3 shipped: the provisioned-table check guarded the stray
    // queue and not the probe's adoption.
    std::lock_guard<std::mutex> lock(m_chan_mtx);
    if (channel_is_provisioned_locked(channelID)) return false;
    m_channel_id = id_utf8;             // UTF-8, reporting only
    m_channel_id_z.assign(channelID);   // SDK identifier, verbatim -- see header
    return true;
}

TalkbackChannelOwner EngineTalkback::expire_stale_pending_create_locked()
{
    // Caller holds m_chan_mtx -- see the header comment on this function and
    // on m_nomination_create_deadline. Only Nomination needs this now:
    // Probe's pending create has a clearer of its own (tick()'s
    // AwaitingChannel timeout, running on the driving thread), so a stale
    // Probe entry here would be a bug in that machinery rather than something
    // to paper over; and Session no longer issues a create at all (Task 3),
    // so there is nothing of its to go stale. See the header comment for why
    // dropping that arm is the fix for the parked Session expire-path race
    // rather than a gap left in it.
    if (m_pending_create.owner != TalkbackChannelOwner::Nomination)
        return TalkbackChannelOwner::None;

    const auto deadline = std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(
            m_nomination_create_deadline.load(std::memory_order_acquire)));
    if (std::chrono::steady_clock::now() < deadline) return TalkbackChannelOwner::None;

    // The response never arrived (or arrived and was somehow lost before
    // reaching onCreateChannelResponse -- either way, indistinguishable from
    // here). Forget the pending create so talkback_may_request_create()
    // unwedges for probe() AND nominate(). The expired owner is returned and
    // reported by the caller -- see the header comment on this function for
    // why the report happens outside m_chan_mtx.
    //
    // Fix round 2, N1 (Major, introduced by fix round 1): the owner AND that
    // owner's own cancellation flag are cleared TOGETHER by ONE shared
    // function, talkback_expire() (src/talkback-channel-owner.h), instead of
    // this function hand-rolling a separate if/else per owner. Round 1's
    // hand-rolled version cleared the SESSION cancellation flag in the
    // Session arm but forgot the equivalent line for the NOMINATION one
    // in the Nomination arm: nominate() -> Leave (before the response is
    // pumped) -> nomination_reset() sets the flag and leaves the owner as
    // Nomination -> the response never arrives at all -> more than
    // kAwaitTimeout later, a fresh nominate() re-arms the owner -> THIS
    // function fired, cleared the owner, but the now-orphaned cancelled flag
    // survived -> the FRESH create's own response later arrived, was
    // claimed as Nomination, found the flag still true, and destroyed the
    // brand-new channel instead of adopting it -- the operator's next
    // nomination silently provisioned zero channels. There is only one owner
    // left to route through it (Task 3), so the asymmetry that produced N1 is
    // no longer even expressible here -- but the call stays the shared
    // transition rather than an inlined clear, because the moment a second
    // owner returns, an inlined copy is exactly how N1 came back.
    const TalkbackChannelOwner expired = m_pending_create.owner;
    m_pending_create = talkback_expire(m_pending_create);

    // A swallowed Nomination create response also means the rest of the
    // queued plan can never be provisioned by THIS ladder (there is nothing
    // to resume from -- we don't know if the channel exists). Forget the
    // queue so a later nominate() starts clean instead of silently
    // continuing to believe channels are still coming.
    //
    // Safe to do unconditionally, including when THIS very call is nested
    // inside a fresh nominate()'s own nomination_create_next(): nominate()
    // (fix round 2) calls nomination_create_next() -- and therefore this
    // function -- BEFORE assigning its own plan into m_nomination_pending,
    // specifically so that if an expiry fires here it can only ever be
    // clearing a genuinely stale leftover queue from an EARLIER ladder, never
    // the queue the calling nominate() is about to populate. Do not move this
    // clear to run AFTER that assignment without re-verifying that ordering
    // still holds -- see nominate()'s own comment on why the order matters.
    m_nomination_pending.clear();

    // The generation bump that goes with abandoning this create is NOT a
    // separate statement here: talkback_expire() above does it as part of the
    // same transition (fix round 5). It used to sit on its own line, which is
    // exactly the shape a re-review mutation exploited elsewhere in this file
    // -- delete the one line, keep the other, and the whole suite stays
    // green. See src/talkback-channel-owner.h's note on TalkbackCreateState.
    //
    // Fix round 4: m_provisioned_channels is NOT cleared here -- the caller
    // destroys it via handle_expired_create() once the lock is released,
    // because those are real Zoom channels and clearing the table without
    // destroying them would leak channel budget for the rest of the meeting.
    // See handle_expired_create()'s comment for why the expiry must not leave
    // them standing either.
    return expired;
}

void EngineTalkback::handle_expired_create(TalkbackChannelOwner expired_owner)
{
    // Fix round 4. Called by every caller of
    // expire_stale_pending_create_locked() AFTER they release m_chan_mtx --
    // never with it held: the Nomination arm calls the SDK. See the
    // declaration comment for why the Nomination arm destroys, and for why
    // this stays a function of the owner now that only one owner reaches it.
    if (expired_owner == TalkbackChannelOwner::Nomination) {
        report_nomination("create_expired",
                          R"("reason":"swallowed_create_response")");
        // The expiry has already forgotten the queue (m_nomination_pending)
        // but the channels this ladder DID create are still standing on Zoom
        // and still in m_provisioned_channels: a PARTIAL set, unreachable by
        // any key that needs the whole fan-out and consuming budget out of
        // the meeting's 16. Destroy them, exactly as fix round 1's M2 does
        // for an error response, so what remains is either a complete set or
        // nothing.
        //
        // The reason this arm was originally written -- "they refuse every
        // later nominate() with already_provisioned" -- is VOID as of Task 3,
        // which replaced that gate with a replace-in-place path; the round-1
        // review caught the comment still arguing from it. The behaviour is
        // unchanged and still right for the reason above, and the ordering
        // that makes it safe is worth stating in its place: this only ever
        // fires against a stalled ladder's OWN partial set, because
        // nominate()'s replace path destroys any earlier COMPLETE set before
        // issuing a create. No-op when the table is empty, the common case.
        //
        // Fix round 3 (N6, Major): nomination_abort_ladder() (not a bare
        // nomination_destroy_provisioned()) is what tells the plugin this
        // ladder is over -- this was one of the three async abort paths that
        // destroyed silently while round 2 fixed only the two SYNCHRONOUS
        // ones inside nomination_create_next(). Its own queue-clear is
        // redundant with the one above (harmless -- clearing an
        // already-empty vector) but kept rather than special-cased, so this
        // call site looks exactly like every other nomination_abort_ladder()
        // caller.
        nomination_abort_ladder("create_expired");
    }
}

void EngineTalkback::tick()
{
    // BATCH-DESTROY DISCIPLINE. The previous version of this comment said
    // "tick() is the ONLY caller of the batch-destroy API ... Callbacks ...
    // never call the API themselves". That stopped being true in Task 2 fix
    // round 1 and was left standing through two more rounds. Re-counted from
    // the code in Task 3 (`grep -n "BeginBatchDestroyChannels\|destroy_
    // channel_retrying(" engine/src/engine-talkback.cpp`, run before this
    // sentence was written): THREE Begin/Add/Execute sequences, on TWO
    // threads:
    //   * drain_stray_channels() (called only from here) and tick()'s own
    //     Destroying phase -- the PROBE'S DRIVING THREAD;
    //   * destroy_channel_retrying() -- four call sites, all Nomination's:
    //     onCreateChannelResponse's stale, cancelled and untracked branches,
    //     plus nomination_destroy_provisioned(): the COMMAND-LOOP THREAD.
    // Task 3 removed the fourth sequence (session_stop()'s own copy of the
    // loop) and the fifth call site (the session-cancelled branch) along with
    // the session's CreateChannel: a key release destroys nothing now, which
    // is the point of the milestone -- the channel has to survive it.
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
    //      excludes any Nomination-owned create for as long as it does --
    //      and the three command-loop destroy branches all sit inside
    //      `owner == Nomination`, so none of them can be reached while this
    //      thread is alive.
    //   3. The remaining command-loop destroy
    //      (nomination_destroy_provisioned(), via nominate()'s replace path
    //      or handle_expired_create()) is reached only from command branches
    //      that JOIN this thread first -- main.cpp joins before nominate()
    //      exactly as it does before probe().
    // Fact 1 was FALSE until fix round 4: a probe refused because the
    // arbiter was held -- i.e. refused precisely when another owner's
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
                if (m_pending_create.owner == TalkbackChannelOwner::Probe)
                    m_pending_create = talkback_expire(m_pending_create);
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

        // Fix round 1 (review, Minor 5): put the probe's own duck back before
        // destroying. RUNG 4 sets this channel's background volume to 0.3 so
        // the tone is unambiguous, and nothing ever restored it. Usually
        // harmless -- the channel is about to cease to exist -- but the
        // destroy below can exhaust its retries and be ABANDONED, and then a
        // participant is left hearing the meeting at 30% for the rest of it
        // because of a three-second diagnostic. Task 3 established the
        // set-on-the-way-in / restore-on-the-way-out pairing for the session;
        // this is the one place that still had only half of it.
        if (!channel_copy.empty())
            m_ctrl->SetChannelBackgroundVolume(channel_copy.c_str(), 1.0f);

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

    // Route by who asked. See src/talkback-channel-owner.h: the response
    // carries no indication of its requester, so the arbiter is the only
    // thing standing between the probe and the session adopting each other's
    // channels. Claim and clear happen in the SAME lock scope -- once this
    // response has been attributed to an owner, m_pending_create must not be
    // observable as still "theirs" by anyone else, even for the instant
    // between a separate claim and a separate clear.
    //
    // Fix round 5: ALL FOUR parts of attributing a response -- who owns it,
    // releasing the arbiter, reading-and-clearing that owner's cancellation
    // flag, and judging its generation -- happen HERE, in one call, before
    // any branch below runs. Not "in this scope, on four lines": in one
    // transition (talkback_create_response()), so a branch cannot skip one
    // and a mutation cannot delete one.
    //
    // Both halves of that were live defects. Round 3 updated the generation
    // only inside the Nomination branch, so a response claimed by
    // None/Probe/Session skipped it and permanently desynchronised the
    // tracking. Round 4 moved it here but left it as its own assignment next
    // to the claim -- a re-review deleted that one line and all 64 tests
    // stayed green. And round 4 left the cancellation check-and-clear inside
    // the two owner branches, where the Stale early-return jumped over it:
    // the flag outlived its create, the next nomination destroyed its own
    // first channel, and `already_provisioned` refused every nomination for
    // the rest of the meeting (F1, Major -- N1's third door).
    //
    // Pure; no SDK call under m_chan_mtx.
    TalkbackCreateResponse claimed;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        claimed = talkback_create_response(m_pending_create);
        m_pending_create = claimed.next;
    }
    const TalkbackChannelOwner owner = claimed.owner;
    const bool cancelled = claimed.cancelled;
    const TalkbackResponseFreshness freshness = claimed.freshness;

    // LIVE GATE RUN 1, cosmetic finding: this raw trace line used to be the
    // FIRST statement of this function, before the arbiter claim, and so had
    // no way to know who the response belonged to -- it always went through
    // report(), i.e. "cmd":"talkback_probe", and the gate log shows it doing
    // exactly that in the middle of a nomination ladder. Moved below the
    // claim and routed by owner, the same ReportSink split resolve_participant()
    // already does for participant_talkback_support (fix round 1, M3). Nothing
    // reports between the old position and this one, so the only observable
    // change is the "cmd" field -- and no consumer keys on this stage name
    // (it is a log-only trace line; see attempt_field()'s inventory).
    const std::string response_fields =
        R"("channel":")" + json_escape(id) + R"(","error":)" +
        std::to_string(static_cast<int>(error));
    if (owner == TalkbackChannelOwner::Nomination)
        report_nomination("create_channel_response", response_fields);
    else
        report("create_channel_response", response_fields);
    // NO SESSION BRANCH ANY MORE (Task 3). It used to sit here and was the
    // most delicate code in this file: session_start() issued a CreateChannel
    // on the key press, so this callback had to decide, with no correlation
    // id from Zoom, whether a response belonged to the key that is still down,
    // to one already released (adopt vs. destroy-cancelled), or to the press
    // before last. Milestone 5 needed three fix rounds for that and the
    // round-6 review still parked a Major on it -- a first press's late
    // response adopted as live while the second press's real response reached
    // m_stray_channels, which nothing drains, wedging has_pending_work() true
    // for the life of the process and orphaning a channel on Zoom.
    //
    // Keying now selects an already-provisioned channel, so the session
    // issues no create and nothing ever claims the arbiter for Session. The
    // race is not fixed here; it is unreachable, because the create that
    // made it expressible is gone. If some future change gives the session a
    // create back, this branch has to come back WITH the disposition
    // machinery -- and inherit that history, not just the code.
    //
    // Nothing special is done for a hypothetical owner == Session response.
    // Where it actually goes -- corrected in fix round 1, because the Task 3
    // version of this sentence claimed it "falls through to the id-comparison
    // path at the bottom", and that is only true when m_phase is NOT
    // AwaitingChannel. With a probe mid-ladder it reaches the probe's
    // ADOPTION instead, and that mis-statement is the exact argument M1 hid
    // behind for owner == Probe (a real, reachable case). Both destinations
    // are now safe for the same reason: adoption goes through
    // adopt_probe_channel(), which refuses any id the provisioned table
    // already holds. Do not restate where a response "falls through to"
    // without checking m_phase; that is what made this comment wrong.
    if (owner == TalkbackChannelOwner::Nomination) {
        // Fix round 3, "expire-path double create" (Major, found by the
        // round-2 re-review); mechanism rebuilt in fix round 4 -- see
        // src/talkback-channel-owner.h's "Generation tracking" section and
        // m_pending_create's header comment. A create that merely
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
            //
            // THE RULE THIS BRANCH FOLLOWS (rewritten, final review m1/N9 --
            // the previous wording forbade touching m_provisioned_channels
            // three lines above a call that correctly does, and had done
            // since fix round 3 unified the teardown): destroy exactly the
            // channel THIS RESPONSE NAMES -- it is genuinely orphaned on
            // Zoom's side and nothing else will ever clean it up -- ADVANCE
            // NOTHING (never nomination_create_next(), which would issue a
            // second create while one may still be in flight, the one
            // invariant the arbiter exists to hold; never re-claim
            // m_pending_create, see below), and then END THE LADDER
            // TERMINALLY through nomination_abort_ladder(). That last step is
            // what tears down the rest of m_provisioned_channels and reports
            // it, and it is deliberate: a ladder this file has given up on
            // must not leave channels standing that no key can reach and no
            // later nominate() can account for.
            //
            // Task 3 (parked ruling 3 from Task 2's close): the QUEUE is
            // cleared, which this branch did not do. The ladder that queued
            // those entries is by definition the one we gave up on, so
            // leaving them behind is "the queue outlives the ladder" -- the
            // exact shape that left already_provisioned stuck for a whole
            // meeting in F1. Every path I can construct reaches here with the
            // queue already empty (an expiry clears it, and only an expiry
            // can make a response read Stale), so this is insurance rather
            // than a fix -- and it is written as two lines precisely because
            // the code deliberately declines to ASSERT that unreachability,
            // after a round-4 comment asserting the same shape of thing was
            // proved wrong by a live Major. Do not delete it on the strength
            // of an argument that it cannot fire.
            //
            // This branch must NOT re-claim the owner (round 3 did:
            // m_pending_create = Nomination). Doing so would claim the
            // arbiter for a create the state has no record of, wedging
            // probe() and nominate() until the deadline expired,
            // and it would falsify the "claims-then-clears regardless of
            // what the branch then does" invariant that
            // src/talkback-channel-owner.h documents as load-bearing.
            //
            // Fix round 5 deleted a six-line derivation that used to sit
            // here claiming this branch was unreachable. It was WRONG --
            // `talkback_nominate -> leave -> talkback_nominate` inside
            // kAwaitTimeout reached it (nominate() bumped the generation
            // with no arbiter check, and nomination_reset() empties both
            // nomination tables while deliberately leaving the owner
            // claimed) -- and it was actively harmful: it told the next
            // reader not to look at a branch that fires, and this branch's
            // early return was at the time skipping the cancellation
            // check-and-clear, which is how one Major (F1) reached a live
            // operator. Both holes are closed now, and no replacement
            // derivation is offered in their place. Treat this branch as
            // live, and if you need to know whether some sequence reaches
            // it, derive that at the time against the code -- do not trust
            // a claim about it written in the past tense.
            //
            // What must not change is the disposition. Making a late
            // response distinguishable from the current one is impossible
            // with what Zoom provides (onCreateChannelResponse carries no
            // correlation id), so widening what routes here is guessing, and
            // guessing wrong DESTROYS a channel the ladder is legitimately
            // waiting on. The deliberate treatment of an AMBIGUOUS response
            // is the opposite one -- fail open, adopt it, let any extra
            // response fall out down channel_untracked. Only a positively
            // superseded generation lands here: destroy exactly what the
            // response names, advance nothing.
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
            // Fix round 3 (N6, Major): nomination_abort_ladder() replaces the
            // bare queue-clear this branch used to do on its own -- the
            // review named this "the queue-clearing sibling" of the other
            // four abort paths' nomination_destroy_provisioned() calls.
            // Whether this is reachable at all is exactly the question the
            // comment above (line 855) declines to answer positively OR
            // negatively, by this file's own hard-won policy -- so it is
            // handled exactly as fully as every path this file DOES know
            // reaches a live operator, rather than half-handled on an
            // argument that it cannot. `nomination_destroy_provisioned()`
            // (called from inside nomination_abort_ladder()) is a no-op on
            // whatever is left in m_provisioned_channels beyond the one
            // channel already destroyed above by name -- normally nothing,
            // per that same comment -- and the terminal report is the one
            // thing this branch never had, on any analysis of when it fires.
            nomination_abort_ladder("channel_stale");
            return;
        }

        // Fix round 1, C1 (CRITICAL): a cancelled create must never be
        // adopted (pushed into m_provisioned_channels), invited, or queued
        // as a stray (nothing drains m_stray_channels without a probe's
        // driving thread running, and queuing here would reproduce the exact
        // wedge this fix exists to close) -- destroy it immediately instead,
        // same Begin/Add/Execute sequence and retry bound as every other
        // command-loop-thread destroy in this file (the stale branch above,
        // the untracked branch below, nomination_destroy_provisioned()).
        //
        // Fix round 5: `cancelled` comes from the arbiter transition at the
        // top of this function, which read AND cleared it before any branch
        // ran. It used to be checked-and-cleared right here, which is how
        // the Stale branch above -- returning earlier -- left the flag set
        // for the NEXT nomination to inherit and destroy its own first
        // channel with (F1, Major; N1's third door). The ordering is now a
        // property of the function, not of where each branch remembers to
        // put its check.
        if (talkback_create_disposition(owner, cancelled) ==
            TalkbackCreateDisposition::DestroyCancelled) {
            // Terminal exit: leave no queue behind. nomination_reset() (the
            // only setter of this flag) already cleared m_nomination_pending
            // when it cancelled, and nominate() now refuses to start a
            // ladder while a create is outstanding, so this is empty on
            // every path I can construct -- but "the queue outlives the
            // ladder" is exactly what left already_provisioned stuck for a
            // whole meeting in F1, so make it structurally impossible here
            // rather than argued. No destroy: the cancelled ladder's
            // channels were already dealt with by nomination_reset()'s
            // caller (Leave/quit), and destroying into a meeting we have
            // left is not something this branch can do safely.
            {
                std::lock_guard<std::mutex> lock(m_chan_mtx);
                m_nomination_pending.clear();
            }
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
            // Fix round 3 (N6, Major): this is the LIKELIER real-world abort
            // of the two N1 originally fixed -- CreateChannel()'s synchronous
            // return mostly validates arguments; a genuine Zoom-side failure
            // (budget past 16 channels, permission, transport) arrives HERE,
            // on the async callback. Kept its own rich diagnostic (the raw
            // SDK error code) in addition to nomination_abort_ladder()'s
            // terminal report -- that function does the "queue can never be
            // provisioned by THIS ladder, forget it" clear and the "channel
            // k failing must not strand 1..k-1 forever" destroy (fix round 1,
            // M2) that used to be hand-written here, AND tells the plugin the
            // attempt is over -- which the hand-written version never did.
            report_nomination("channel_failed",
                              R"("error":)" + std::to_string(static_cast<int>(error)));
            nomination_abort_ladder("channel_failed");
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
                // LIVE GATE RUN 1: the front just advanced, so the rate-limit
                // retry budget belongs to a new channel. Reset it HERE, in the
                // same lock scope as the pop, so the two cannot drift -- the
                // budget is defined as "consecutive retries for the channel at
                // the front", and this is the only place the front advances.
                m_nomination_create_retries = 0;
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
            // LIVE GATE RUN 1 (2026-08-26): this used to be a direct
            // nomination_create_next() -- channel N+1's CreateChannel issued
            // synchronously from inside channel N's response, a 0ms gap, which
            // Zoom refuses with SDKERR_TOO_FREQUENT_CALL (18). The create is
            // now SCHEDULED and nomination_tick() issues it from the command
            // loop once kNominationCreateSpacing has passed. Same thread
            // either way (this callback and the pump both run on the command
            // loop -- see assert_command_loop_thread()); what changes is only
            // that the create no longer leaves from inside the callback.
            nomination_schedule_create(kNominationCreateSpacing);
        else
            report_nomination("nominate_done",
                              R"("channels":)" + std::to_string(provisioned_count) +
                              attempt_field(m_nomination_attempt));
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
        // batches. The Nomination branch above is the path where no probe
        // ladder is running -- see the inventory and the recorded residual
        // gap at the top of tick(). So: here, queue and never call.
        // The comparison against m_channel_id_z below is exactly
        // the access that must go through m_chan_mtx -- reachable while
        // phase is Idle/Done, i.e. while a fresh probe() is allowed to be
        // clearing/reassigning that member on another thread right now; see
        // the m_chan_mtx comment in the header.
        //
        // R2 review-round fix: a channel we OWN is never a stray, no matter
        // what m_pending_create said at routing time above. This is the fix
        // for a redelivered response: the SDK can and does redeliver
        // onCreateChannelResponse (that's what the *_duplicate / *_stray
        // handling in this whole block is for), and a redelivery can arrive
        // long after the request that caused it was already resolved -- at
        // which point m_pending_create has moved on to None or to the OTHER
        // subsystem, so the arbiter above no longer has an opinion about it.
        // Comparing ONLY against m_channel_id_z (as this branch used to)
        // meant a redelivered response for a channel we were talking on
        // landed here, failed that comparison, and got queued as a stray --
        // so tick() destroyed a LIVE channel out from under drain_audio() on
        // the probe's own thread.
        //
        // TASK 3 MOVED WHICH TABLE HOLDS "ours". The session no longer owns a
        // channel of its own; the channels a key press talks on are
        // PROVISIONED ones, so m_provisioned_channels is what this comparison
        // has to consult. Without this change the R2 fix would have gone on
        // reading a member that is always empty, and a redelivered nomination
        // create response would have been queued as a stray and destroyed --
        // taking out a provisioned channel mid-show, which is strictly worse
        // than the session-channel case R2 was written for, because that
        // channel is meant to survive every key press until the meeting ends.
        if (error == TALKBACK_ERROR_OK) {
            bool is_probe_channel;
            bool is_provisioned_channel;
            {
                std::lock_guard<std::mutex> lock(m_chan_mtx);
                is_probe_channel = (m_channel_id_z == channelID);
                // Fix round 1, M1: the same question the adoption path asks,
                // through the same helper, so the two answers cannot drift.
                is_provisioned_channel =
                    !is_probe_channel && channel_is_provisioned_locked(channelID);
                if (!is_probe_channel && !is_provisioned_channel) {
                    // A genuinely different, untracked channel now exists.
                    // Queue it (still under the lock, so the check and the
                    // push are one atomic decision); drain_stray_channels()
                    // (called from tick()) owns actually destroying it.
                    m_stray_channels.emplace_back(channelID);
                }
            }
            if (is_provisioned_channel) {
                // Matches a channel nomination already provisioned -- a
                // duplicate/redelivered callback for a create that was
                // already adopted. Queuing it for destroy would tear down a
                // channel the operator can key at any moment. Report and do
                // NOTHING else.
                report("create_channel_response_provisioned_duplicate",
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
    // Fix round 1, M1 (Major): ADOPT ONLY WHAT IS NOT ALREADY OURS. This
    // used to be a bare assignment of m_channel_id/m_channel_id_z, and it is
    // the path a redelivered NOMINATION response takes when a probe holds the
    // arbiter: the response is attributed to Probe, so the Nomination branch
    // above is skipped, and phase IS AwaitingChannel, so the stray path's own
    // provisioned-table check is skipped too. The probe then invited a
    // participant into a talent's live channel, sent 3s of tone into it, and
    // destroyed it from tick(). Task 3 did not introduce that shape -- R2 had
    // the same hole against the session's channel -- but pre-provisioning
    // widened the window from one key press to the whole meeting.
    //
    // Refusing to adopt is the right disposition, not destroying and not
    // queuing: the channel is legitimately ours and in use, and THIS ladder's
    // own response has simply not arrived yet. Stay in AwaitingChannel so it
    // still can; if it never does, tick()'s existing 10s timeout settles the
    // probe with no channel id to destroy. Fail open, exactly as the arbiter
    // does elsewhere -- a probe that reports nothing costs a diagnostic, a
    // destroyed provisioned channel costs the show.
    if (!adopt_probe_channel(channelID, id)) {
        report("create_channel_response_provisioned_duplicate",
               R"("channel":")" + json_escape(id) + R"(","adopted":false)");
        return;
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

    // Task 4: nomination-issued invites -- both the initial provisioning
    // loop's (onCreateChannelResponse's Nomination branch, above) and
    // resolve_roster_change()'s later re-invites -- are correlated HERE,
    // unconditionally, before the PROBE's own m_phase check below even runs.
    // m_phase belongs to the probe's ladder and has no idea a nomination
    // invite ever happened; before this task every response to one landed
    // in that check's early return and was silently discarded, including
    // TALKBACK_ERROR_ALREADY_EXIST, which this file must treat as success,
    // not failure, and had nowhere to do that. Checked by (channel, user id)
    // against m_nomination_pending_invites, which can never collide with the
    // probe's own AwaitingInvite wait: a probe and a nomination never share
    // a channel id.
    bool nomination_handled = false;
    if (channelID) {
        std::string resolved_name;
        std::string channel_id_utf8;
        bool matched = false;
        bool confirmed = false;
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            for (auto it = m_nomination_pending_invites.begin();
                 it != m_nomination_pending_invites.end(); ++it) {
                if (it->channel_id_z != channelID || it->user_id != userID) continue;
                resolved_name = it->name;
                matched = true;
                m_nomination_pending_invites.erase(it);
                break;
            }
            if (matched) {
                // TALKBACK_ERROR_ALREADY_EXIST IS SUCCESS, not a failure. The
                // SDK documents it as "the invited user is already in the
                // channel" -- exactly the outcome a roster-driven re-invite
                // (a burst of the five roster callbacks racing this same
                // response, or Zoom's own state already reflecting an
                // earlier invite) is expected to produce. Treating it as a
                // failure here would under-count presence and could drive
                // resolve_roster_change() to keep retrying an invite that
                // already succeeded. Any OTHER non-OK error is a real gate
                // (e.g. IsSupportTalkback() == false surfacing as a rejected
                // invite) and must be reported as a failure, never swallowed.
                if (error == TALKBACK_ERROR_OK || error == TALKBACK_ERROR_ALREADY_EXIST) {
                    confirmed = true;
                    for (auto &pc : m_provisioned_channels) {
                        if (pc.channel_id_z != channelID) continue;
                        bool already = false;
                        for (const auto &m : pc.present)
                            if (m.name == resolved_name) { already = true; break; }
                        if (!already) pc.present.push_back({resolved_name, userID});
                        channel_id_utf8 = pc.channel_id;
                        break;
                    }
                } else {
                    // Fix round 1, M1 (Major): a genuine gate used to be
                    // reported and then FORGOTTEN -- the pending entry was
                    // erased above and nothing else remembered it failed, so
                    // present_here && !was_present read true again on the
                    // very next roster event and re-invited forever (up to
                    // 10 invites / 20 report lines across 5 events, proved
                    // live). `failed` marks this presence stint as resolved
                    // (attempted, and the attempt did not work) without
                    // counting as present -- resolve_roster_change() will not
                    // retry it until the person's name actually leaves the
                    // roster and comes back, the one signal that plausibly
                    // changes the outcome.
                    for (auto &pc : m_provisioned_channels) {
                        if (pc.channel_id_z != channelID) continue;
                        bool already = false;
                        for (const auto &n : pc.failed)
                            if (n == resolved_name) { already = true; break; }
                        if (!already) pc.failed.push_back(resolved_name);
                        channel_id_utf8 = pc.channel_id;
                        break;
                    }
                }
            }
        }
        if (matched) {
            nomination_handled = true;
            if (confirmed) {
                report_nomination("member_invited",
                                  R"("name":")" + json_escape(resolved_name) +
                                  R"(","channel":")" + json_escape(channel_id_utf8) +
                                  R"(","user_id":)" + std::to_string(userID) +
                                  R"(,"already_member":)" +
                                  (error == TALKBACK_ERROR_ALREADY_EXIST ? "true" : "false"));
            } else {
                // Gates are surfaced, never swallowed: a rejected nomination
                // invite (e.g. IsSupportTalkback() == false rendered as
                // TALKBACK_ERROR_NOPERMISSION/REJECTED by the SDK) is
                // reported by name here rather than left as an unexplained
                // gap in who is actually reachable.
                report_nomination("member_invite_failed",
                                  R"("name":")" + json_escape(resolved_name) +
                                  R"(","user_id":)" + std::to_string(userID) +
                                  R"(,"error":)" + std::to_string(static_cast<int>(error)));
            }
        }
    }
    if (nomination_handled) return;

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

void EngineTalkback::onChannelUserLeaveResponse(const zchar_t *channelID, unsigned int userID,
                                                TalkbackError error)
{
    report("leave_response",
           R"("channel":")" + json_escape(zchar_to_utf8(channelID)) +
           R"(","user_id":)" + std::to_string(userID) +
           R"(,"error":)" + std::to_string(static_cast<int>(error)));

    // Fix round 1, M4 (Major): the mirror image of the correlation
    // onChannelUserJoinResponse now does. Before this fix, `present` only
    // ever decremented on a MEETING departure (resolve_roster_change()'s
    // name-based diff against the roster). A CHANNEL-side removal while the
    // person stays in the meeting -- a host action, a Zoom-side eviction,
    // channel churn -- left them counted in `present`/"N of M present"
    // forever and never re-invited, because resolve_roster_change() only
    // ever sees present_here == true (they never left the MEETING) and
    // was_present == true (nothing cleared it).
    //
    // Correlated structurally by (channel, user_id) against
    // TalkbackPresentMember -- the id it stores for exactly this purpose --
    // never by re-resolving the name through the participants list, which
    // could fail anyway if the person already left the meeting entirely by
    // the time this response arrives. A leave for someone not currently
    // present (never invited, already removed, or a stray/duplicate
    // response) finds nothing and is a NO-OP: `present` is a vector sized by
    // what is actually in it, so there is no counter to underflow and no
    // separate guard needed to make "not present" and "leave it alone" the
    // same code path.
    if (!channelID || error != TALKBACK_ERROR_OK) return;

    std::string resolved_name;
    std::string channel_id_utf8;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        for (auto &pc : m_provisioned_channels) {
            if (pc.channel_id_z != channelID) continue;
            auto it = std::find_if(pc.present.begin(), pc.present.end(),
                                   [userID](const TalkbackPresentMember &m) {
                                       return m.user_id == userID;
                                   });
            if (it != pc.present.end()) {
                resolved_name = it->name;
                channel_id_utf8 = pc.channel_id;
                pc.present.erase(it);
                found = true;
            }
            break;
        }
    }
    if (found) {
        report_nomination("member_left",
                          R"("name":")" + json_escape(resolved_name) + R"(","channel":")" +
                          json_escape(channel_id_utf8) + "\"");
    }
}
void EngineTalkback::onJoinTalkbackChannel(unsigned int) {}
void EngineTalkback::onLeaveTalkbackChannel(unsigned int) {}
void EngineTalkback::onInviterAudioLevel(unsigned int, unsigned int) {}

// ── Pre-provisioned channels (Task 2, 2026-08-25) ───────────────────────────
//
// Moves channel creation from key time to nomination time -- see nominate()'s
// header declaration comment for the live-measured reason (buffers discarded
// on every key press while the create+invite round trip was in flight).
bool EngineTalkback::nominate(ZOOMSDK::IMeetingService *svc,
                              const std::vector<std::string> &nominees,
                              uint32_t attempt)
{
    // Final-review C1: every refusal below reports THIS call's own attempt id
    // -- never m_nomination_attempt, which belongs to whatever ladder is
    // still running and is precisely what a refused re-nomination must not be
    // confused with. m_nomination_attempt is claimed further down, at the
    // moment this attempt claims the ladder.
    const std::string att = attempt_field(attempt);
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
        report_nomination("nominate", R"("ok":false,"reason":"session_live")" + att);
        return false;
    }
    if (has_pending_work()) {
        report_nomination("nominate", R"("ok":false,"reason":"probe_busy")" + att);
        return false;
    }
    if (!svc) {
        report_nomination("nominate", R"("ok":false,"reason":"not_in_meeting")" + att);
        return false;
    }
    m_svc  = svc;
    m_ctrl = m_svc->GetMeetingTalkbackController();
    if (!m_ctrl) {
        report_nomination("nominate", R"("ok":false,"reason":"no_controller")" + att);
        return false;
    }
    if (!m_ctrl->IsMeetingSupportTalkBack()) {
        report_nomination("nominate", R"("ok":false,"reason":"not_supported")" + att);
        return false;
    }
    m_ctrl->SetEvent(this);

    // Fix round 1 (review, promoted from Minor): refuse a nominee whose
    // display name IS the all-talent sentinel, BEFORE anything is destroyed
    // or created. Keying that name would put a private aside on air to the
    // whole panel -- the one promise this feature makes -- and names are
    // participant-controlled, so this is provokable, not just unlucky. Refuse
    // the whole nomination and name the person: the operator can rename them
    // or drop them, and either is better than a nomination that looks fine
    // and broadcasts. Placed above the arbiter gate and the replace path so a
    // refused nomination leaves the standing set exactly as it was.
    const std::string collision = talkback_nominate_sentinel_collision(nominees);
    if (!collision.empty()) {
        report_nomination("nominate",
                          R"("ok":false,"reason":"target_name_collision","name":")" +
                          json_escape(collision) + R"(","sentinel":")" +
                          std::string(kTalkbackAllTalentTarget) + "\"" + att);
        return false;
    }

    // Fix round 4 (Major, found by the round-3 re-review): run the lazy
    // self-heal FIRST, before anything else decides this call's fate. A
    // ladder whose channel-k create response was swallowed (no Leave, no
    // cancellation: the SDK simply never called back) left the arbiter
    // claimed and channels 1..k-1 standing, so one transient SDK hiccup cost
    // the operator re-nomination for the rest of the show, recoverable only
    // by a Leave. Expiring here releases the arbiter and
    // handle_expired_create() destroys that partial set (see its comment), so
    // the arbiter gate below sees a free arbiter and a clean table rather
    // than the wreckage of a ladder nobody is waiting on any more.
    //
    // Deliberately placed AFTER m_ctrl is resolved above: the destroy it
    // may perform needs a controller for the CURRENT meeting, and reaching
    // for the previous ladder's m_ctrl would be exactly the stale-pointer
    // shape this file's mutual-exclusion guards exist to avoid. probe()
    // orders its own expiry the same way.
    TalkbackChannelOwner expired_owner;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        expired_owner = expire_stale_pending_create_locked();
    }
    handle_expired_create(expired_owner);

    // Fix round 3, "expire-path double create": bump the nomination
    // generation before issuing this ladder's first create -- see
    // src/talkback-channel-owner.h's "Generation tracking" section. A fresh
    // ladder must not be confused with an older one whose create may still be
    // floating around unresolved.
    //
    // Fix round 5 (F1, Major): this bump used to run with NO arbiter check,
    // and nothing else stood in for one -- nomination_reset() empties both
    // nomination tables while deliberately leaving the owner as Nomination,
    // so `talkback_nominate -> leave -> talkback_nominate` inside
    // kAwaitTimeout bumped the generation with a create still outstanding,
    // which is the one way to manufacture a response for a create the ladder
    // still owns that judges as Stale. The gate check here is the fix at this
    // call site; talkback_new_ladder() refusing to bump while a create is
    // outstanding is the fix that cannot be forgotten at the next one.
    //
    // Task 3 moved the gate ABOVE the plan and the replace below, so that
    // this function never destroys a standing nomination it then has to
    // refuse to replace. Order matters in one direction only: check the
    // arbiter, claim the ladder, THEN tear down and rebuild.
    //
    // LIVE GATE RUN 1 (2026-08-26): the gate now also refuses while a create
    // is merely SCHEDULED. Before the ladder was paced, "the arbiter is free"
    // and "no ladder is mid-provisioning" were the same fact -- the next
    // create was issued from inside the previous response, so there was no
    // instant in between. kNominationCreateSpacing opens a ~300ms window where
    // the arbiter is genuinely free and a ladder is genuinely still running,
    // and a re-nomination landing in it would have started a second ladder
    // over the top of the first: the first's channels destroyed by the
    // replace path with no terminal report of its own, which is the one rule
    // this feature's whole abort machinery exists to hold. Both halves are
    // read in ONE lock scope for the same reason they are refused together.
    // Reported as "create_busy" like the arbiter half, and correctly so: this
    // is nominate()'s early gate, which destroys nothing and leaves the
    // running ladder alone -- exactly what "create_busy" without
    // "channels_destroyed" already means to the plugin.
    bool create_gate_ok;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        create_gate_ok = !m_nomination_create_scheduled &&
                         talkback_may_request_create(m_pending_create.owner);
        if (create_gate_ok) m_pending_create = talkback_new_ladder(m_pending_create);
    }
    if (!create_gate_ok) {
        report_nomination("nominate", R"("ok":false,"reason":"create_busy")" + att);
        return false;
    }

    // Final-review C1: the ladder is now THIS attempt's, so every terminal
    // report it can still produce after this function returns -- "nominate_
    // done" from onCreateChannelResponse, nomination_abort_ladder()'s report
    // from any of its call sites -- must carry this attempt's id. Assigned
    // AFTER handle_expired_create() above, deliberately: the abort that
    // self-heal may emit belongs to the PREVIOUS ladder and must still carry
    // the previous ladder's id.
    m_nomination_attempt = attempt;

    // RE-NOMINATION REPLACES (Task 3). Task 2 refused here with
    // "already_provisioned" and said in as many words that Task 3 -- which
    // reworks the session path against this table -- owns deciding what a
    // re-nomination should do. It replaces, for one reason: pre-provisioning
    // makes that refusal permanent. Channels now stand for the whole meeting
    // instead of being destroyed on each key release, so under the old rule
    // the FIRST nominate() of a meeting fixed the talent list until a Leave --
    // and a talent list that cannot change mid-show is not a talent list.
    //
    // Destroying is safe HERE and nowhere earlier: a live key press
    // (m_session_live) and a probe are already refused at the top, and the
    // arbiter gate immediately above proves no create is outstanding, so
    // there is no third party holding any of these ids. m_session_channels is
    // cleared by the same call, so a selection can never outlive the channels
    // it names.
    //
    // An EMPTY nominee list therefore denominates: the standing set is
    // destroyed, the plan has no channels, and the early return below reports
    // nominate_done with zero. That is deliberate -- it gives the operator a
    // denominate without a second wire command, which is the only reason this
    // change does not also need one.
    std::size_t replaced = 0;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        replaced = m_provisioned_channels.size();
        // A leftover queue with the arbiter free means an earlier ladder
        // stopped without clearing it. Nothing is coming for those entries;
        // drop them so the fresh plan is the only queue.
        m_nomination_pending.clear();
    }
    if (replaced != 0) {
        report_nomination("replacing", R"("channels":)" + std::to_string(replaced));
        // Command-loop thread, m_chan_mtx not held -- both required, see
        // nomination_destroy_provisioned().
        nomination_destroy_provisioned();
    }

    const TalkbackPlan plan = talkback_plan(nominees);

    // Gates are surfaced, never swallowed: name every nominee the planner
    // could not fully reach BEFORE creating a single channel, not only if
    // something later fails. See src/talkback-plan.h for what each list
    // means.
    for (const auto &name : plan.uncovered_private)
        report_nomination("uncovered_private", R"("name":")" + json_escape(name) + "\"" + att);
    for (const auto &name : plan.unreachable)
        report_nomination("unreachable", R"("name":")" + json_escape(name) + "\"" + att);
    report_nomination("plan", R"("channels":)" + std::to_string(plan.channels.size()) +
                      R"(,"all_talent_complete":)" +
                      (plan.all_talent_complete ? "true" : "false") + att);

    if (plan.channels.empty()) {
        // Nothing to provision (e.g. an empty nominee list) -- not a
        // failure, just a plan with no channels. Report completion so a
        // caller waiting on "nominate_done" doesn't wait forever.
        report_nomination("nominate_done", R"("channels":0)" + att);
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
    // already-empty vector -- the replace above clears it eagerly, and
    // nomination_reset() does the same at Leave/quit) and never the plan this
    // call is about to run. nomination_create_next() does not read
    // m_nomination_pending's CONTENT to decide whether to issue CreateChannel
    // -- only whether the arbiter gate is open -- so calling it before the
    // assignment is safe; the response for the create it issues cannot be
    // delivered until this function returns and control goes back to the
    // command loop / message pump, so the queue is populated well before
    // anything could consult it.
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

    // Gate through the same arbiter probe() and nominate() use -- copy the
    // decision out under the lock, release, THEN call the SDK, same
    // discipline as every other m_chan_mtx access in this file.
    bool create_gate_ok;
    TalkbackChannelOwner expired_owner;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        // LIVE GATE RUN 1: whatever was scheduled, THIS call is it. Disarm
        // here rather than only in nomination_tick(), so the direct callers
        // (nominate() for the plan's first channel) cannot leave a stale
        // schedule armed behind them, and so a scheduled create can never be
        // issued twice.
        m_nomination_create_scheduled = false;
        expired_owner = expire_stale_pending_create_locked();
        create_gate_ok = talkback_may_request_create(m_pending_create.owner);
    }
    handle_expired_create(expired_owner);
    if (!create_gate_ok) {
        // The probe or the session is mid-create. Refuse rather than wait:
        // there is no sensible way to hold this queue open across an
        // unrelated create's whole round trip, and the plan-level ruling
        // already says nomination must not block on the arbiter.
        //
        // Fix round 2 (N1, Major), unified in fix round 3 (N6) via
        // nomination_abort_ladder(): "channels_destroyed":true is what tells
        // the plugin this is NOT the same "create_busy" nominate() itself
        // reports from its own early gate check (above, before the replace
        // step -- see this function's header comment) -- that one leaves the
        // standing set untouched; THIS one is reached only mid-ladder, after
        // nominate()'s replace already destroyed the previous set, and is
        // about to destroy whatever this ladder itself got as far as. Same
        // reason string, opposite truth about what is still standing -- the
        // plugin cannot tell those apart from "reason" alone, which is
        // exactly what left it holding a plan for destroyed channels
        // (src/talkback-nomination.h's talkback_nomination_note_failed_after_destroy()).
        nomination_abort_ladder("create_busy");
        return false;
    }

    const ZOOMSDK::SDKError e = m_ctrl->CreateChannel(1);
    report_nomination("create_channel", R"("code":)" + std::to_string(static_cast<int>(e)));

    // LIVE GATE RUN 1 (2026-08-26): SDKERR_TOO_FREQUENT_CALL is the ONE
    // synchronous failure that is not a reason to end the ladder. It is Zoom
    // saying "not yet", which is a wait, not a refusal -- and the live gate
    // proved it is the failure this ladder actually hits. Back off and retry
    // the SAME channel (m_nomination_pending's front is untouched: nothing
    // popped it, and only a successful response does). Every OTHER
    // synchronous failure keeps the terminal abort below, unchanged.
    if (e == ZOOMSDK::SDKERR_TOO_FREQUENT_CALL) {
        uint32_t retry;
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            retry = ++m_nomination_create_retries;
        }
        if (retry > kMaxNominationCreateRetries) {
            // Terminal, through the same funnel as every other abort -- but
            // with a reason that names the rate limit. A generic
            // "create_channel_failed" here would have sent the operator
            // looking at permissions and channel budget for a problem that is
            // neither (the live gate spent its first pass doing exactly that).
            nomination_abort_ladder("create_rate_limited");
            return false;
        }
        const std::chrono::milliseconds delay =
            kNominationRateLimitBackoff * (1u << (retry - 1));
        // Stage name deliberately distinct from the terminal abort's
        // "create_rate_limited" REASON: one says "waiting, will retry", the
        // other says "gave up". A log where those two read the same is a log
        // that cannot answer the only question the operator has.
        report_nomination("create_rate_limited_retry",
                          R"("retry":)" + std::to_string(retry) +
                          R"(,"max_retries":)" + std::to_string(kMaxNominationCreateRetries) +
                          R"(,"backoff_ms":)" + std::to_string(delay.count()));
        nomination_schedule_create(delay);
        // TRUE, not false: the ladder is alive and a retry is armed. This is
        // the one return path where true does not mean "a create is
        // outstanding" -- see the declaration comment. nominate() relies on
        // it: a first create that is merely rate-limited must still let
        // nominate() assign the plan into m_nomination_pending, or the retry
        // would fire against an empty queue.
        return true;
    }

    if (e != ZOOMSDK::SDKERR_SUCCESS) {
        // Fix round 2 (N1, Major), unified in fix round 3 (N6): this branch
        // used to report ONLY the "create_channel" diagnostic line above,
        // with the SDK error code -- a stage line, not a terminal outcome.
        // nomination_abort_ladder() is the terminal report every abort path
        // must have, for the same reason as the create_busy branch above.
        nomination_abort_ladder("create_channel_failed");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        // Claiming the arbiter and stamping the create with the current
        // generation are ONE transition (fix round 5) -- they used to be two
        // adjacent assignments, and a re-review mutation showed that either
        // could be deleted alone with the whole suite still green. The stamp
        // OVERWRITES the one outstanding slot rather than queuing behind it
        // (fix round 4): anything already there belonged to a create the
        // ladder abandoned, so there is nothing to reconcile and nothing
        // that can desynchronise.
        m_pending_create = talkback_create_issued(m_pending_create,
                                                  TalkbackChannelOwner::Nomination);
        m_nomination_create_deadline.store(
            (std::chrono::steady_clock::now() + kAwaitTimeout).time_since_epoch().count(),
            std::memory_order_release);
    }
    return true;
}

void EngineTalkback::nomination_schedule_create(std::chrono::milliseconds delay)
{
    // See the declaration comment for the live failure this exists for, and
    // for why arming this deadline is deliberately NOT an arbiter claim.
    std::lock_guard<std::mutex> lock(m_chan_mtx);
    m_nomination_create_scheduled = true;
    m_nomination_next_create_at = std::chrono::steady_clock::now() + delay;
}

void EngineTalkback::nomination_tick()
{
    // The whole pump. Deliberately tiny: everything about WHICH channel and
    // WHETHER the arbiter allows it already lives in nomination_create_next(),
    // and duplicating any of it here is how the two would drift.
    //
    // Consume the schedule under the lock BEFORE issuing, never after: the
    // issue path calls the SDK (which must not happen under m_chan_mtx) and
    // can itself re-arm the schedule on a rate-limit retry. Clearing after
    // would wipe that fresh arm and stall the ladder silently.
    bool due;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        due = m_nomination_create_scheduled &&
              std::chrono::steady_clock::now() >= m_nomination_next_create_at;
        if (due) m_nomination_create_scheduled = false;
    }
    if (!due) return;
    // Return value deliberately unused: every false path inside has already
    // emitted its own terminal report through nomination_abort_ladder(), and
    // there is no caller here to tell -- unlike main.cpp's nominate() call
    // site, which keeps a diagnostic backstop for exactly that reason.
    (void)nomination_create_next();
}

void EngineTalkback::debug_expire_create_spacing_for_test()
{
    // TEST-ONLY, no production call site. Only moves the deadline into the
    // past -- nomination_tick() is what issues, exactly as the real pump
    // would. Same discipline as debug_expire_pending_create_for_test() and
    // debug_expire_pending_invites_for_test().
    std::lock_guard<std::mutex> lock(m_chan_mtx);
    m_nomination_next_create_at = std::chrono::steady_clock::now();
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
        // Task 4: any invite this ladder is still waiting to hear back about
        // belongs to a channel that no longer exists in a moment -- forget
        // it, same reasoning as m_session_channels.clear() just below: a
        // pending invite must never outlive the channel it names.
        m_nomination_pending_invites.clear();
        // Task 3: a selection must never outlive the channels it names.
        // Clearing it in the SAME lock scope that empties the table means
        // "selected a destroyed channel" is not a state that exists, rather
        // than one that is argued to be unreachable. drain_audio() would then
        // count no_channel_drops, which is loud, instead of sending into a
        // channel Zoom no longer has, which is silent.
        //
        // AND THE SELECTION CAN BE A LIVE ONE (final review, C2, CRITICAL).
        // This comment used to read "every caller of this function has
        // already ruled out a live key press ... so this is normally already
        // empty", which was false and was the belief that hid the Critical:
        // nominate()'s replace path does rule it out (nominate() refuses
        // outright while m_session_live), but nomination_abort_ladder() does
        // NOT -- its ladder started before the key and aborts after it. The
        // live case is handled by that caller, which stops the session and
        // reports live:false around this call. What this line owns is the
        // structural guarantee, not a claim about who calls it.
        m_session_channels.clear();
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

void EngineTalkback::nomination_abort_ladder(const std::string &reason)
{
    // See the header comment: this exists so a ladder-ending teardown cannot
    // happen without the terminal report that goes with it. Clear the
    // not-yet-created queue first (same ordering every existing call site
    // already used), then destroy whatever WAS provisioned -- harmless as a
    // no-op when there is nothing to destroy, which is the common case for a
    // ladder that never got past channel 1.
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        m_nomination_pending.clear();
        // LIVE GATE RUN 1: disarm the pacing schedule in the same breath as
        // the queue it paces. Every terminal abort funnels through here (that
        // is this function's entire reason to exist), so a dead ladder's
        // scheduled create cannot fire into whatever comes next -- which,
        // after an abort, is usually the operator's re-nomination. Retries go
        // with it: a fresh ladder starts with a full budget.
        m_nomination_create_scheduled = false;
        m_nomination_create_retries = 0;
    }

    // FINAL REVIEW, C2 (CRITICAL). A key may be LIVE right now: session_start()
    // gates only on `still_coming` for ITS OWN target, so keying "all" while
    // the private channels are still being created is legal and deliberate
    // (see the argument at session_start()'s "NO ARBITER GATE HERE"). The
    // destroy below takes down EVERY provisioned channel and empties the
    // selection, so that press is about to be talking into nothing -- and
    // nothing else in this file reports session state after a session has
    // gone live, so before this fix m_session_live stayed true, the plugin's
    // TalkbackSessionStatus.live stayed true, evaluate() saw no reason to
    // close, the tally stayed red and the OPEN cue had already played. Zero
    // audio, no signal at the desk.
    //
    // Decide it HERE, under m_chan_mtx and BEFORE the destroy, because
    // nomination_destroy_provisioned() clears m_session_channels itself: after
    // it runs there is nothing left to compare. The SDK calls and both reports
    // stay outside the lock, per this file's standing discipline.
    bool orphans_live_session = false;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        if (m_session_live) {
            for (const auto &sel : m_session_channels) {
                for (const auto &pc : m_provisioned_channels) {
                    if (pc.channel_id_z == sel) { orphans_live_session = true; break; }
                }
                if (orphans_live_session) break;
            }
        }
    }
    if (orphans_live_session) {
        // BEFORE the destroy, so session_stop() restores the key-down duck on
        // channels that still exist (SetChannelBackgroundVolume on a destroyed
        // channel restores nothing, and the talent would be left hearing the
        // meeting at 30% for the rest of the show), and so sending stops
        // before the ids underneath it go away. session_stop() is the
        // existing "stop sending, clear the selection, restore the duck, reset
        // the flags" function -- reused rather than re-implemented, because a
        // second hand-written copy of that teardown is how this file's
        // siblings have drifted before.
        session_stop();
    }

    nomination_destroy_provisioned();

    if (orphans_live_session) {
        // The plugin's TalkbackController::evaluate() closes the key on any
        // live:false carrying a non-empty reason (its `explicit_failure`
        // path), plays the CLOSE cue on the live edge, and shuts the tap. A
        // reason of its own, not a generic one: the operator needs to be able
        // to tell "the channels you were talking on were destroyed by a
        // failing nomination" from "your key never came up".
        report_session_state(false, "channels_destroyed");
    }

    // Reported AFTER the destroy above -- outside m_chan_mtx either way, but
    // this ordering means "channels_destroyed":true is "it's actually gone
    // now", not merely "about to be". `reason` is always an engine-authored
    // literal today (never participant-controlled), but json_escape() costs
    // nothing and keeps this call site honest if that ever stops being true.
    report_nomination("nominate",
                      R"("ok":false,"reason":")" + json_escape(reason) +
                      R"(","channels_destroyed":true)" +
                      attempt_field(m_nomination_attempt));
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
    if (e != ZOOMSDK::SDKERR_SUCCESS) return;

    // Task 4: ExecuteBatchInviteUsers is asynchronous -- its own SDK doc
    // comment says the synchronous SDKERR_SUCCESS above is only "the batch
    // call was accepted", never "the user is in the channel". Track this so
    // onChannelUserJoinResponse's eventual response -- including
    // TALKBACK_ERROR_ALREADY_EXIST, which this file treats as success, never
    // a failure -- can be attributed back to this NAME (never an id, same
    // rule as everywhere else in this file) and update this channel's
    // `present` set, which is what session_start()'s "N of M present" line
    // and resolve_roster_change()'s own idempotence both read.
    //
    // Fix round 1, C1: stamped with a deadline (kAwaitTimeout, same bound
    // every other "did Zoom actually answer" wait in this file uses) so
    // resolve_roster_change()'s sweep can forget an entry whose response is
    // never coming -- see m_nomination_pending_invites' header comment for
    // why an un-expiring entry here permanently suppressed a rejoin.
    std::lock_guard<std::mutex> lock(m_chan_mtx);
    m_nomination_pending_invites.push_back(
        {channel_id_z, uid, name, std::chrono::steady_clock::now() + kAwaitTimeout});
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
    // fixed for Session before Task 3 removed the session's create
    // altogether (see m_pending_create's header comment); Nomination had
    // reintroduced the unfixed version, and Nomination still creates.
    //
    // Fix: leave m_pending_create AS Nomination (so the eventual response is
    // still routed to the Nomination branch in onCreateChannelResponse, not
    // lost to "owner == None") and set m_pending_create.nomination_cancelled
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
    // m_pending_create.nomination_cancelled directly. It was the same
    // function session_stop()'s early branch called for Session, so the two
    // owners' cancellation logic could not diverge the way their EXPIRY logic
    // did (N1); Task 3 left it the only caller, and it stays routed through
    // the shared transition for the reason the header gives -- an inlined
    // per-owner copy is how N1 happened in the first place.
    std::lock_guard<std::mutex> lock(m_chan_mtx);
    m_pending_create = talkback_cancel(m_pending_create, TalkbackChannelOwner::Nomination);
    m_nomination_pending.clear();
    // LIVE GATE RUN 1: the pacing schedule is forgotten alongside the queue it
    // paces, for the same reason everything else here is -- this runs on
    // Leave/quit, and a create scheduled against a meeting we have left must
    // never be issued. Unlike an OUTSTANDING create (which is cancelled rather
    // than forgotten, because Zoom will still answer it -- see the long
    // comment above), a scheduled one has not reached Zoom at all, so
    // dropping it really is the end of it.
    m_nomination_create_scheduled = false;
    m_nomination_create_retries = 0;
    m_provisioned_channels.clear();
    // Task 4: same reasoning as nomination_destroy_provisioned() -- a
    // pending invite naming a channel that no longer exists (meeting-scoped,
    // like everything else this function forgets) must not outlive it.
    m_nomination_pending_invites.clear();
    // Task 3: the key press's selection is made of ids from the table above,
    // so it is forgotten in the same breath -- same reasoning as
    // nomination_destroy_provisioned(). main.cpp's Leave path calls
    // session_stop() just before this, which already clears it; doing it here
    // too costs nothing and means this function cannot leave a selection
    // pointing into a table it just emptied, whatever order a future caller
    // uses.
    m_session_channels.clear();
}

// ── Roster re-resolution (Task 4, 2026-08-25) ───────────────────────────────
std::vector<EngineTalkback::TalkbackRosterEntry> EngineTalkback::current_roster() const
{
    std::vector<TalkbackRosterEntry> roster;
    if (!m_svc) return roster;
    auto *part = m_svc->GetMeetingParticipantsController();
    if (!part) return roster;
    ZOOMSDK::IList<unsigned int> *ids = part->GetParticipantsList();
    if (!ids) return roster;
    roster.reserve(static_cast<std::size_t>(ids->GetCount()));
    for (int i = 0; i < ids->GetCount(); ++i) {
        const unsigned int uid = ids->GetItem(i);
        ZOOMSDK::IUserInfo *u = part->GetUserByUserID(uid);
        if (!u) continue;
        roster.push_back({uid, zchar_to_utf8(u->GetUserName())});
    }
    return roster;
}

void EngineTalkback::resolve_roster_change(ZOOMSDK::IMeetingService *svc)
{
    if (!svc) return;

    // THE RULING THIS FUNCTION EXISTS UNDER (see the header comment): invite
    // only, NEVER create. Every provisioned channel already exists from
    // nomination time; a name with nowhere to go is a planning gap for the
    // operator to fix with nominate(), not something this function may fix
    // by calling CreateChannel -- CreateChannel is command-loop-thread-only
    // under the arbiter's single-outstanding-create rule
    // (src/talkback-channel-owner.h), and nothing below calls it.

    // Fix round 1, m1 (Minor): checked BEFORE has_pending_work() so a
    // meeting that has never nominated anyone -- the common case for most of
    // a meeting's life -- never emits "roster_resolve probe_busy" during an
    // unrelated ~30s probe. There is nothing here to be busy ABOUT.
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        if (m_provisioned_channels.empty()) return;   // nothing nominated yet
    }

    // Gated exactly like nominate()/session_start(): when it INVITES, this
    // runs the same kind of Begin/Add/Execute sequence tick()'s own
    // inventory documents as unsafe to interleave, on different threads,
    // with the probe's driving thread.
    //
    // FINAL REVIEW, M1 (Major) -- THIS GATE IS NARROWED, not an early return
    // any more, and the comment that used to sit here ("a refusal here costs
    // nothing but a delay ... the next roster event gets another chance") was
    // only ever true of the INVITE half. It is false of departures. An invite
    // is a pending entry that persists, so a refused resolution genuinely
    // does get another chance at it. A departure is an EDGE, detected by
    // diffing this file's tables against a LIVE roster snapshot -- and the
    // next roster event compares against the roster as it is THEN. So a
    // refusal does not delay that edge, it destroys it: a talent who leaves
    // and rejoins under a new user id during a talkback probe (up to ~30s of
    // has_pending_work()) stayed "present" under their DEAD id for the rest
    // of the meeting, never re-invited, while session_start() cheerfully
    // reported "1 of 1 present" for someone who could hear nothing.
    //
    // So the roster snapshot, the pending-invite prune and the whole
    // departure/presence diff below now run REGARDLESS -- they touch only
    // this file's own tables under m_chan_mtx and call no talkback SDK API --
    // and only the invite issuance is gated. Reading the participants list is
    // not new exposure: main.cpp's rebuild_roster() already reads exactly
    // that, on every one of the same roster callbacks, probe or no probe.
    // Hoisting the prune above the gate instead would have left the
    // FAILED-invite clear and the re-invite decision on the wrong side of it,
    // which is the same edge in a different place.
    const bool invites_allowed = !has_pending_work();
    if (!invites_allowed)
        report_nomination("roster_resolve", R"("ok":false,"reason":"probe_busy","pruned":true)");

    // Fix round 1, M3 (Major): m_svc/m_ctrl are reassigned ONLY when no
    // session is live. probe() and nominate() both refuse OUTRIGHT while
    // m_session_live because they touch these exact fields; this function
    // cannot refuse outright (a rejoin mid-press must still be invited), so
    // it leaves the press's own pointers alone instead. Without this, a
    // roster event firing mid-press (anybody muting, unmuting, joining,
    // leaving or renaming does) could have GetMeetingTalkbackController()
    // return null for one call -- a meeting reconnect/ending state -- and
    // null m_ctrl for the REST of the press: drain_audio() then snapshots a
    // null ctrl into every subsequent buffer's SendCtx (counted as
    // no_channel_drops, so not silent -- but the director is mid-sentence
    // and off air), and session_stop()'s `!m_ctrl` bail then also skips
    // restoring the duck, stranding the talent at 30% meeting audio for the
    // rest of the show. A live press already proved m_ctrl valid at
    // session_start() time; reusing it is strictly safer than re-querying it
    // here.
    //
    // M1: and ONLY when invites are allowed. The pointers belong to whatever
    // subsystem has_pending_work() is reporting; the pruning half below needs
    // neither of them (current_roster() reads the PARTICIPANTS controller,
    // and every table it touches is ours), so a probe-busy pass must leave
    // them exactly as it found them.
    if (m_session_live || !invites_allowed) {
        if (!m_ctrl && invites_allowed)
            return;            // paranoia only -- session_start() would not
                               // have gone live without a valid m_ctrl
    } else {
        m_svc  = svc;
        m_ctrl = m_svc->GetMeetingTalkbackController();
        if (!m_ctrl) return;
    }

    const std::vector<TalkbackRosterEntry> roster = current_roster();
    auto is_present_now = [&roster](const std::string &name) {
        for (const auto &r : roster) if (r.name == name) return true;
        return false;
    };
    auto uid_in_roster = [&roster](unsigned int uid) {
        for (const auto &r : roster) if (r.user_id == uid) return true;
        return false;
    };

    // Snapshot the work under the lock -- the SDK is never called while
    // holding m_chan_mtx, same discipline as everywhere else in this file.
    // Departures are applied to `pc.present`/`pc.failed` in THIS scope (so a
    // concurrent reader, e.g. session_start()'s own present-count read,
    // never sees a half-updated table); invites happen AFTER the lock is
    // released, below.
    struct ChannelInvites {
        std::basic_string<zchar_t> channel_id_z;
        std::string channel_id;
        std::vector<std::string> names;
    };
    std::vector<ChannelInvites> to_invite;
    std::vector<std::pair<std::string, std::string> > left;             // (name, channel)
    std::vector<std::pair<std::string, std::string> > expired_invites;  // (name, reason)
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);

        // Fix round 1, C1 (CRITICAL): prune every pending invite this file
        // can never hear a USEFUL answer for, BEFORE the per-channel diff
        // below reads m_nomination_pending_invites to decide what is
        // "already outstanding". Two independent triggers -- see
        // m_nomination_pending_invites' header comment for the two
        // triggering sequences this closes and why one alone leaves the
        // other open:
        //   * TIMED OUT -- the response is never delivered at all (this SDK
        //     swallows responses; kAwaitTimeout is the same bound
        //     tick()'s AwaitingChannel timeout and
        //     expire_stale_pending_create_locked() use for the identical
        //     reason on the CREATE side).
        //   * LEFT THE ROSTER -- the id this invite was issued to is no
        //     longer in the meeting at all. This is the semantically RIGHT
        //     trigger (a response for an id that no longer exists cannot
        //     mean anything useful) and fires immediately rather than after
        //     up to kAwaitTimeout.
        //
        // Blast radius, accepted rather than hidden (re-review residual 3):
        // a TRANSIENT empty roster (current_roster() returning {} because
        // GetMeetingParticipantsController()/GetParticipantsList() answered
        // null for one call -- the same convention the diff loop below
        // already treats as "everybody left") makes uid_in_roster() false
        // for every outstanding uid, pruning the WHOLE pending table in one
        // pass. This is intentional, not a gap: the consequence is one round
        // of re-invites on the next good roster event, answered
        // TALKBACK_ERROR_ALREADY_EXIST for anyone genuinely still present
        // (see that error's handling above -- treated as success, deduped by
        // name), so it self-heals rather than losing or double-counting
        // anyone.
        const auto now = std::chrono::steady_clock::now();
        for (auto it = m_nomination_pending_invites.begin();
             it != m_nomination_pending_invites.end(); ) {
            const bool timed_out = now >= it->deadline;
            const bool uid_left  = !uid_in_roster(it->user_id);
            if (timed_out || uid_left) {
                expired_invites.emplace_back(it->name, uid_left ? "left_meeting" : "timeout");
                it = m_nomination_pending_invites.erase(it);
            } else {
                ++it;
            }
        }

        for (auto &pc : m_provisioned_channels) {
            // FINAL REVIEW, M1 (Major): PRUNE `present` BY USER ID, mirroring
            // the pending-invite prune above, and do it BEFORE the per-name
            // diff so the same pass can re-invite.
            //
            // Channel membership is per user id: a talent who leaves and
            // rejoins gets a NEW id and is NOT in the channel. The diff below
            // matches by NAME only (`is_present_now(name)`), so if no
            // resolution observed the roster while the name was absent --
            // a fast rejoin between two events, or, before this round's other
            // half, any departure during a ~30s probe -- then present_here
            // and was_present were BOTH true, no departure fired, no
            // re-invite was ever issued, and members_present_locked() counted
            // a dead id for the rest of the meeting. "1 of 1 present" for
            // someone who hears nothing is the exact failure this feature is
            // written against.
            //
            // TalkbackPresentMember stores the id for precisely this reason
            // (it is a per-stint correlation key, never persisted past the
            // meeting); this is the check that was missing, not new state.
            // Same accepted blast radius as the invite prune above: a
            // TRANSIENT empty roster prunes everyone and costs one round of
            // re-invites, answered TALKBACK_ERROR_ALREADY_EXIST for anyone
            // genuinely still there.
            //
            // WHERE THE MIRROR STOPS, stated so the next reader does not
            // assume coverage that is not here (verification round). `present`
            // and m_nomination_pending_invites both carry a user_id, so both
            // can be pruned by one. `pc.failed` carries NAMES ONLY -- it is
            // the "this person was invited once this presence stint and
            // permanently refused" list (fix round 1, M1) -- so it cannot be
            // uid-pruned, and the departure branch below, which is the only
            // thing that clears it, needs the NAME to be absent. A talent
            // whose invite failed permanently and who then rejoins under a new
            // uid WITHOUT any resolution observing them gone therefore keeps
            // their `failed` entry and is not retried for the rest of the
            // meeting. That is bounded and it stays HONEST, which is why it is
            // documented rather than fixed here: they were pruned out of
            // `present` by the loop above, so the count reads "0 of 1" -- a
            // shortfall the operator is shown, not a claim of presence for
            // someone who hears nothing, which is the failure M1 was actually
            // about. Recovery is a re-nomination (which rebuilds the table).
            for (auto it = pc.present.begin(); it != pc.present.end(); ) {
                if (uid_in_roster(it->user_id)) { ++it; continue; }
                left.emplace_back(it->name, pc.channel_id);
                it = pc.present.erase(it);
            }

            ChannelInvites work;
            for (const auto &name : pc.members) {
                const auto present_it = std::find_if(
                    pc.present.begin(), pc.present.end(),
                    [&name](const TalkbackPresentMember &m) { return m.name == name; });
                const bool was_present = present_it != pc.present.end();
                const bool was_failed =
                    std::find(pc.failed.begin(), pc.failed.end(), name) != pc.failed.end();
                const bool present_here = is_present_now(name);
                if (present_here && !was_present && !was_failed) {
                    // Idempotence: skip a name that already has an invite
                    // outstanding. A burst of the five roster callbacks for
                    // the SAME join must issue ONE ExecuteBatchInviteUsers,
                    // not one per callback -- TALKBACK_ERROR_ALREADY_EXIST
                    // covers Zoom's side of a redundant invite, this covers
                    // ours, and "make the work proportional to what actually
                    // changed" means not relying on Zoom's answer alone.
                    // `was_failed` is the OTHER half (fix round 1, M1): a
                    // permanently-gated invite must not be retried on every
                    // roster event either, including the two of the five
                    // callbacks (onUserAudioStatusChange,
                    // onUserVideoStatusChange) that fire on every mute and
                    // camera toggle by anyone in the meeting, not just on an
                    // actual roster change.
                    bool already_pending = false;
                    for (const auto &pi : m_nomination_pending_invites) {
                        if (pi.channel_id_z == pc.channel_id_z && pi.name == name) {
                            already_pending = true;
                            break;
                        }
                    }
                    if (!already_pending) work.names.push_back(name);
                } else if (!present_here && (was_present || was_failed)) {
                    // A talent renaming themselves is a LEAVE of the old
                    // name (handled here, generically -- no special case)
                    // and possibly a JOIN of a nominated new name, handled
                    // by the `present_here && !was_present` branch above on
                    // this SAME pass, for whichever channel plans the new
                    // name.
                    //
                    // Fix round 1, M1: clearing `failed` HERE -- on an
                    // actual roster departure, never on a timer -- is the
                    // "retry only on that person's join transition" ruling.
                    // A permanently-gated client is invited exactly ONCE per
                    // presence stint and gets a fresh attempt only once they
                    // actually leave and rejoin.
                    if (was_present) {
                        pc.present.erase(present_it);
                        left.emplace_back(name, pc.channel_id);
                    }
                    pc.failed.erase(std::remove(pc.failed.begin(), pc.failed.end(), name),
                                    pc.failed.end());
                }
            }
            if (!work.names.empty()) {
                work.channel_id_z = pc.channel_id_z;
                work.channel_id = pc.channel_id;
                to_invite.push_back(std::move(work));
            }
        }
    }

    for (const auto &e : expired_invites)
        report_nomination("pending_invite_expired",
                          R"("name":")" + json_escape(e.first) + R"(","reason":")" +
                          e.second + "\"");

    for (const auto &l : left)
        report_nomination("member_left",
                          R"("name":")" + json_escape(l.first) + R"(","channel":")" +
                          json_escape(l.second) + "\"");

    // M1: the one half that is still gated on has_pending_work(). Nothing
    // above marked any of these names as resolved, so a pass that skips this
    // genuinely does get another chance at them on the next roster event --
    // the claim the old early-return comment made about the whole function,
    // which is true here and only here.
    if (invites_allowed)
        for (const auto &work : to_invite)
            for (const auto &name : work.names)
                invite_nominee(work.channel_id_z, work.channel_id, name);
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
    // Every attempt starts from a clean verdict -- see m_audio_fail_reason's
    // header comment for why "failed" and "not attempted yet" must stay
    // distinguishable. close_audio() above already clears it; this is the
    // statement that makes that independent of what close_audio() does.
    m_audio_fail_reason.clear();
    if (!shm_region_open_readwrite(
            m_audio_region, region_name,
            shm_audio_region_bytes(kTalkbackSlotBytes))) {
        report_session("audio_open", R"("ok":false,"reason":"map_failed","region":")" +
                       json_escape(region_name) + "\"");
        m_audio_fail_reason = "map_failed";   // session_start() refuses on this
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
        m_audio_fail_reason = "layout_mismatch";   // session_start() refuses on this
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
        m_audio_fail_reason = "unsupported_rate";   // session_start() refuses on this
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
        m_audio_fail_reason = "unsupported_channels";   // session_start() refuses on this
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
        m_audio_fail_reason = "pipe_header_mismatch";   // session_start() refuses on this
        report_session_state(false, "pipe_header_mismatch");
        // F5 review-round fix: leave the mapping open -- see the comment on
        // the layout_mismatch rejection above.
        return false;
    }

    m_audio_region_name = region_name;
    m_audio_rate        = hdr_rate;
    m_audio_channels    = hdr_channels;
    // START AT 0, NOT AT THE WRITER'S CURRENT INDEX (Task 3 fix round 2).
    //
    // This line used to read `m_audio_read_index = hdr->write_index;` with the
    // reasoning "buffers published before we mapped are stale by definition".
    // That reasoning is inherited from the MAIN audio ring, where the region
    // outlives many subscriptions and a late reader genuinely would replay
    // somebody else's old audio. It is false for THIS ring: TalkbackTap::open()
    // lays the region out and calls talkback_ring_init() -- which memsets the
    // header and sets write_index = 0 -- on every key press, and only THEN
    // sends talkback_open and attaches its OBS capture callback
    // (src/talkback-tap.cpp). So every buffer at an index below the writer's
    // current one belongs to THIS press and is the director's first syllable,
    // not stale audio.
    //
    // Snapping past it was therefore a discard, not a de-staling: the residual
    // window between the tap attaching its callback and this engine getting to
    // run open_audio() -- one pipe write plus one command-loop turn. Small
    // (fix round 1 already removed all the SDK work that used to sit in it) but
    // real, uninstrumented, and the exact failure this milestone exists to
    // remove. Reading from 0 closes it: the ring's 8 slots become a buffer for
    // that window instead of a wall, and an overrun past those slots is
    // COUNTED (`lost` in drain_audio's audio_send report) rather than silent.
    //
    // That last clause was FALSE when this comment was first written, and the
    // closing sweep caught it: talkback_ring_drain() skipped a lapped reader
    // forward without touching `lost`, which counted only seqlock give-ups.
    // The claim is true now because the skip was made to count what it steps
    // over (src/talkback-ring.h) -- the fix went into the code rather than
    // into the sentence, because a lapped ring is the LARGER of the two losses
    // and was the one nothing reported.
    //
    // Do NOT "restore" the snap here on the strength of the general rule. It
    // remains correct for any ring that can be PRE-EXISTING when a reader
    // arrives; it is wrong for one the writer re-initialises per press. If a
    // future change makes this region survive across presses without a fresh
    // talkback_ring_init(), the snap has to come back with it.
    m_audio_read_index = 0;

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
    // Not a report_session_state(true, ...) call: "live" is the SELECTION's
    // confirmed state, not the audio path's. Final review, m2: this used to
    // say "set in onCreateChannelResponse once the invite is accepted", which
    // has been false since Task 3 deleted the session's own CreateChannel and
    // the invite on the key path -- there is exactly ONE live:true in this
    // file, session_start()'s success line, and believing otherwise is what
    // hid C2 (nothing reported state after a session went live). This session
    // can be fully
    // open_audio()-ready while the channel itself never came up, and vice
    // versa (channel live before the plugin ever calls talkback_open) --
    // conflating the two would let a working audio pipe into a nonexistent
    // channel read as "live".
    return true;
}

namespace {
struct SendCtx {
    ZOOMSDK::IMeetingTalkbackController *ctrl;
    // Task 3: every channel serving the keyed target, not one. Points at a
    // vector the caller owns for the whole drain, built from ids copied out
    // under m_chan_mtx and released before this runs -- the SDK is never
    // called while that lock is held.
    const std::vector<const zchar_t *> *channels;
    uint32_t rate;
    ZOOMSDK::ZoomSDKAudioChannel chan;
    uint32_t sent;               // successful sends, counted per channel
    uint32_t failed;             // failed sends, counted per channel
    uint32_t no_channel_drops;   // buffers seen while ctrl/selection is unset
    int last_err;
};

void send_one(const void *pcm, uint32_t byte_len, uint64_t, void *ctx)
{
    auto *c = static_cast<SendCtx *>(ctx);
    // No channel selected (no key press is live, or the target was refused,
    // or a re-nomination destroyed the selection mid-press) -- there is
    // nowhere to send this buffer. That is a real choice, not a bug, but per
    // this codebase's rule against silent audio loss it must still be
    // counted, not just dropped -- see the no_channel_drops report below.
    if (!c->ctrl || !c->channels || c->channels->empty()) {
        ++c->no_channel_drops;
        return;
    }
    // THE SAME PCM TO EVERY CHANNEL OF THE TARGET, in one pass. An all-talent
    // target with more than 10 people owns ceil(n/10) channels because the SDK
    // caps a channel at 10 users; sending to only the first would leave
    // everybody from the eleventh person on hearing silence, with the
    // director believing the whole panel is listening. A partial failure does
    // NOT abort the rest of the fan-out: one channel refusing must not
    // silence the others.
    for (const zchar_t *channel : *c->channels) {
        const ZOOMSDK::SDKError e = c->ctrl->SendAudioDataToChannel(
            channel, static_cast<const char *>(pcm), byte_len, c->rate, c->chan);
        if (e != ZOOMSDK::SDKERR_SUCCESS) {
            c->last_err = static_cast<int>(e);
            ++c->failed;
        } else {
            ++c->sent;
        }
    }
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

    // The channels to talk on are the SESSION's SELECTION (Task 3) -- the
    // provisioned channels session_start() matched to the keyed target --
    // never the probe's m_channel_id_z, which tick() destroys from a separate
    // thread three seconds after it opens; sending on that would race the
    // destroy mid-SendAudioDataToChannel. Nothing tick() touches is reachable
    // from here, so that race stays structurally impossible rather than
    // merely unlikely.
    //
    // Copy the ids out under m_chan_mtx, release, THEN call the SDK -- the
    // one rule this file never bends. `channels` owns the strings for the
    // whole drain and `ptrs` borrows from it, so both must (and do) outlive
    // the talkback_ring_drain() calls below. No channel id crosses the IPC
    // boundary and nothing here needs a UTF-8 -> zchar_t conversion.
    std::vector<std::basic_string<zchar_t>> channels;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        channels = m_session_channels;
    }
    std::vector<const zchar_t *> ptrs;
    ptrs.reserve(channels.size());
    for (const auto &c : channels) ptrs.push_back(c.c_str());

    auto *hdr = static_cast<ShmAudioHeader *>(m_audio_region.ptr);

    SendCtx ctx{m_ctrl, &ptrs,
                m_audio_rate,
                m_audio_channels > 1 ? ZOOMSDK::ZoomSDKAudioChannel_Stereo
                                     : ZOOMSDK::ZoomSDKAudioChannel_Mono,
                0, 0, 0, 0};

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

    // Fix round 1, M4: the key-down duck, applied AFTER this pass's sends and
    // never before them. session_start() only arms it -- see the comment there
    // for why. Ordering within this function is the whole point: the first
    // buffers are already on their way to Zoom by the time these calls run,
    // and nothing the duck does can delay them. Deliberately not gated on ctx.sent -- a drain that
    // found nothing still means the ring is live and the press is real, and
    // waiting for a buffer that may not come would leave the duck unapplied
    // for the whole press.
    if (m_session_duck_pending) {
        m_session_duck_pending = false;
        if (m_ctrl && !channels.empty()) {
            for (const auto &id : channels)
                m_ctrl->SetChannelBackgroundVolume(id.c_str(), 0.3f);
            m_session_ducked = true;
            report_session("audio_duck", R"("channels":)" +
                           std::to_string(channels.size()));
        }
    }

    if (ctx.failed != 0 || lost != 0 || ctx.no_channel_drops != 0) {
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
            // "sent"/"failed" count SENDS, not buffers: with a fanned-out
            // all-talent target one buffer is several sends, and reporting
            // buffers here would hide "three of four channels are refusing"
            // behind a healthy-looking count. "channels" is what makes the
            // two readable together.
            report_session("audio_send", R"("code":)" + std::to_string(ctx.last_err) +
                   R"(,"channels":)" + std::to_string(ptrs.size()) +
                   R"(,"sent":)" + std::to_string(ctx.sent) +
                   R"(,"failed":)" + std::to_string(ctx.failed) +
                   R"(,"lost":)" + std::to_string(lost) +
                   R"(,"no_channel_drops":)" + std::to_string(ctx.no_channel_drops) +
                   R"(,"occurrence":)" + std::to_string(m_audio_send_fail_count));
        }
    }
}

// ── Persistent talkback session (Milestone 5; SELECT-ONLY since Task 3) ─────
//
// Deliberately NOT part of the probe's Phase machine above: that machine
// exists to tear itself down after one tone, which is the opposite of what a
// key held down needs. The session never sends on the probe's channel
// (m_channel_id_z), which tick() destroys from a separate thread, so that
// SendAudioDataToChannel/destroy race stays structurally impossible rather
// than merely unlikely.
//
// TASK 3: KEYING SELECTS, IT NEVER CREATES. Everything below reads
// m_provisioned_channels and calls no create, no invite, and nothing that
// waits for a Zoom response. That is the milestone: the create+invite round
// trip that used to run between the key going down and the first buffer
// leaving is what discarded the director's first words on every press
// (measured live 2026-08-25 as no_channel_drops), and the fix is not to make
// it faster but to have already done it, at nomination time.
bool EngineTalkback::session_live() const { return m_session_live; }

void EngineTalkback::members_present_locked(const std::string &target,
                                             std::size_t *present,
                                             std::size_t *total) const
{
    std::size_t p = 0, t = 0;
    for (const auto &pc : m_provisioned_channels) {
        if (!talkback_channel_serves_target(pc.is_all_talent, pc.members, target))
            continue;
        t += pc.members.size();
        p += pc.present.size();
    }
    if (present) *present = p;
    if (total) *total = t;
}

void EngineTalkback::members_present_for_target(const std::string &target,
                                                 std::size_t *present,
                                                 std::size_t *total) const
{
    std::lock_guard<std::mutex> lock(m_chan_mtx);
    members_present_locked(target, present, total);
}

void EngineTalkback::debug_expire_pending_invites_for_test()
{
    std::lock_guard<std::mutex> lock(m_chan_mtx);
    const auto past = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    for (auto &pi : m_nomination_pending_invites) pi.deadline = past;
}

void EngineTalkback::debug_expire_pending_create_for_test()
{
    // Only forces the DEADLINE into the past -- see the header comment. The
    // next lazy self-heal (nominate()/nomination_create_next()/probe()) reads
    // m_nomination_create_deadline the same way it reads a real one, so the
    // whole expire_stale_pending_create_locked()/handle_expired_create() path
    // runs exactly as it would after a real kAwaitTimeout wait.
    const auto past = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    m_nomination_create_deadline.store(past.time_since_epoch().count(),
                                       std::memory_order_release);
}

bool EngineTalkback::session_start(ZOOMSDK::IMeetingService *svc,
                                   const std::string &target)
{
    if (m_session_live) {
        // Refusing a redundant start does not end the existing live session
        // -- no report_session_state() here, unlike every other early return
        // in this function: they all report false because they mean the
        // session never got anywhere.
        report_session("session_start", R"("ok":false,"reason":"already_live")");
        return false;
    }

    // Fix round 2 (Major): A KEY MAY NOT GO LIVE OVER A DEAD AUDIO PATH.
    //
    // open_audio() rejects a ring it cannot use (map_failed, layout_mismatch,
    // unsupported_rate/channels, pipe_header_mismatch) and says so with
    // report_session_state(false, reason). Fix round 1 made the plugin open
    // the tap BEFORE talkback_start, which is right -- it stops the key path
    // from sitting inside a discard window -- but it also made that failure
    // arrive FIRST, where the plugin's last-write-wins status handler let this
    // function's "live" overwrite it. The key then stayed open with the OPEN
    // cue played and a live tally shown while drain_audio() bailed on
    // !m_audio_open and nothing ever reached Zoom: the director believing they
    // are heard, which is the failure this whole feature exists to prevent.
    //
    // The dependency is the fix, not the ordering. Refuse with the AUDIO
    // path's own reason (not a generic one) so the operator is told what is
    // actually wrong -- layout_mismatch means a half-applied install, which
    // CLAUDE.md documents as routine, and it is fixed by installing both
    // binaries rather than by anything they can do at the desk.
    //
    // Deliberately keyed on the REASON and not on !m_audio_open: "the open
    // failed" and "no open has happened yet" are different states, and under
    // any ordering where talkback_start arrives first (which is how this
    // feature shipped until fix round 1) the latter is the normal case. See
    // m_audio_fail_reason's header comment.
    if (!m_audio_fail_reason.empty()) {
        report_session("session_start", R"("ok":false,"reason":")" +
                       json_escape(m_audio_fail_reason) + R"(","audio_path":"failed")");
        report_session_state(false, m_audio_fail_reason);
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

    // Fix round 1, M4: IsMeetingSupportTalkBack() and SetEvent() USED to run
    // here and no longer do. They are nomination-time facts -- nominate()
    // checks the same gate and registers the same sink (this object) before
    // creating anything, and a key press cannot select a channel unless that
    // nomination succeeded in THIS meeting, because Leave clears the table.
    // Re-asking them per press put two SDK calls (one of which queries meeting
    // state, the other re-registers a callback sink) between the key going
    // down and the first buffer leaving -- see the duck comment below for why
    // that is the one place work must not go. Only the null-controller check
    // stays, because that is the one thing a stale pointer would not survive.

    // NO ARBITER GATE HERE, and that is deliberate rather than forgotten:
    // this function issues no CreateChannel, so it has nothing to arbitrate.
    // Do not "restore" the gate for symmetry -- it would take two things with
    // it that are actively wrong on a key press. First, refusing to key
    // because a nomination ladder is still creating LATER channels would deny
    // the operator a target that is already provisioned and ready, for a
    // reason that has nothing to do with it. Second, the gate's companion
    // call, handle_expired_create(), DESTROYS the provisioned set on a
    // Nomination expiry -- running that from a key press would destroy the
    // very channel the press is about to talk on.
    //
    // SELECT. Copy the ids serving this target out under m_chan_mtx, publish
    // the selection in the SAME lock scope (so a concurrent reader can never
    // see a half-built selection), release, and only then touch the SDK --
    // the discipline every other m_chan_mtx access in this file follows.
    std::vector<std::basic_string<zchar_t>> selected;
    std::string selected_ids;   // UTF-8, reporting only
    std::size_t provisioned_total = 0;
    std::size_t still_coming = 0;   // channels for THIS target not created yet
    // Task 4: how many of the target's nominated members this file currently
    // believes are actually in their channel(s), versus how many are
    // nominated for it in total -- "3 of 4 present" in the session_live line
    // below. Task 3 deliberately left this out: the provisioned entry did
    // not track membership at all, only the plan. Fix round 2: computed by
    // members_present_locked(), the SAME loop members_present_for_target()
    // calls -- previously a second, hand-copied traversal of this same
    // table, which the re-review flagged as exactly the kind of duplication
    // that lets the operator's report and a test's accessor drift apart.
    std::size_t members_present = 0;
    std::size_t members_total = 0;
    const char *reason = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        provisioned_total = m_provisioned_channels.size();
        members_present_locked(target, &members_present, &members_total);
        for (const auto &pc : m_provisioned_channels) {
            if (!talkback_channel_serves_target(pc.is_all_talent, pc.members, target))
                continue;
            selected.push_back(pc.channel_id_z);
            if (!selected_ids.empty()) selected_ids += ",";
            selected_ids += pc.channel_id;
        }
        // Fix round 1, M2 (Major): the provisioned table answers "how many
        // channels does this target own SO FAR", and nothing distinguished
        // that from "how many it owns". Provisioning is sequential -- one
        // CreateChannel per response, so an 11-name plan is 13 round trips --
        // and a key pressed a few hundred ms after nominate() would match the
        // one all-talent slice created so far, report "live", and put the
        // director on air to the first ten of eleven. m_nomination_pending
        // holds the plan entries not yet created, and the SAME pure matcher
        // answers the same question about them, so the shortfall is knowable
        // exactly rather than inferred.
        for (const auto &planned : m_nomination_pending)
            if (talkback_channel_serves_target(planned.is_all_talent,
                                               planned.members, target))
                ++still_coming;

        if (still_coming != 0) {
            // FAIL CLOSED. A refused key is recoverable in a second -- press
            // it again -- while a half-broadcast is not recoverable at all:
            // the director briefs a panel believing everyone heard, and no
            // one in the room can tell. This feature's standing rule is that
            // a shortfall is NAMED, never swallowed (src/talkback-plan.h),
            // and this is that rule arriving through the table instead of
            // through the fan-out loop.
            reason = "provisioning_incomplete";
        } else if (selected.empty()) {
            // REFUSE WITH A SPECIFIC REASON, and never create one on demand.
            // Creating here is precisely the behaviour this milestone exists
            // to remove: it would silently restore the clipped-first-syllable
            // defect for whichever target the operator forgot to nominate --
            // and it would do so on the press where the operator is least
            // expecting it, with no signal that this press differed from the
            // last. The two reasons are kept apart because they need
            // different actions: nominate anybody at all, versus nominate
            // THIS person (or key "all" instead).
            reason = provisioned_total == 0 ? "no_nomination"
                                            : "target_not_provisioned";
        }

        // Fix round 1, Minor 6: an UNCONDITIONAL store, of a value the
        // decision above has already made empty on every refusal. The Task 3
        // version was `if (!selected.empty()) m_session_channels = selected;`
        // -- correct only by an argument about the other writers, in the one
        // file whose whole discipline is that state changes are structural
        // rather than argued. Clearing on refusal is also load-bearing now
        // that provisioning_incomplete exists: that path refuses with a
        // NON-empty `selected`, and drain_audio() sends to
        // m_session_channels without consulting m_session_live, so storing it
        // would put audio into a partial fan-out the key press just refused.
        if (reason) selected.clear();
        m_session_channels = selected;
    }

    // Expected vs actual on EVERY key, refused or not: "2 of 13" is what tells
    // the operator (and the log, after the fact) that the fan-out was short.
    const std::string counts =
        R"("channels":)" + std::to_string(selected.size()) +
        R"(,"expected":)" + std::to_string(selected.size() + still_coming);

    if (reason) {
        // Fix round 2 (Minor): name the recovery, not just the refusal. Fail
        // closed is right and stands, but a ladder whose create response was
        // swallowed leaves this target refusing FOREVER -- session_start
        // deliberately does not run the lazy self-heal, because
        // handle_expired_create() destroys, and a key press must never destroy
        // the channels it is about to talk on. So the recovery is manual, and
        // an operator who is refused mid-show needs to be told what it is in
        // the same line rather than having to know.
        const char *recover = std::string(reason) == "provisioning_incomplete"
                                  ? R"(,"recover":"re-nominate")"
                                  : "";
        report_session("session_start", std::string(R"("ok":false,"reason":")") +
                       reason + R"(","target":")" + json_escape(target) + "\"," +
                       counts + recover);
        report_session_state(false, reason);
        return false;
    }

    m_session_target = target;
    m_session_live   = true;

    // Duck the main meeting for the people in these channels, so the director
    // is unambiguous rather than competing with meeting audio -- the same
    // 0.3 the probe's invite path has always used.
    //
    // ON THE KEY, NOT AT NOMINATION: the SDK documents this as "the main
    // meeting audio volume that people in the talkback channel can hear", and
    // a pre-provisioned channel now stands for the WHOLE SHOW. Ducking once at
    // nomination would leave every nominated talent hearing the meeting at 30%
    // from nomination until the meeting ends, whether or not anyone ever keys.
    //
    // ...but NOT SYNCHRONOUSLY HERE either (fix round 1, M4; the
    // justification is fix round 2's, because the one this originally rested
    // on no longer exists). talkback_start and talkback_audio are branches of
    // the same command loop, so everything this function does happens BEFORE
    // the first buffer is read.
    //
    // The original argument was that open_audio() then DISCARDED that audio,
    // by snapping m_audio_read_index to the writer's current index. It does
    // not any more -- fix round 2 made it read from 0 for this ring, precisely
    // so that window buffers instead of vanishing -- so do not reason from a
    // discard here. What survives is the reason that never depended on it:
    // THIS IS THE ONE PLACE WORK MUST NOT GO. A talkback key is judged by
    // whether the director's first syllable is heard; the ring bounds the
    // buffering at 8 slots (~80ms of 10ms buffers) and past that it is real
    // loss -- counted as `lost` now, but counted is not the same as avoided.
    // So the duck is armed here and applied by the first drain_audio() of the
    // press, after its sends. One buffer of director-over-unducked-meeting is
    // a far better failure than a late or lost first syllable.
    m_session_duck_pending = true;

    // "audio_path" is a LABEL, not a gate (fix round 2). A refusal here would
    // regress the ordering fix: "not_open" is the normal state for any caller
    // that drives talkback_start before talkback_open, which is how this
    // feature shipped until fix round 1 and which nothing stops a raw-pipe
    // caller doing today. What it is worth is visibility -- a key that reports
    // live with no ring behind it sends nothing, and this makes that legible
    // in the log instead of leaving the operator to infer it from silence. A
    // FAILED open is a different matter and is refused above; this only
    // distinguishes open from not-yet.
    // Task 4: "members_present"/"members_total" -- the director should see
    // "live, 3 of 4 present", not just channel counts. `counts` above answers
    // "how many CHANNELS", which says nothing when a single provisioned
    // channel is short a member who has not rejoined yet; this answers the
    // question the operator is actually asking with a key press.
    report_session("session_live", R"("target":")" + json_escape(target) +
                   R"(",)" + counts +
                   R"(,"members_present":)" + std::to_string(members_present) +
                   R"(,"members_total":)" + std::to_string(members_total) +
                   R"(,"audio_path":")" + (m_audio_open ? "open" : "not_open") +
                   R"(","channel_ids":")" + json_escape(selected_ids) + "\"");
    report_session_state(true, "live");
    return true;
}

void EngineTalkback::session_stop()
{
    // TASK 3: STOPPING SENDING IS ALL THIS DOES. The channels stay alive for
    // the next press -- that is what makes the next press instant too, and
    // destroying them here would put the create+invite round trip back on the
    // key path one press later, which is the defect this milestone removes.
    // Destruction lives in nominate()'s replace path and nomination_reset()
    // (Leave/quit).
    //
    // Everything this function used to do about a Session-owned CreateChannel
    // -- the cancellation branch, the arbiter expire, the retrying
    // batch-destroy -- is gone with the create itself; see the header comment
    // where those members were declared for the race that deletion closes.
    //
    // Idempotent: Leave and quit both call it unconditionally, and a key that
    // was never granted a channel must not be an error on the way out.
    std::vector<std::basic_string<zchar_t>> channels;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        channels.swap(m_session_channels);
    }
    const bool was_live   = m_session_live;
    const bool was_ducked = m_session_ducked;
    m_session_live         = false;
    m_session_ducked       = false;
    m_session_duck_pending = false;   // a press released before its first
                                      // drain never ducked; disarm it so the
                                      // NEXT press's drain cannot inherit it
    m_session_target.clear();

    if (channels.empty() || !m_ctrl) {
        report_session("session_stop", std::string(R"("ok":true,"reason":"no_channel","was_live":)") +
                       (was_live ? "true" : "false"));
        return;
    }

    // Undo the key-down duck -- but only if it was ever applied. 1.0 is the
    // SDK's unattenuated value (documented range 0.0-2.0), so the talent is
    // left hearing the meeting exactly as everybody else does between presses
    // rather than at 30% for the rest of the show. A press released before its
    // first drain_audio() never ducked (fix round 1, M4), and restoring then
    // would be N SDK calls setting 1.0 on channels already at 1.0.
    // Best-effort by design: a failure here costs one person's meeting-audio
    // level, and refusing to release the key over it would cost the operator
    // the feature.
    if (was_ducked) {
        for (const auto &id : channels)
            m_ctrl->SetChannelBackgroundVolume(id.c_str(), 1.0f);
    }

    report_session("session_stop", R"("ok":true,"channels":)" +
                   std::to_string(channels.size()) + R"(,"restored":)" +
                   (was_ducked ? "true" : "false"));
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
    // The verdict belongs to the attempt, and the attempt is over. Leaving it
    // set would make the NEXT press refuse for a failure that has already been
    // torn down -- see m_audio_fail_reason's header comment.
    m_audio_fail_reason.clear();
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
