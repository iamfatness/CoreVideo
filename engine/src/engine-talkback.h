#pragma once
//
// engine-talkback.h — the Zoom talkback probe (Milestone 1), plus everything
// built on top of it since: the persistent session (Milestone 5) and, as of
// Task 2 (2026-08-25), pre-provisioning a nominated talent list's channels
// at nomination time rather than at key time.
//
// Talkback is the first path in this codebase that SENDS audio to Zoom. Every
// other media path runs engine -> plugin. This class exists to answer one
// question before any of that is built: can this account open a talkback
// channel and put audio in it?
//
// Neither the SDK headers nor Zoom's documentation state what entitles
// talkback. The 7.0.0 changelog says only "Support talkback audio feature" and
// lists Permission denied among the error codes. Our working assumptions are
// host/co-host plus the Zoom Enhanced Media add-on, and this probe is how they
// get tested rather than believed.
//
// Every rung reports its own SDKError and TalkbackError over E2P, so a failure
// names the exact rung it fell off instead of surfacing as silence.
//
// engine-ipc.h must come before any Zoom SDK header: it pulls in <windows.h>
// (under WIN32), and zoom_sdk_def.h uses HWND without including it itself.
// Every other engine-*.h in this codebase follows the same order.
#include "../../src/engine-ipc.h"

#include "zoom_sdk.h"
#include "meeting_service_interface.h"
#include "meeting_service_components/meeting_talkback_ctrl_interface.h"
// meeting_service_interface.h only forward-declares IMeetingParticipantsController;
// resolve_participant() needs the full definition (GetParticipantsList,
// GetUserByUserID) and IUserInfo (GetUserName). meeting_participants_ctrl_interface.h
// uses AudioType without including its home header, so meeting_audio_interface.h
// must come first -- same order main.cpp already uses for this same pair.
#include "meeting_service_components/meeting_audio_interface.h"
#include "meeting_service_components/meeting_participants_ctrl_interface.h"

// talkback_pcm_rate_supported() -- F7 review-round fix uses this to validate
// the ring header's sample_rate engine-side, the same gate the plugin
// already applies before it ever creates the region.
#include "../../src/talkback-pcm.h"
#include "../../src/talkback-channel-owner.h"
// talkback_plan() / TalkbackPlan / TalkbackPlannedChannel -- Task 1's pure
// budget planner that nominate() (below) consumes. No SDK dependency itself.
#include "../../src/talkback-plan.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class EngineTalkback : public ZOOMSDK::IMeetingTalkbackCtrlEvent {
public:
    // Starts the probe ladder. Reports and returns without blocking; the
    // asynchronous rungs continue through the callbacks below and tick().
    //
    // THE RETURN VALUE IS A CONTRACT, not a status code: true means, and
    // means only, "this call issued a CreateChannel, so there is now SDK
    // work a tick()-driving thread must own". The caller MUST spawn that
    // thread on true and MUST NOT on false. Every refusal returns false --
    // the re-entrancy guard, a live session, no service, no controller,
    // talkback unsupported, SetEvent failing, the create arbiter being held
    // by someone else, and CreateChannel failing synchronously. There is
    // exactly one `return true`, at the end of the function, immediately
    // after the create is claimed; that is deliberate, so the contract is
    // checkable by reading the exits rather than by trusting this comment.
    // (It is not checkable by a test: the decision that is not already
    // covered by talkback_may_request_create()'s own tests is "did this call
    // reach CreateChannel", which needs a live IMeetingTalkbackController --
    // the talkback tests compile pure headers with no engine and no SDK.
    // Structure carries this invariant, not ctest.)
    //
    // Why it is a contract: the driving thread is the only thread besides
    // the command loop that drives the batch-destroy API (see the inventory
    // at the top of tick()), and four branches of onCreateChannelResponse
    // destroy directly on the command loop. A thread spawned for a probe
    // that created nothing can be draining strays at exactly the moment one
    // of those branches fires -- and the arbiter-refused case is the worst
    // one, because the refusal reason IS that a Session/Nomination create
    // is outstanding. A probe that did issue a create holds the arbiter
    // itself, which excludes those responses for as long as its thread
    // lives. Refusals that create nothing therefore drain any queued stray
    // synchronously before returning false, so "no thread" never means
    // "nobody drains" -- see probe_refused_without_ladder().
    bool probe(ZOOMSDK::IMeetingService *svc, const std::string &participant_name);

    // Called from the engine's main loop. Sends tone buffers while a send is
    // in progress, then destroys the channel.
    void tick();

    // ── Talkback audio path (Milestone 2) ──────────────────────────────────
    bool open_audio(const std::string &region_name, uint32_t sample_rate,
                    uint16_t channels);
    void drain_audio();
    void close_audio();

    // ── Persistent talkback session (Milestone 5; SELECT-ONLY since Task 3) ─
    // Deliberately NOT part of the probe's Phase machine: that machine exists
    // to tear itself down after one tone, which is the opposite of what a key
    // held down needs. The session never touches the PROBE's channel
    // (m_channel_id_z), which tick() destroys from a separate thread, so that
    // race stays structurally impossible rather than lock-avoided.
    //
    // TASK 3 (2026-08-25) IS THE POINT OF THIS MILESTONE: session_start()
    // SELECTS, it does not create. It looks `target` up in
    // m_provisioned_channels (filled at nomination time) and points
    // drain_audio() at every channel serving it. No CreateChannel, no invite,
    // no round trip -- those two round trips are what discarded the start of
    // every key press (measured live 2026-08-25 as no_channel_drops on every
    // press). An unprovisioned target is REFUSED with a specific reason and
    // never provisioned on demand: creating on the key is the exact behaviour
    // this milestone removes, so a fallback would silently restore the defect
    // for whichever target the operator forgot to nominate -- the one case
    // where it matters most.
    //
    // `target` is the operator's target string, not a channel: either
    // kTalkbackAllTalentTarget or a nominee's name (src/talkback-plan.h's
    // talkback_channel_serves_target() owns the matching, and is the only part
    // of the key path a test can reach).
    //
    // session_stop() STOPS SENDING. It does not destroy anything: the channels
    // stand for the next press, which is what makes the next press instant
    // too. Destruction belongs to nominate()'s replace path, nomination_reset()
    // (Leave/quit) and the failure paths in the create ladder -- never to a key
    // release. Idempotent: Leave and quit both call it unconditionally.
    bool session_start(ZOOMSDK::IMeetingService *svc, const std::string &target);
    void session_stop();
    bool session_live() const;

    // ── Pre-provisioned channels (Task 2, 2026-08-25) ───────────────────────
    // Computes talkback_plan(nominees) (src/talkback-plan.h) and provisions
    // every channel it decides on, so a later key press only SELECTS an
    // already-live channel instead of creating one on the spot -- the
    // create-then-invite round trip that clipped the director's first words
    // on every press, measured live 2026-08-25 as discarded buffers on every
    // key (no_channel_drops).
    //
    // CreateChannel may be called only from the engine's command-loop
    // thread, and the arbiter (src/talkback-channel-owner.h) allows exactly
    // one outstanding create at a time -- so provisioning is SEQUENTIAL:
    // this issues ONE CreateChannel and returns without blocking; each
    // onCreateChannelResponse routed to TalkbackChannelOwner::Nomination
    // invites that channel's members and issues the next, until the plan's
    // queue is empty. Every gate the plan or the SDK surfaces
    // (uncovered_private, unreachable, a nominee not currently in the
    // meeting, IsSupportTalkback() == false) is reported, never swallowed --
    // see report_nomination()'s call sites in the .cpp.
    //
    // Returns false when nothing was queued at all: no meeting service, no
    // controller, the meeting does not support talkback, the probe's driving
    // thread or a live session already hold the arbiter (same R1 mutual
    // exclusion session_start() enforces -- nomination must not break it), or
    // a create is still outstanding.
    //
    // Task 3 decides what a RE-nomination does, which Task 2 deliberately left
    // open (its already_provisioned refusal has a comment saying so). It
    // REPLACES: the standing set is destroyed and the new plan provisioned in
    // its place. Task 2's refusal was correct while nothing owned the
    // decision, but pre-provisioning turns it into a trap -- the talent list
    // is now fixed for the rest of the meeting after the first nominate(), and
    // the only escape is a Leave. Replacing is safe here specifically because
    // the two states that make destroying wrong are already refused above: a
    // live key press (m_session_live) and an outstanding create.
    // Returns true once the first CreateChannel of the plan is in flight (or
    // immediately, if the plan needed zero channels) -- true is not a
    // promise every channel will finish provisioning, only that a queue
    // started; report_nomination()'s "nominate_done" line is what confirms
    // completion.
    bool nominate(ZOOMSDK::IMeetingService *svc,
                  const std::vector<std::string> &nominees);

    // Bookkeeping-only reset for the nomination table/queue -- never calls
    // the SDK. Called from Leave/quit in engine/src/main.cpp because
    // provisioned channels and their membership are meeting-scoped (see the
    // design doc's "Meeting rejoin" row in the failure table): once the
    // meeting is gone there is nothing left on Zoom's side to select or
    // destroy, so clearing our own record of it is all that is needed. Does
    // NOT destroy anything meeting-side -- there is no un-nominate SDK call
    // in this task, and none is needed here for the same reason.
    void nomination_reset();

    // True once the ladder is quiescent: Idle before the first probe() ever
    // runs, Done after one finishes (success, failure, or abandoned
    // destroy). Task 5's driving thread uses this to stop ticking as soon as
    // the probe settles instead of always spinning its full bound -- and
    // deliberately exposes only this bool, not m_phase itself, so callers
    // outside this file never take a dependency on the phase enum's shape.
    bool is_idle() const
    {
        const Phase p = m_phase.load(std::memory_order_acquire);
        return p == Phase::Idle || p == Phase::Done;
    }

    // True while the driving loop in main.cpp must keep calling tick():
    // either the ladder itself is not settled, OR a stray channel is queued
    // and still needs drain_stray_channels() (called only from tick()) to
    // run for it. is_idle() and has_pending_work() answer different
    // questions and must not be conflated: is_idle() answers "may a new
    // ladder start?" -- the refusal gate in main.cpp uses it, and a pending
    // stray must NOT make that gate refuse new probes indefinitely, because
    // a stray drain has nothing to do with whether a fresh ladder is safe to
    // start. has_pending_work() answers "must the driver keep running?" --
    // the driving loop uses it instead of is_idle() so it does not exit and
    // orphan a queued-but-undrained stray channel between the ladder
    // settling to Idle/Done and drain_stray_channels() next getting a
    // chance to run (see the F3 review-round finding: AwaitingChannel times
    // out at 10s -> Destroying -> Done -> driver exits -> a genuinely late
    // onCreateChannelResponse arrives after that and queues a real Zoom
    // channel that nothing then destroys).
    //
    // R1-round-3 review fix: session_start() gates on this function believing
    // "false" means the driving thread will not touch m_ctrl. That was wrong
    // for one specific window: drain_stray_channels() swaps m_stray_channels
    // into a local UNDER m_chan_mtx, releases the lock, and only THEN runs
    // its Begin/Add/ExecuteBatchDestroyChannels loop against m_ctrl. In that
    // window the member m_stray_channels already reads empty and m_phase can
    // independently already read Done, so the two checks below would both
    // pass and this function would report "nothing to wait for" while the
    // driving thread is still mid SDK-call. m_driving_thread_in_sdk_call is
    // what actually closes that: set before drain_stray_channels() risks
    // touching the SDK, cleared once it's done, checked here FIRST so it
    // dominates the other two checks. (tick()'s Destroying-phase SDK
    // sequence does not need the same treatment: unlike the stray path, it
    // never stores Phase::Done until strictly after its own Begin/Add/
    // Execute sequence finishes, so m_phase alone already reads "busy" for
    // that entire window -- traced, not assumed.)
    bool has_pending_work() const
    {
        if (m_driving_thread_in_sdk_call.load(std::memory_order_acquire))
            return true;
        const Phase p = m_phase.load(std::memory_order_acquire);
        if (p != Phase::Idle && p != Phase::Done) return true;
        // Takes m_chan_mtx only for this queue check -- never call this
        // function while already holding m_chan_mtx elsewhere, same
        // discipline as every other access to m_stray_channels.
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        return !m_stray_channels.empty();
    }

    // IMeetingTalkbackCtrlEvent
    void onCreateChannelResponse(const zchar_t *channelID, TalkbackError error) override;
    void onDestroyChannelResponse(const zchar_t *channelID, TalkbackError error) override;
    void onChannelUserJoinResponse(const zchar_t *channelID, unsigned int userID,
                                   TalkbackError error) override;
    void onChannelUserLeaveResponse(const zchar_t *channelID, unsigned int userID,
                                    TalkbackError error) override;
    void onJoinTalkbackChannel(unsigned int inviterID) override;
    void onLeaveTalkbackChannel(unsigned int inviterID) override;
    void onInviterAudioLevel(unsigned int inviterID, unsigned int audioLevel) override;

private:
    enum class Phase { Idle, AwaitingChannel, AwaitingInvite, Sending, Destroying, Done };

    // const: touches no member state, only formats a string and writes to
    // the pipe -- resolve_participant() (below, const) needs to call it to
    // report the per-user talkback gate (F2 review-round fix) without
    // losing its own const-ness.
    //
    // report() tags every line "cmd":"talkback_probe" -- correct for the
    // Milestone 1 ladder, wrong for anything from the persistent session
    // (Milestone 5) or its shared audio path. Before the whole-plan review
    // round, session_start/session_live/session_invite/audio_send/
    // session_stop all went through report() and arrived labelled as probe
    // stages -- logged as "talkback_probe: ..." and overwriting the dock's
    // probe status label even when no probe was running (F6 finding).
    // report_session() is the same shape under "cmd":"talkback_session" for
    // every call site that is session/audio-path-only, never reachable from
    // probe().
    void report(const std::string &stage, const std::string &fields) const;
    void report_session(const std::string &stage, const std::string &fields) const;

    // Task 2 counterpart of report()/report_session() above: tagged
    // "cmd":"talkback_nominate", the same string as the P2E command that
    // triggers nominate() -- mirroring the probe's convention (report()
    // reuses "talkback_probe" both ways) rather than the session's (which
    // funnels several P2E commands into one "talkback_session" E2P tag),
    // since nominate() has exactly one entry point to funnel.
    void report_nomination(const std::string &stage, const std::string &fields) const;

    // F2 review-round fix (CRITICAL): the ONE engine->plugin line that
    // carries the session's CONFIRMED state, distinct from every stage
    // trace above. Emitted when the session goes live (after the invite is
    // accepted) and on every failure path in session_start()/
    // onCreateChannelResponse()/open_audio() that ends the session before
    // that point. Shape: {"cmd":"talkback_session","live":true|false,
    // "reason":"..."} -- see ZoomEngineClient::handle_event()'s
    // talkback_session branch (distinguishes this from a stage line by the
    // presence of "live") and TalkbackController::evaluate()/status_json()
    // for the consumer side.
    void report_session_state(bool live, const std::string &reason) const;

    // Fix round 1, M3: resolve_participant()'s one report call
    // ("participant_talkback_support", the per-user IsSupportTalkback()
    // gate) used to always go through report() -- correct for the probe,
    // wrong for the session (pre-existing wart, left alone) and wrong for
    // nomination, where the brief requires this exact gate to be surfaced
    // in the nomination stream, not the probe's. Callers pass which sink
    // they need; defaults to Probe so probe()'s own call site (unchanged)
    // needs no edit.
    enum class ReportSink { Probe, Nomination };
    unsigned int resolve_participant(const std::string &name,
                                     ReportSink sink = ReportSink::Probe) const;

    // Drains m_stray_channels and destroys each one. The caller must be the
    // only batch-destroy caller alive at that moment; the two callers that
    // satisfy that, and why, are named at the function's own comment.
    void drain_stray_channels();

    // Is `channelID` one of the channels nomination has already provisioned?
    // CALLER MUST HOLD m_chan_mtx (it walks m_provisioned_channels).
    //
    // Fix round 1, M1 (Major): this is the one question every disposition of a
    // create response has to ask before it does anything irreversible, and
    // Task 3 first asked it in only ONE of them. The SDK redelivers
    // onCreateChannelResponse -- this file's own duplicate handling exists
    // because it does -- and a redelivered NOMINATION response can arrive
    // while a probe holds the arbiter, in which case it is attributed to
    // Probe, skips the Nomination branch entirely, and lands on the probe's
    // adoption path. The probe then invited into a talent's live channel,
    // toned 3s of 440Hz at them, and destroyed it from tick(): a director's
    // private channel emitting a test tone mid-show and then dying, with
    // every later key press for that target selecting fewer channels and
    // nothing left to re-provision them.
    //
    // Null-safe: a null id matches nothing (basic_string comparison against a
    // null zchar_t* is char_traits::length(nullptr), an access violation, so
    // the guard is here rather than at each caller).
    bool channel_is_provisioned_locked(const zchar_t *channelID) const;

    // Adopt `channelID` as the PROBE's channel, unless it is already
    // provisioned. Returns false when it is, meaning this response belongs to
    // somebody else's create and the ladder must keep waiting for its own.
    //
    // Fix round 1, M1: the check and the assignment are ONE call, in ONE lock
    // scope, so a future adoption path cannot be added that skips the check --
    // the same reasoning that moved the arbiter's claim-and-stamp into a
    // single transition last task. If you add another adopter, call this;
    // never assign m_channel_id_z directly.
    bool adopt_probe_channel(const zchar_t *channelID, const std::string &id_utf8);

    // The single exit every probe() refusal that created nothing routes
    // through: settles the phase, drains any queued stray on this thread
    // (nobody else will -- no driving thread is being spawned), and returns
    // false so main.cpp does not spawn one. See probe()'s return-value
    // contract above for why "created nothing -> no driving thread" is an
    // invariant of the batch-destroy serialization rather than a tidiness
    // preference. Command-loop thread only, and only from probe().
    bool probe_refused_without_ladder();

    // Follow-up to the F1 review-round fix: lazily expires a stale
    // Nomination-owned m_pending_create -- see m_nomination_create_deadline's
    // doc comment below for why this exists. MUST be called with m_chan_mtx
    // already held (every call site -- probe()'s, session_start()'s and
    // nomination_create_next()'s gate checks -- already locks it to read
    // m_pending_create, so this adds no new critical section). Returns which
    // owner it expired (TalkbackChannelOwner::None if nothing was stale), so
    // the caller can report it AFTER releasing the lock, same discipline as
    // every other report() call in this file.
    //
    // NOMINATION IS THE ONLY OWNER WITH AN ARM HERE, and as of Task 3 it is
    // the only owner that can be pending at all. Probe has a clearer of its
    // own (tick()'s AwaitingChannel timeout, on the driving thread), so a
    // stale Probe entry would be a bug in that machinery rather than something
    // to paper over here. Session had an arm until Task 3 and no longer needs
    // one: session_start() issues no CreateChannel, so nothing in this engine
    // ever claims the arbiter for Session, and there is no Session-owned
    // create left to go stale. Returning None for it is therefore the honest
    // answer, not an omission -- and it is what makes the parked "Session-side
    // expire-path race" unreachable rather than fixed (see session_start()).
    TalkbackChannelOwner expire_stale_pending_create_locked();

    // Fix round 4: what every caller of expire_stale_pending_create_locked()
    // must do with its return value, once the lock is released. Was three
    // hand-written copies of the same if/else-if report (probe(),
    // session_start(), nomination_create_next()) -- the exact per-owner
    // duplication that produced N1 -- and round 4 had to add a fourth call
    // site (nominate()) plus a new action to the Nomination arm, which is
    // precisely when that shape goes wrong. MUST be called with m_chan_mtx
    // NOT held: the Nomination arm calls nomination_destroy_provisioned(),
    // which calls the SDK. One arm since Task 3, for the reason on
    // expire_stale_pending_create_locked() above; it stays a function taking
    // an owner rather than collapsing to "if expired" so that a second owner
    // getting a create back gets a compile site to fill in, not a silent
    // fall-through.
    //
    // The Nomination arm destroys whatever the expired ladder had already
    // provisioned. Without that, a ladder whose channel-k create response was
    // swallowed leaves channels 1..k-1 standing forever: a PARTIAL set that
    // consumes budget out of the meeting's 16 and that no key needing the
    // whole fan-out can use. Same reasoning and same helper as fix round 1's
    // M2 (an error response destroys the partial set so a retry can start
    // clean); the expiry path had simply never been wired to it.
    //
    // Task 3 fix round 1: this used to be justified by "nominate()'s
    // already_provisioned gate refuses on that table, so one transient SDK
    // hiccup disabled re-nomination for the rest of the meeting". That gate
    // is gone -- Task 3 replaced it -- so the justification was void while
    // the behaviour stayed correct. Corrected rather than deleted, because
    // the arm still needs a reason and now has the true one.
    void handle_expired_create(TalkbackChannelOwner expired_owner);

    // Issues the CreateChannel for the front of m_nomination_pending and, on
    // synchronous success, claims the arbiter as Nomination. Called once
    // from nominate() (for the plan's first channel) and once more from
    // onCreateChannelResponse's Nomination branch for every channel still
    // queued after that -- see nominate()'s declaration comment on why this
    // is sequential rather than issuing the whole plan at once. Must run on
    // the command-loop thread, same as every other CreateChannel call in
    // this file. Returns false (and empties m_nomination_pending) when the
    // gate refuses or the SDK call itself fails synchronously -- there is no
    // retry queue for this in Task 2; a stalled ladder is reported, not
    // silently abandoned.
    bool nomination_create_next();

    // Fix round 1, m6: nomination_create_next() is the first call in this
    // file to issue CreateChannel from inside a callback dispatch
    // (onCreateChannelResponse's Nomination branch calls it again for the
    // next queued channel) rather than only from a top-level pipe-command
    // handler -- the arbiter's whole design assumes CreateChannel is
    // command-loop-thread-only, and nothing before this task ever checked
    // that assumption at a CreateChannel call site itself. Records the
    // thread id of its first caller and reports a mismatch on every later
    // call from a different thread -- converts an assumption this file's
    // own comments already flag as newly load-bearing (and contradicted in
    // general by engine-writer.h's "SDK fires callbacks on its own internal
    // threads" statement) into evidence instead of leaving it silently
    // trusted. Does not gate or refuse anything -- a wrong assumption here
    // is a race to diagnose, not one this function can safely correct by
    // itself.
    void assert_command_loop_thread(const char *where) const;

    // Fix round 1, M2: destroys every channel currently in
    // m_provisioned_channels and forgets them. Called whenever provisioning
    // cannot continue -- a synchronous CreateChannel failure, an error
    // response, the arbiter refusing the next create, or (fix round 4, via
    // handle_expired_create()) a create whose response never arrived at all
    // -- so a transient
    // failure on channel k of a plan does not strand the first k-1
    // already-created channels for the rest of the meeting: consuming
    // budget, unreachable by any key, and (before this fix) making every
    // later nominate() refuse with "already_provisioned" forever, since
    // nothing destroyed them to make room for a retry. Must run on the
    // command-loop thread, same as create -- every call site is
    // (nominate()/nomination_create_next()/handle_expired_create() from a
    // pipe command, onCreateChannelResponse from the SDK message pump on
    // that same thread). Never call it with m_chan_mtx held; it takes the
    // lock itself to copy the ids out, then calls the SDK after releasing.
    void nomination_destroy_provisioned();

    // Fix round 3: the bounded Begin/Add/Execute destroy retry, which had
    // grown to four hand-copied loops. Extracted so a future change to the
    // retry bound or the sequence itself cannot apply to three of them and
    // miss the fourth, the exact shape of duplication this task's own review
    // history keeps finding. FOUR call sites, re-counted from the .cpp in
    // Task 3: onCreateChannelResponse's nomination-stale,
    // nomination-cancelled and nomination-untracked branches, plus
    // nomination_destroy_provisioned()'s loop. They are all Nomination's now
    // -- the session-cancelled branch went with the session's CreateChannel
    // -- which is not the same statement as the round-3 wording that this
    // helper is Nomination-only BY DESIGN; it is a fact about today's
    // callers. Two hand-written copies of the same sequence remain outside it
    // (drain_stray_channels() and tick()'s Destroying phase, both on the
    // probe's driving thread); the inventory at the top of tick() is the map.
    // Never called with m_chan_mtx held (dereferences m_ctrl); *attempts (if
    // non-null) receives how many tries it took, for callers that report it.
    ZOOMSDK::SDKError destroy_channel_retrying(const zchar_t *channelID, uint32_t *attempts);

    // Resolves `name` to a live user id and, if found, invites it into the
    // already-created channel `channel_id_z`. Deliberately independent of
    // the create-queue machinery above -- it takes a channel id and a name,
    // nothing about m_nomination_pending or the arbiter -- so a future
    // roster-driven re-invite (Task 4, ruled to run on the SDK callback
    // thread and to NEVER call CreateChannel there) can call this same
    // primitive without touching create-side state at all. A name not
    // currently in the meeting is reported and skipped, not an error --
    // that is the expected shape for someone who has not joined yet.
    void invite_nominee(const std::basic_string<zchar_t> &channel_id_z,
                        const std::string &channel_id_utf8,
                        const std::string &name);

    ZOOMSDK::IMeetingService          *m_svc  = nullptr;
    ZOOMSDK::IMeetingTalkbackController *m_ctrl = nullptr;

    // m_phase is written from SDK callback threads (onCreateChannelResponse,
    // onChannelUserJoinResponse) and, once Task 5 wires tick() to the engine
    // main loop, read/written from the engine thread too -- atomic with
    // explicit acquire/release is the cheap, correct fix for THIS field: a
    // trivially-copyable enum can be published as a unit with no lock. It
    // stays atomic rather than folding into m_chan_mtx below -- the two
    // mechanisms are complementary, not competing: the atomic is the fast
    // path every function checks first, the mutex is only for the string
    // fields it is unsafe to reason about from phase ordering alone.
    std::atomic<Phase> m_phase{Phase::Idle};

    // m_channel_id / m_channel_id_z / m_stray_channels are NOT safe under
    // acquire/release on m_phase alone and must go through m_chan_mtx for
    // EVERY access from here down, cross-thread or not -- no exceptions, so
    // nobody has to re-derive which phases are "safe".
    //
    // The tempting argument -- "m_channel_id_z is fully written in
    // onCreateChannelResponse before m_phase is released to AwaitingInvite,
    // so an acquire-load of m_phase >= AwaitingInvite is a synchronizes-with
    // edge" -- is real but incomplete: it only covers phases AT OR ABOVE
    // AwaitingInvite. It says nothing about Idle or Done. probe()'s
    // re-entrancy guard deliberately ALLOWS a fresh probe() to run whenever
    // phase is Idle/Done, and that fresh call clears/reassigns these members
    // from whatever thread called probe(). Meanwhile a late or duplicate
    // onCreateChannelResponse for the PREVIOUS probe can observe phase as
    // Idle/Done (via its own acquire-load) and read these same members on
    // the SDK callback thread at the same moment -- a heap-buffer read
    // racing a concurrent std::basic_string mutation: undefined behaviour,
    // not merely a stale value. This is on the expected path (it is exactly
    // the late-callback scenario the timeout machinery exists to handle),
    // found live in review round 3 after round 2's stray-channel fix added
    // the first cross-thread read of m_channel_id_z that could land in the
    // Idle/Done window.
    //
    // Discipline: copy the needed value out under the lock, release, THEN
    // call the SDK or build a report string with the copy. Never call the
    // SDK while holding m_chan_mtx.
    //
    // mutable: has_pending_work() (above, const) takes this lock purely to
    // read whether m_stray_channels is empty -- a read-only query from the
    // outside, so it is declared const like is_idle(), which requires the
    // mutex itself be lockable from a const method.
    mutable std::mutex m_chan_mtx;
    std::string  m_channel_id;      // UTF-8, REPORTING ONLY -- never pass to
                                     // the SDK, see m_channel_id_z below.
    // zchar_t is wchar_t on Windows (zoom_sdk_def.h) but char elsewhere, so
    // basic_string<zchar_t> is the only type that is simultaneously correct
    // on both platforms and round-trip-safe for an opaque SDK identifier
    // (no UTF-8 re-encoding). Every SDK call that takes a channel ID copies
    // this out under m_chan_mtx first; never call .c_str() on it directly.
    std::basic_string<zchar_t> m_channel_id_z;

    // BeginBatchDestroyChannels/AddChannelToDestroy/ExecuteBatchDestroyChannels
    // has exactly one caller: tick(), on whichever thread owns the engine
    // main loop -- see the invariant comment at the top of tick(). Callbacks
    // that discover a channel needing cleanup push its id here (under
    // m_chan_mtx) instead of calling the SDK; drain_stray_channels(), called
    // only from tick(), is the sole drainer.
    std::vector<std::basic_string<zchar_t>> m_stray_channels;

    // R1-round-3 review fix: true for exactly the window in which
    // drain_stray_channels() (driving thread) is between its m_chan_mtx-
    // protected swap of m_stray_channels and the end of its subsequent
    // Begin/Add/ExecuteBatchDestroyChannels loop against m_ctrl. Neither
    // m_phase nor m_stray_channels alone can express "busy" for that window
    // -- by the time the SDK loop runs, the swap has already emptied the
    // member queue, and m_phase can independently already read Done -- so
    // has_pending_work() needs this as a third, explicit signal. See its
    // doc comment above for why session_start()'s R1 mutual-exclusion gate
    // depends on has_pending_work() being right about this. atomic<bool>,
    // not m_chan_mtx: this flag is read by has_pending_work() while
    // m_chan_mtx may or may not be held by the caller (session_start() does
    // not hold it), and it is set/cleared around SDK calls that must never
    // run under that mutex -- folding this into m_chan_mtx would mean
    // either holding the mutex across the SDK loop (forbidden) or leaving a
    // gap between unlocking and setting/clearing this flag, which is the
    // exact class of gap this flag exists to close. No `mutable` needed:
    // std::atomic<bool>::load() is already const.
    std::atomic<bool> m_driving_thread_in_sdk_call{false};

    std::string  m_participant_name;
    unsigned int m_participant_id = 0;
    uint64_t     m_tone_index = 0;
    uint32_t     m_buffers_sent = 0;

    // Deadline for whichever of AwaitingChannel / AwaitingInvite is
    // currently active (only one is ever active at a time, so one field
    // suffices). An SDK call that returns SDKERR_SUCCESS is only a promise
    // that the call was accepted, not that the matching callback will ever
    // fire; without a deadline a swallowed callback hangs the probe forever
    // and reports nothing, which is silence -- the exact failure mode this
    // class exists to make visible instead of enduring.
    //
    // atomic (F5 review-round fix): this is genuinely cross-thread, and not
    // covered by m_phase's release/acquire the way it first looks. The
    // write in onCreateChannelResponse happens while phase still reads
    // AwaitingChannel -- BEFORE the release-store that advances it to
    // AwaitingInvite -- and tick(), running concurrently on the driving
    // thread, can read this same field for its AwaitingChannel timeout
    // check at that exact moment. That is a plain, unsynchronized
    // concurrent read/write of the same non-atomic memory from two threads:
    // undefined behaviour, not merely a stale value, and it is on the
    // expected path (a create_channel_response arriving while tick() is
    // mid-timeout-check is ordinary timing, not a rare interleaving).
    // Stored as the steady_clock rep (an integer) rather than the
    // time_point itself, since time_point is not trivially atomic-friendly
    // across implementations; reconstructed with
    // steady_clock::time_point(steady_clock::duration(rep)) at each read.
    // Deliberately not folded into m_chan_mtx: that mutex guards the
    // channel-id/stray-queue string state specifically, and this field has
    // nothing to do with it.
    std::atomic<std::chrono::steady_clock::rep> m_phase_deadline{0};

    // How many times BeginBatchDestroyChannels/AddChannelToDestroy/
    // ExecuteBatchDestroyChannels has been attempted for the current
    // channel. Reset to 0 at the start of every probe().
    uint32_t m_destroy_attempts = 0;

    // ── Talkback audio path (Milestone 2) ──────────────────────────────────
    // The plugin CREATES this region and writes it; we open it read-write
    // because a reader must be able to clear the notify flag. See
    // src/talkback-ring.h for why the roles are reversed here.
    ShmRegion   m_audio_region{};
    std::string m_audio_region_name;
    uint32_t    m_audio_read_index = 0;
    uint32_t    m_audio_rate       = 0;
    uint16_t    m_audio_channels   = 0;
    bool        m_audio_open       = false;

    // F8 review-round fix: counts audio_send report emissions in
    // drain_audio() so it can report the first occurrence and then only
    // periodically, never once per drain. Without this, a stale
    // m_channel_id_z (fixed elsewhere in this round -- see the destroy
    // paths in tick()) made every buffer fail and every drain_audio() call
    // report it: ~50-100 pipe lines/sec, the message-storm shape this
    // codebase already has a live incident about. Reset whenever a fresh
    // region is opened so each session gets its own "first occurrence".
    uint32_t    m_audio_send_fail_count = 0;

    // Why the LAST open_audio() attempt was rejected, or empty if the last
    // attempt succeeded or none has been made. Task 3 fix round 2 (Major,
    // introduced by fix round 1's reorder).
    //
    // THREE STATES, not two, and that is the whole point: "open failed"
    // (non-empty), "open succeeded" (empty, m_audio_open true) and "no open
    // has happened yet" (empty, m_audio_open false). session_start() refuses
    // only on the first. Keying on !m_audio_open instead would conflate the
    // failure with the not-yet, and refuse every press on any ordering where
    // talkback_start reaches the engine before talkback_open -- which is the
    // order this feature shipped with until fix round 1.
    //
    // Why it exists: open_audio()'s four rejections (map_failed,
    // layout_mismatch, unsupported_rate, pipe_header_mismatch) each emit
    // report_session_state(false, reason), and session_start() emits
    // report_session_state(true, "live"). The plugin's talkback_session
    // handler is last-write-wins. Fix round 1 made the plugin open the tap
    // BEFORE talkback_start (so the ring is mapped before any key-path work),
    // which also made the audio failure arrive FIRST -- and be overwritten by
    // "live". The key then stayed open with the OPEN cue played, a live tally
    // shown, a dead-man switch kept fresh by the tap's own plugin-side stamp,
    // and NOTHING EVER SENT, because drain_audio() bails on !m_audio_open.
    // The director believes they are on air. That is the worst failure this
    // feature has, and the realistic trigger is layout_mismatch from a
    // DLL-only install, which CLAUDE.md calls a routine mistake here.
    //
    // Fixed as a DEPENDENCY, not as a message order: session_start() consults
    // this and refuses with the audio reason. Making the outcome depend on
    // which report lands last would be the same coin, flipped. No lock: every
    // reader and writer of this and of m_audio_open is the command-loop
    // thread (see the THREADING comment above open_audio()), which is a
    // stronger guarantee than a mutex, not a weaker one.
    std::string m_audio_fail_reason;

    // ── Persistent talkback session (Milestone 5) ──────────────────────────
    // Exactly one CreateChannel may be outstanding across the probe, the
    // session, and nomination (Task 2); see src/talkback-channel-owner.h for
    // why.
    //
    // Guarded by m_chan_mtx -- NOT command-loop-thread-only, despite an
    // earlier version of this comment claiming otherwise. Every WRITER but
    // one is the command-loop thread. Re-derive the list from the .cpp with
    // the grep in src/talkback-channel-owner.h rather than trusting a
    // paragraph -- that header's own inventory has been caught wrong about
    // itself four times, and this one enumerated seven sites and named
    // session_start() among the claimers, which Task 3 made false by removing
    // the session's CreateChannel. What is stable and worth stating: writers
    // are the two functions that CLAIM after a CreateChannel returns
    // SDKERR_SUCCESS (probe(), nomination_create_next()), the response
    // callback that releases, the teardown that CANCELS without releasing
    // (nomination_reset()), the lazy expiry, and tick()'s AwaitingChannel
    // timeout. That callback is safe on the
    // command-loop thread for the same reason
    // open_audio/drain_audio/close_audio are (see the THREADING comment
    // above the audio path below): on Windows this engine's main loop is
    // ALSO the SDK's message-pump thread, so every SDK callback, this one
    // included, runs there, not on some SDK-internal thread. The exception
    // is tick()'s AwaitingChannel-timeout clear (review-round R3 fix, see
    // tick()): that one genuinely runs on the probe's OWN separate driving
    // thread -- the seventh writer, and the only one off the command loop --
    // so this field needs the same cross-thread protection as the
    // channel-id strings below rather than being lock-free. Never call the
    // SDK while holding m_chan_mtx for this field either -- same discipline
    // as everywhere else in this class.
    // Fix round 5: ONE member, not four. This used to be the owner alone,
    // with m_session_create_cancelled, m_nomination_create_cancelled and
    // m_nomination_generation as three separate siblings that every
    // transition had to unpack into a TalkbackCreateState, mutate, and write
    // back field by field. Two defects came out of exactly that shape in one
    // round -- a branch that returned before its check-and-clear (F1), and a
    // re-review mutation that deleted ONE of the write-backs with the whole
    // 64-test suite still green -- so the state is now stored, transitioned
    // and written back whole. Everything that mutates it is a call to one of
    // the pure transitions in src/talkback-channel-owner.h, under this lock,
    // stored back as a single assignment; `grep -nE "m_pending_create *= "`
    // over engine-talkback.cpp finds every one of them (run it -- the
    // previous version of this block prescribed a regex that also matched
    // `==` and told the reader to expect four extra "writes").
    //
    // Its fields, and the defect each one exists because of:
    //   * owner -- who owns the outstanding create. F1/C1: nomination_reset()
    //     must NOT clear it (the create already went to Zoom; forgetting it
    //     strands the response on a stray queue nothing drains, which wedged
    //     the whole feature). session_stop()'s early branch was the other
    //     half of that pair until Task 3 removed the session's create.
    //   * session_cancelled / nomination_cancelled -- that owner tore down
    //     while its create was in flight, so the response destroys instead
    //     of adopting. N1: both must be cleared by BOTH clearers (the
    //     response and the expiry) or a stale flag destroys the next
    //     create's channel. As of Task 3 nothing in this engine SETS
    //     session_cancelled -- session_start() issues no CreateChannel, so
    //     there is no Session-owned create to cancel. The field and its
    //     transitions stay modelled and tested in
    //     src/talkback-channel-owner.h; they are simply not reached from
    //     here. Do not read that as "Session is impossible in principle" --
    //     read it as "no call site in this file claims Session", which is a
    //     property of the .cpp that a grep for TalkbackChannelOwner::Session
    //     confirms in one line.
    //   * generation -- which ladder issued the outstanding Nomination
    //     create, so a response for one the ladder gave up on is not
    //     mistaken for the one it is waiting on. Round 3 carried this as a
    //     FIFO and wedged the feature permanently the first time an entry
    //     went unmatched.
    TalkbackCreateState        m_pending_create;

    // The channels the CURRENT key press is talking on -- copied out of
    // m_provisioned_channels by session_start() and read by drain_audio().
    // Guarded by m_chan_mtx, like every other channel-id state here.
    //
    // A VECTOR, not one id, and that is Task 3's second half: an all-talent
    // target with more than 10 people owns ceil(n/10) channels (the SDK caps a
    // channel at 10 users), so the same PCM has to go to all of them in one
    // drain pass or the eleventh person hears nothing while the operator has
    // no way to know. Panel size stops being a special case: a private target
    // is simply the one-channel case of the same loop.
    //
    // Copies of the ids rather than indices into m_provisioned_channels: the
    // table can be destroyed and rebuilt by a re-nomination, and an index
    // would then point at a different channel rather than at nothing.
    // nominate()'s replace path and nomination_reset() both clear THIS too,
    // so a destroyed channel can never still be selected -- drain_audio()
    // then counts no_channel_drops loudly instead of sending into a channel
    // Zoom no longer has.
    std::vector<std::basic_string<zchar_t>> m_session_channels;
    std::string                m_session_target;  // UTF-8, reporting only
    bool                       m_session_live = false;

    // Fix round 1, M4: has the key-down duck actually been applied yet?
    //
    // The duck (SetChannelBackgroundVolume, see session_start()) used to run
    // ON the key press, and the round-1 review traced why that was worse than
    // it looked: talkback_start and talkback_audio are branches of the SAME
    // command loop, so session_start() runs to completion before any buffer is
    // read.
    //
    // Round 1 justified deferring it by saying that audio was then DISCARDED
    // (open_audio() snapped the read index past it). Fix round 2 removed that
    // mechanism -- this ring is re-laid-out per press, so open_audio() reads
    // from 0 -- so the justification is restated here rather than carried
    // forward: work on the key path DELAYS the first buffer, the ring bounds
    // that delay at 8 slots, and past those slots it is loss (counted as
    // `lost` now, which is not the same as avoided). The syllable the operator
    // is listening for is the first one.
    //
    // So the duck is deferred to the first drain_audio() of the press, AFTER
    // its sends. Talent hearing one buffer of director-over-unducked-meeting
    // is a far better failure than the director losing their first word.
    // Command-loop thread only, like the audio path itself; not under
    // m_chan_mtx, because it is not channel-id state and is never read off
    // that thread.
    bool                       m_session_duck_pending = false;
    // ...and was it applied, so session_stop() knows whether there is anything
    // to restore. A key released before the first buffer never ducked, and
    // restoring then would be N SDK calls to set 1.0 on channels already at
    // 1.0.
    bool                       m_session_ducked = false;

    // WHAT USED TO BE HERE, and why its absence is the fix rather than an
    // omission (Task 3): m_session_channel_z / m_session_channel /
    // m_session_participant / m_session_user_id / m_session_create_deadline,
    // plus the F1 cancellation machinery around them, all existed to manage
    // ONE thing -- a CreateChannel the session issued on the key press and
    // then had to reconcile with a response that might arrive after the key
    // was already released. Milestone 5 needed three fix rounds for that
    // reconciliation, and the round-6 review still parked a Major on it: two
    // presses in quick succession could have the first press's late response
    // adopted as live while the second's real response reached
    // m_stray_channels, which nothing drains -- has_pending_work() true
    // forever (the C1 wedge by another door) plus a channel orphaned on Zoom.
    //
    // Task 3 does not fix that race; it deletes the create that makes it
    // expressible. session_start() now only reads a table, so there is no
    // session-owned create, no response to arrive late, no deadline to expire
    // and no cancellation to remember. If a future change gives the session a
    // create back, it inherits ALL of that history, not just the code.

    // ── Pre-provisioned channels (Task 2, 2026-08-25) ───────────────────────
    // Fix round 1, C1: this is now a SECONDARY backstop, not the fix for a
    // create outstanding across Leave/quit -- the nomination cancellation
    // flag (m_pending_create.nomination_cancelled) is. This still matters for the case that flag does not cover: a
    // CreateChannel response that is genuinely never delivered at all (no
    // Leave, no cancellation, the SDK simply never calls back), which would
    // otherwise leave m_pending_create stuck at Nomination forever, refusing
    // every later probe()/session_start()/nominate() for the life of the
    // process. Read and lazily cleared by expire_stale_pending_create_locked(),
    // called from the three gate checks that evaluate the arbiter: probe(),
    // nominate(), and nomination_create_next(). session_start() is NOT one of
    // them any more and must not become one again: it issues no create, so it
    // has no gate to evaluate -- and handle_expired_create()'s Nomination arm
    // DESTROYS the provisioned set, which is the one thing a key press must
    // never do to the channel it is about to talk on.
    // Reuses kAwaitTimeout, the same bound tick()'s AwaitingChannel timeout
    // uses: both bound the same underlying wait (a Zoom CreateChannel
    // response), so there is no reason for a second constant.
    std::atomic<std::chrono::steady_clock::rep> m_nomination_create_deadline{0};

    // Fix round 1, C1 (CRITICAL): true when nomination_reset() (called from
    // Leave/quit) ran while a Nomination-owned CreateChannel was still
    // outstanding (m_pending_create == Nomination at that moment). Mirrors
    // m_pending_create.session_cancelled exactly, because this is the same bug F1
    // already fixed for Session, reintroduced here: the original
    // nomination_reset() cleared m_pending_create unconditionally and
    // returned, but the CreateChannel had already gone to Zoom. When its
    // response arrived, the arbiter saw None (owner == None), the id matched
    // no tracked channel, and it was queued onto m_stray_channels -- which
    // nothing drains without a probe's driving thread running
    // (drain_stray_channels() has exactly one caller, tick(), which has
    // exactly one caller, the probe's driving thread). has_pending_work()
    // then reads true forever (m_stray_channels non-empty), and
    // has_pending_work() gates the top of BOTH nominate() and
    // session_start() -- so one ordinary "nominate, then leave before the
    // create response is pumped" sequence permanently disabled the whole
    // talkback feature, citing a probe that never ran. Fix: leave
    // m_pending_create AS Nomination (so the eventual response still routes
    // to the Nomination branch in onCreateChannelResponse, not lost to
    // "owner == None") and set this flag instead; that branch destroys the
    // channel immediately on arrival rather than adopting it or queuing it
    // as a stray. Guarded by m_chan_mtx, same discipline as m_pending_create.
    //
    // Fix round 2, N1 (Major, introduced by fix round 1): this flag has TWO
    // clearers, not one. onCreateChannelResponse's Nomination branch clears
    // it when the create it belongs to actually responds. But if that
    // response is never delivered at all, expire_stale_pending_create_locked()
    // eventually expires the owner instead -- and round 1 only cleared
    // the session's flag there, not this one, so a cancelled-but-
    // never-answered create left this true forever. The next nominate() then
    // re-armed the owner with a brand-new CreateChannel, and when THAT
    // create's real response arrived, it found this flag still set and
    // destroyed the new channel instead of adopting it -- the operator's
    // next nomination silently provisioned zero channels. Both clearers must
    // stay in sync; see expire_stale_pending_create_locked()'s Nomination
    // arm for the fix and nominate()'s comment for why the ordering with
    // m_nomination_pending's assignment also had to change alongside this.
    //
    // Fix round 5, F1 (Major -- the SAME defect a third time, through a third
    // door): the flag also outlived its create when a branch of
    // onCreateChannelResponse RETURNED before reaching the check-and-clear.
    // The Stale branch did exactly that. The clear no longer lives in any
    // branch: talkback_create_response() does it as part of attributing the
    // response, before any branch runs, so there is nothing left to jump
    // over. This flag is now m_pending_create.nomination_cancelled above.

    // Fix round 3 ("expire-path double create"), now
    // m_pending_create.generation above: the cancellation flag answers "did
    // THIS create get cancelled" -- it says nothing about a create that
    // simply EXPIRED (nobody cancelled it, its response is just slow or
    // genuinely lost) and then has that response arrive AFTER a fresh
    // nomination re-armed the SAME owner and issued a SECOND CreateChannel.
    // onCreateChannelResponse cannot tell the two creates' responses apart
    // by owner alone -- Zoom gives no correlation id -- so a late response
    // for the FIRST one could be adopted as the SECOND ladder's channel 1
    // while the second create is still genuinely in flight, and the queue
    // would then issue a THIRD: two outstanding creates at once, the one
    // thing the arbiter exists to prevent. See
    // src/talkback-channel-owner.h's "Generation tracking" section for the
    // mechanism (mirrors src/shm-generation.h's fix for the same shape of
    // problem), and its TalkbackCreateState comment for why round 3's FIFO
    // became a permanent wedge and why the replacement is one scalar folded
    // into the arbiter state rather than a member of its own.

    // One entry per Zoom channel nominate() has successfully created --
    // populated by onCreateChannelResponse's Nomination branch, one at a
    // time, as create responses arrive. Members are the plan's stored
    // NAMES (see src/talkback-plan.h's TalkbackPlannedChannel), never ids:
    // ids are meeting-scoped, so a stored id would point at nobody after a
    // rejoin and at the wrong person once ids are recycled -- resolved to a
    // live id only at invite time, same rule as m_session_participant.
    //
    // Guarded by m_chan_mtx like every other channel-id state in this class.
    // Every writer today is the command-loop thread: onCreateChannelResponse's
    // Nomination branch (pushes an entry per successful create),
    // nomination_destroy_provisioned() (fix round 1, M2 -- drains and clears
    // the whole table before destroying each channel), and nomination_reset()
    // (clears on Leave/quit, bookkeeping only). Nomination never spawns a
    // driving thread the way the probe does, so nothing here has
    // m_channel_id_z's cross-thread Idle/Done hazard. It is guarded anyway
    // per this task's own instruction ("guard it with m_chan_mtx like every
    // other channel-id state") so a future reader that is NOT the command
    // loop -- a talkback_status query, say -- does not have to re-derive
    // whether this table is safe to read from elsewhere.
    //
    // Task 3 added the reader this table exists for: session_start() copies
    // the ids serving a target out of here under the lock. THE ID COPY IS
    // WHAT MAKES A KEY PRESS INSTANT -- there is no SDK call between the key
    // and the first buffer any more, which is the whole milestone.
    struct TalkbackProvisionedChannel {
        std::basic_string<zchar_t> channel_id_z;
        std::string channel_id;              // UTF-8, reporting only
        std::vector<std::string> members;    // by NAME, see above
        bool is_all_talent = false;
    };
    std::vector<TalkbackProvisionedChannel> m_provisioned_channels;

    // Channels talkback_plan() decided on that have not been created yet,
    // in plan order; the front entry is whichever CreateChannel is either
    // about to be issued or currently outstanding.
    //
    // Fix round 1, m3: the previous version of this comment named only
    // nominate(), onCreateChannelResponse's Nomination branch, and
    // nomination_reset() -- undercounting in exactly the way
    // src/talkback-channel-owner.h's sibling inventory was just caught doing
    // (M4). The full writer list, all command-loop thread: nominate()
    // (assigns the whole plan), nomination_create_next() (clears on a gate
    // refusal or a synchronous CreateChannel failure -- there is no retry
    // queue for those), onCreateChannelResponse's Nomination branch (pops
    // the front on a successful create, moving it into
    // m_provisioned_channels above; clears the rest on an error response),
    // expire_stale_pending_create_locked() (clears on a stale-Nomination
    // timeout), and nomination_reset() (clears unconditionally on
    // Leave/quit).
    std::vector<TalkbackPlannedChannel> m_nomination_pending;
};
