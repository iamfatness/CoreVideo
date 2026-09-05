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
// Every rung reports its own result (TalkbackResult for an SDK operation
// crossing the seam below, Zoom's own TalkbackError for a raw async callback)
// over E2P, so a failure names the exact rung it fell off instead of
// surfacing as silence.
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
// TalkbackSdk / TalkbackResult -- the macOS-port seam (2026-09-04): every
// call this file makes against IMeetingTalkbackController now goes through
// this normalised interface instead of the raw Zoom type, so a raw SDKError
// never reaches the ladder's own decisions (Law 2's backoff keys on exactly
// one normalised value, TalkbackResult::TooFrequent). No SDK dependency
// itself -- same reason talkback-plan.h has none.
#include "../../src/talkback-sdk.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class EngineTalkback : public ZOOMSDK::IMeetingTalkbackCtrlEvent {
public:
    // TalkbackSdk seam (Task 1, 2026-09-04): injects the adapter every
    // controller OPERATION below (create/invite/destroy/send/volume/
    // is_supported) is issued through. Callers own the adapter's lifetime and
    // decide WHEN to (re)inject -- this class deliberately never calls
    // m_svc->GetMeetingTalkbackController() to refresh it internally any
    // more (Steps 1-6 of this task's brief). Two reasons, not one:
    //   (1) a test must be able to inject a FakeTalkbackSdk and have it
    //       actually reached; an internal re-derivation from m_svc would
    //       silently overwrite it with whatever the test's fake meeting
    //       service's own GetMeetingTalkbackController() returns (nothing, in
    //       every existing fake), making the seam untestable.
    //   (2) resolve_roster_change()'s fix-round-1 (M3) guard specifically
    //       AVOIDS refreshing m_svc/the controller while a live key press or a
    //       busy nomination holds them, because a transient null picked up
    //       there used to persist for the rest of a live press. Removing the
    //       internal refresh everywhere is a strictly SAFER generalisation of
    //       that fix, not a narrower one: resolve_roster_change() now simply
    //       never re-derives the adapter, in EITHER of its branches, so the
    //       failure mode M3 closed cannot exist here at all. See
    //       tasks-1-report.md for the full reasoning.
    // On Windows, engine/src/main.cpp constructs a TalkbackWinSdk wrapping
    // svc->GetMeetingTalkbackController() and calls this before probe(),
    // nominate() and session_start() -- mirroring exactly where this class
    // used to refresh m_svc/the controller itself. It deliberately does NOT
    // call this before resolve_roster_change(): that function reuses whatever
    // was last injected, which is what makes point (2) above true.
    void set_sdk(TalkbackSdk *sdk) { m_sdk = sdk; }

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

    // ── The nomination ladder's pacing pump (LIVE GATE RUN 1, 2026-08-26) ───
    // Issues the nomination ladder's next CreateChannel once its not-before
    // deadline has passed, and nothing else. Called from main.cpp's command
    // loop on every idle turn of ipc_read_line_with_message_pump() -- which
    // wakes on a 50ms MsgWaitForMultipleObjects timeout, so the pump's
    // granularity is 50ms against a 600ms spacing (kMembershipCallSpacing --
    // Law 2 raised it from 300ms and made it shared with the invites). Cheap
    // and safe to call on
    // every turn: with nothing scheduled it is one mutex acquire and a
    // compare.
    //
    // WHY THIS IS NOT tick(), which is this file's other deadline-driven pump
    // and the obvious place to put it. tick() has exactly one driver: the
    // dedicated thread main.cpp spawns when probe() returns true, and only
    // then (see probe()'s return-value contract above and the batch-destroy
    // inventory at the top of tick()). During a nomination ladder that thread
    // does not exist -- main.cpp JOINS it before calling nominate(), and
    // nominate() refuses outright while has_pending_work() -- so a create
    // scheduled into tick() would never be issued at all. Worse, if one ever
    // were, it would be issued from the PROBE'S DRIVING THREAD, breaking both
    // the "CreateChannel is command-loop-thread-only" rule the arbiter is
    // built on and fact 2 of tick()'s batch-destroy chain. The two pumps run
    // on two different threads and must stay separate functions; naming this
    // one after tick() is as close as that gets.
    //
    // Command-loop thread only (nomination_create_next() asserts it).
    //
    // TALKBACK DELIVERY LAW 2 (ZComms, 2026-08-29, live 12-person meeting):
    // Zoom's rate limit is per MEMBERSHIP CALL, not per call KIND -- invites
    // draw SDKERR_TOO_FREQUENT_CALL exactly as creates do. So this is no
    // longer the create pump; it is the ONE membership-call pacer, and creates
    // and invites share its single ~600ms cadence (kMembershipCallSpacing).
    // See membership_pump_invite() below for the invite half and for why
    // creates take priority within a turn.
    void nomination_tick();

    // TALKBACK DELIVERY LAW 1 (ZComms, 2026-08-29, live): talkback delivers
    // ONLY while this client's own meeting audio is OPEN. Muted, every
    // SendAudioDataToChannel is ACCEPTED -- success codes, members confirmed,
    // zero failures -- and every member hears silence. A host can re-mute the
    // bot at any moment, including mid-key, so "open at session_start()" is
    // not a state that stays true; it has to be re-asserted.
    //
    // Rides main.cpp's command-loop idle pump beside nomination_tick(), NOT a
    // thread of its own: everything this touches (m_svc, the participants and
    // audio controllers, m_session_live) is command-loop state, and this file
    // already has one hard-won exception to that rule (the probe's tick()
    // thread) which is not worth a second. Cheap on every turn: with no live
    // session it is one bool read, and with one it is a deadline compare
    // against kMicReassertInterval (2s, ZComms's measured value).
    void mic_tick();

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

    // Fix round 1: the same "N of M present" computation session_start()'s
    // "session_live" report line makes, exposed as a direct, read-only
    // query instead of only a pipe line. Added because the fix-round review
    // (M1) found that an unchanged invite count alone cannot distinguish a
    // CONFIRMED member (TalkbackProvisionedChannel::present) from a member
    // this file merely stopped retrying after a permanent gate
    // (TalkbackProvisionedChannel::failed) -- both suppress re-invites
    // identically, so neither the ALREADY_EXIST-is-success test nor a future
    // one could tell "present" from "gated" by invite count alone.
    // `*present`/`*total` are left at 0 if `target` matches no provisioned
    // channel. Locks and delegates to members_present_locked() below --
    // fix round 2 (re-review residual 1): session_start() computed the
    // identical sum with a second, hand-written copy of the same loop, so
    // the test's accessor and the operator's report line could drift apart
    // silently. One source of truth now; both callers read it.
    void members_present_for_target(const std::string &target,
                                    std::size_t *present, std::size_t *total) const;

    // Fix round 2: pins the deadline sweep for a test without sleeping
    // kAwaitTimeout (10s) for real. TEST-ONLY -- no production call site
    // exists or should ever exist; a real caller has no legitimate reason to
    // want every pending invite to look 10s older than it is. Guarded by
    // m_chan_mtx like every other access to m_nomination_pending_invites.
    // See resolve_roster_change()'s test coverage for why this exists: the
    // re-review proved (mutation: disable ONLY the timed-out trigger, keep
    // the uid-left prune) that the deadline half of C1's fix was otherwise
    // unpinned -- 65/65 stayed green with it silently doing nothing.
    void debug_expire_pending_invites_for_test();

    // Task 5 fix round 3 (N6): the sibling hook for the OTHER expiry this
    // file has -- m_nomination_create_deadline, which guards a pending
    // CreateChannel response, not a pending invite. Same TEST-ONLY
    // discipline as debug_expire_pending_invites_for_test() above: no
    // production call site exists or should exist, and this only forces the
    // deadline into the past -- it does not itself run the expiry. The next
    // lazy self-heal (nominate()/nomination_create_next()/probe(), all of
    // which call expire_stale_pending_create_locked() before anything else)
    // is what actually fires it, exactly as a real timeout would.
    void debug_expire_pending_create_for_test();

    // LIVE GATE RUN 1 (2026-08-26): the third TEST-ONLY deadline hook, and the
    // sibling of the two above. Forces the nomination ladder's create-SPACING
    // deadline (m_nomination_next_create_at) into the past so a test can drive
    // the pacing without sleeping kMembershipCallSpacing per channel -- a
    // 13-channel plan would otherwise cost ~4s of real time in the suite. Like
    // its siblings it only moves the deadline; nomination_tick() is what
    // actually issues, exactly as the command loop's own pump would.
    //
    // LAW 2 (2026-08-29): it now expires ALL THREE deadlines the pacer has --
    // the create's own not-before, the shared membership-call not-before
    // (m_membership_next_at), and every QUEUED INVITE's per-entry code-18
    // backoff -- because once creates and invites shared one budget, a test
    // that expired only the create half would find the pump still closed and
    // read the result as "the ladder stalled". Deliberately kept under its
    // original name: same hook, same job, and renaming it would churn ~50 call
    // sites in the select test for nothing.
    //
    // REVIEW ROUND 1, m1: this comment used to say "BOTH halves" while the
    // code expired three things, and the unnamed third is what made the
    // invite-side backoff unpinnable -- see
    // debug_expire_membership_floor_for_test() below.
    void debug_expire_create_spacing_for_test();

    // LAW 2 / REVIEW ROUND 1, m1: the narrow hook for the SHARED FLOOR alone.
    // Advances the pacer's turn without touching any queued invite's own
    // code-18 backoff, which is the only way to express "the pacer is open but
    // this invite is still backed off" -- the entire content of "an 18 is a
    // wait, not a failure" on the invite side. TEST-ONLY, same discipline as
    // its siblings; it only moves a deadline, nomination_tick() still issues.
    void debug_expire_membership_floor_for_test();

    // LAW 2 (2026-08-29): the NARROW sibling of the hook above -- it expires
    // ONLY the create's own not-before and leaves the shared membership floor
    // standing. TEST-ONLY, same discipline as the rest.
    //
    // It exists because the wide hook cannot express the state Law 2 is
    // actually about ("a create is due, but a membership call just went out"),
    // and without that state the shared floor is UNPINNABLE: deleting it left
    // the whole suite green, because every test that expired the create
    // deadline expired the floor along with it. Found by mutation, not by
    // review -- the third time in this feature that a guard's only test also
    // disabled the thing it guards against.
    void debug_expire_create_schedule_for_test();

    // LAW 1 (2026-08-29): the mic re-assert's deadline, same TEST-ONLY
    // discipline as its three siblings above -- it only moves the deadline
    // into the past; mic_tick() is what actually re-asserts, exactly as 2s of
    // real command-loop idle would.
    void debug_expire_mic_assert_for_test();

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
    //
    // `attempt` is the plugin's own identity for THIS request, echoed back in
    // every TERMINAL report this attempt produces -- the seven early
    // refusals, nomination_abort_ladder()'s report, and "nominate_done" (see
    // m_nomination_attempt). Final-review C1 (CRITICAL): the plugin stages an
    // in-flight nomination in ONE slot, so a re-nomination sent while this
    // ladder is still provisioning re-stages that slot, and without an id the
    // earlier ladder's "nominate_done" committed the LATER attempt's nominee
    // list against the earlier ladder's channels. 0 means "the requester did
    // not identify this attempt" (a raw-pipe caller, or an older plugin) and
    // suppresses the field entirely, so those reports look exactly like a
    // pre-C1 engine's and the plugin's tolerant path handles them.
    bool nominate(ZOOMSDK::IMeetingService *svc,
                  const std::vector<std::string> &nominees,
                  uint32_t attempt = 0);

    // Bookkeeping-only reset for the nomination table/queue -- never calls
    // the SDK. Called from Leave/quit in engine/src/main.cpp because
    // provisioned channels and their membership are meeting-scoped (see the
    // design doc's "Meeting rejoin" row in the failure table): once the
    // meeting is gone there is nothing left on Zoom's side to select or
    // destroy, so clearing our own record of it is all that is needed. Does
    // NOT destroy anything meeting-side -- there is no un-nominate SDK call
    // in this task, and none is needed here for the same reason.
    void nomination_reset();

    // ── Roster re-resolution (Task 4, 2026-08-25) ───────────────────────────
    // Nominations store NAMES, never Zoom user ids, because ids are
    // meeting-scoped: an id points at nobody after a rejoin and at the wrong
    // person once ids are recycled. This function is the other half of that
    // rule. Every time the roster changes -- five SDK callbacks in
    // engine/src/main.cpp (onUserJoin, onUserLeft, onUserNamesChanged,
    // onUserAudioStatusChange, onUserVideoStatusChange) all funnel here --
    // re-resolve every nominated name against who is actually in the meeting
    // right now: invite anyone newly present into the channels their name is
    // planned for, and report anyone whose name dropped out of a channel's
    // roster. A talent who drops and rejoins therefore ends up back in their
    // channels with no operator action -- that is the entire point of
    // storing names instead of ids, finished.
    //
    // A RENAME (onUserNamesChanged) needs no special case: it is the SAME
    // generic diff. The old name disappears from the roster (a LEAVE) and the
    // new name may or may not match a nominated name (a JOIN if it does).
    //
    // THE RULING THIS FUNCTION EXISTS UNDER: it may INVITE, and must NEVER
    // CREATE. Channels are provisioned once, at nomination time
    // (nominate()/nomination_create_next()), specifically so nothing on this
    // path ever needs a create -- CreateChannel is command-loop-thread-only
    // under the arbiter's single-outstanding-create rule
    // (src/talkback-channel-owner.h), and this function must never need to
    // try. If a name has no channel to join, that is a planning gap for the
    // operator to fix with a fresh nominate() -- report it, never create one
    // here. (Fix round 1, M2: this was previously pinned only behind the
    // has_pending_work() refusal below, where the function does nothing at
    // all -- a mutant `CreateChannel` inserted on the LIVE invite path left
    // all 65 tests green. A second test now asserts the ruling on a
    // resolution that actually invites.)
    //
    // TWO GUARANTEES, from two different sources -- do not conflate them.
    // (1) SERIALIZATION with the command loop: on Windows, the five roster
    // callbacks and the command loop are the SAME THREAD --
    // ipc_read_line_with_message_pump() (main.cpp) pumps the SDK's messages
    // ON the command-loop thread, the same fact the audio path's THREADING
    // comment already relies on. So this function can only ever run BETWEEN
    // command-loop turns, never inside one: the interleaving with
    // nominate()'s own ladder is TEMPORAL (which roster event lands between
    // which two responses), not concurrent, and there is no data race to
    // reason about with the command loop itself. (2) m_chan_mtx around every
    // access to m_provisioned_channels/m_nomination_pending_invites/
    // TalkbackProvisionedChannel::present exists for a DIFFERENT reason and
    // is NOT redundant with (1): the probe's tick() driving thread is a
    // genuine second thread, has_pending_work() only excludes it while it is
    // known to be running, and any future platform or future change that
    // makes the roster callbacks arrive off the command-loop thread inherits
    // this file's cross-thread state, not just its single-thread ordering.
    // Keep both: (1) is what makes today's interleaving reasoned about
    // (below and in the Task 4 review's Q4), (2) is what keeps that
    // reasoning from silently depending on today's thread model.
    //
    // Idempotent, with two independent forms of idempotence that are easy to
    // conflate:
    //   * A confirmed presence (TalkbackProvisionedChannel::present, set by
    //     onChannelUserJoinResponse on TALKBACK_ERROR_OK or
    //     TALKBACK_ERROR_ALREADY_EXIST) is never re-invited while the person
    //     stays in the meeting.
    //   * A CONFIRMED FAILURE (TalkbackProvisionedChannel::failed, fix round
    //     1, M1) is ALSO never re-invited while the person stays in the
    //     meeting -- a permanent gate (IsSupportTalkback() == false, most
    //     commonly) must not be retried on every roster event, and two of
    //     the five callbacks (onUserAudioStatusChange, onUserVideoStatusChange)
    //     fire on every mute and every camera toggle by anyone in the
    //     meeting, not just on an actual roster change. `failed` is cleared
    //     ONLY when the person's name actually leaves the roster -- a
    //     person-scoped state change is the only signal that plausibly
    //     changes the outcome; a timer would just space out the same retries
    //     without ever answering whether retrying helps.
    // A burst of the five callbacks for the SAME underlying change therefore
    // invites (or fails to invite) exactly once, and reports nothing extra,
    // whether that one outcome is success or a permanent gate.
    //
    // FIX ROUND 1, C1 (CRITICAL): a pending invite (m_nomination_pending_invites)
    // is now swept for two independent reasons before anything else runs --
    // see that member's header comment for the two triggering sequences this
    // closes and why one alone is not enough.
    //
    // FIX ROUND 1, M3 (Major): m_svc used to be reassigned ONLY when no
    // session is live -- probe() and nominate() both refuse outright while
    // m_session_live specifically because they touch it; this function
    // cannot refuse outright (a rejoin mid-press must still be invited), so
    // it left the press's own pointer alone and reused it instead. The
    // failure this closed: refreshing unconditionally could observe a
    // TRANSIENT null from GetMeetingTalkbackController() (a reconnect/ending
    // window) and that null then persisted for the rest of a live press.
    //
    // TASK 1 (2026-09-04) GENERALISED THIS rather than narrowly porting it:
    // this function no longer re-derives the SDK adapter (m_sdk) from m_svc
    // AT ALL, in either branch -- see set_sdk()'s own comment for why. That
    // makes the M3 failure mode structurally impossible here instead of
    // conditionally avoided, at the cost of never picking up a legitimately
    // fresher adapter via a roster event either; membership_pump_invite()
    // (the only later reader of m_sdk this function's work feeds) simply
    // keeps using whatever probe()/nominate()/session_start() last injected,
    // which main.cpp keeps alive for the life of the meeting. m_svc itself
    // keeps the ORIGINAL M3 conditional (reassigned only when no session is
    // live and invites are allowed): current_roster()/resolve_participant()
    // still read it, and that half of M3 is unrelated to the adapter.
    //
    // Gated on has_pending_work() before touching m_sdk, same as
    // nominate()/session_start(): when it invites, this ends up spending a
    // pacer turn on BeginBatchInviteUsers/AddUserToInvite/ExecuteBatchInviteUsers
    // (via m_sdk->invite_users(), through membership_pump_invite() --
    // never issued directly from here, see LAW 2) -- the same shape of
    // Begin/Add/Execute sequence tick()'s own inventory documents as unsafe
    // to interleave, on different threads, with the probe's driving thread.
    // Unlike nominate()/session_start(), a refusal here is not reported back
    // to an operator waiting on a command response -- it costs nothing but a
    // delay, because nothing is marked resolved when it refuses: the next
    // roster event (there is always another one eventually) gets another
    // chance.
    void resolve_roster_change(ZOOMSDK::IMeetingService *svc);

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
    // "false" means the driving thread will not touch m_sdk. That was wrong
    // for one specific window: drain_stray_channels() swaps m_stray_channels
    // into a local UNDER m_chan_mtx, releases the lock, and only THEN runs
    // its destroy_channels() calls (Begin/Add/ExecuteBatchDestroyChannels,
    // Task 1) against m_sdk. In that
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
    // trace above.
    //
    // WHERE IT IS EMITTED, re-counted from the .cpp (final review, m2 -- the
    // previous wording said "after the invite is accepted" and named
    // onCreateChannelResponse(), which has emitted neither since Task 3
    // deleted the Session branch and the invite on the key path; believing
    // some other site drove "live" is what hid final-review C2):
    //   * live:true  -- session_start()'s ONE success line, and nowhere else.
    //   * live:false -- session_start()'s five early returns, open_audio()'s
    //     five rejections, and nomination_abort_ladder() when a teardown
    //     orphans a LIVE key (reason "channels_destroyed", final-review C2).
    // Shape: {"cmd":"talkback_session","live":true|false,
    // "reason":"..."} -- see ZoomEngineClient::handle_event()'s
    // talkback_session branch (distinguishes this from a stage line by the
    // presence of "live") and TalkbackController::evaluate()/status_json()
    // for the consumer side.
    //
    // LAW 1 (2026-08-29): `mic` is the third, OPTIONAL field. nullptr emits
    // nothing, so every one of the eleven pre-existing call sites is
    // byte-identical to before; session_start()'s live line passes "open" or
    // "blocked", and mic_tick() re-emits on a mid-key CHANGE.
    //
    // It rides THIS line rather than only the session_live stage line because
    // this is the one the plugin's state machine consumes. THE CONSUMER,
    // named so this comment can be checked rather than believed (review round
    // 1, M1: the previous version made this same claim while nothing on the
    // plugin side read the field at all):
    //   ZoomEngineClient::handle_event()'s talkback_session live-line branch
    //   (src/zoom-engine-client.cpp) -> TalkbackSessionStatus::mic_blocked
    //   (src/zoom-engine-client.h) -> TalkbackDockSessionView::mic_blocked ->
    //   talkback_dock_banner() (src/talkback-dock-state.h), which returns
    //   TalkbackDockBannerState::LiveMicBlocked and the headline
    //   "ON AIR - BOT MUTED", pinned in tests/talkback-dock-state-test.cpp.
    // A missing field is tolerated on the plugin side, same mixed-version rule
    // as the nomination attempt id: a DLL-only install is this project's
    // canonical mistake.
    void report_session_state(bool live, const std::string &reason,
                              const char *mic = nullptr) const;

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

    // Fix round 1: `(user_id, name)`, not a bare name -- see current_roster()
    // below for why the id is now needed too.
    struct TalkbackRosterEntry {
        unsigned int user_id = 0;
        std::string name;
    };

    // Task 4: every participant currently in the meeting, via the same
    // controller resolve_participant() uses one name at a time.
    // resolve_roster_change() needs the whole roster to diff against, not a
    // single lookup. Empty if there is no meeting service, no controller, or
    // no participants list -- callers treat that the same as "nobody is
    // present" rather than an error, same null-safety resolve_participant()
    // already has. const for the same reason resolve_participant() is: both
    // only read m_svc, and command-loop-thread-only convention (not a lock)
    // is what makes that safe -- see m_pending_create's header comment for
    // why that convention is stated once there rather than re-argued here.
    //
    // Fix round 1, C1: originally returned bare names (current_roster_names()).
    // The id is now needed too -- resolve_roster_change()'s pending-invite
    // sweep has to ask "is the id this invite was issued to still in the
    // meeting", which a name-only roster cannot answer (the same name could
    // now belong to nobody, or -- after a rename collision -- to someone
    // else's rejoin). One participants-controller walk answers both
    // questions; this does not add a second walk beyond what the old
    // name-only version already did.
    std::vector<TalkbackRosterEntry> current_roster() const;

    // Fix round 2 (re-review residual 1): the "N of M present" sum, factored
    // out so members_present_for_target() and session_start() share ONE
    // implementation instead of two hand-copied loops that could silently
    // drift (the re-review's own finding: the test asserted the accessor,
    // the operator read session_start()'s inline copy, and nothing kept them
    // equal). CALLER MUST HOLD m_chan_mtx -- m_chan_mtx is non-recursive and
    // session_start() already holds it in the scope where it wants these
    // counts, so this cannot lock internally the way the public
    // members_present_for_target() wrapper does.
    void members_present_locked(const std::string &target,
                                std::size_t *present, std::size_t *total) const;

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
    // gate refuses or the SDK call itself fails synchronously -- a stalled
    // ladder is reported, not silently abandoned.
    //
    // LIVE GATE RUN 1 (2026-08-26) changed two things about that last
    // sentence. (a) The second and later calls now come from nomination_tick()
    // rather than directly from onCreateChannelResponse -- see
    // nomination_schedule_create() below for the live failure that forced it.
    // (b) There IS a retry queue now, for exactly one synchronous failure:
    // SDKERR_TOO_FREQUENT_CALL. That one schedules a backed-off retry of the
    // SAME channel and returns TRUE (the ladder is still alive, just not yet
    // in flight -- the one case where a true return does not mean "a create
    // is outstanding"); every other synchronous failure keeps the terminal
    // abort. Retries are capped at kMaxNominationCreateRetries per channel,
    // and exhausting them aborts with reason "create_rate_limited" so the
    // operator's log says why rather than blaming CreateChannel generically.
    bool nomination_create_next();

    // LIVE GATE RUN 1 (2026-08-26): arms the not-before deadline
    // nomination_tick() issues against. Two callers, both in this file:
    // onCreateChannelResponse's Nomination branch (kMembershipCallSpacing,
    // for the next channel of the plan) and nomination_create_next() itself
    // (a doubling kNominationRateLimitBackoff, retrying the SAME channel).
    //
    // WHY THE LADDER IS PACED AT ALL. In the first live gate (2026-08-26
    // 20:04, real meeting, two-channel plan) the ladder issued channel 2's
    // CreateChannel synchronously from inside channel 1's
    // onCreateChannelResponse -- a 0ms gap -- and Zoom refused it with
    // SDKERR_TOO_FREQUENT_CALL (18). The ladder then correctly aborted
    // terminally, which meant NO nomination with more than one channel could
    // ever succeed live: every real talent list is more than one channel.
    // Zoom rate-limits back-to-back creates; no unit test could have caught
    // that, because the fake controller has no rate limit.
    //
    // WHY THE ARBITER CLAIM IS NOT HELD ACROSS THE WAIT. A scheduled create is
    // not an OUTSTANDING create -- Zoom has never seen it -- and the arbiter's
    // single-outstanding-create rule is about responses that cannot be told
    // apart. Claiming early would also arm m_nomination_create_deadline
    // (kAwaitTimeout) against a request that was never issued, so a spacing
    // wait would eventually read as a swallowed response and self-expire the
    // ladder it is pacing. The claim is therefore taken at ISSUE time, in
    // nomination_create_next(), exactly as it always was; the ~600ms window
    // where the arbiter is free is closed on the one path that could abuse it
    // (nominate() refuses with "create_busy" while a create is scheduled, the
    // same non-destructive early refusal it already gives while one is
    // outstanding), and left open on the other (a probe may take the arbiter,
    // and nomination_create_next()'s gate check then ends the ladder
    // terminally via nomination_abort_ladder("create_busy") -- the existing,
    // tested mid-ladder path, not a new one).
    void nomination_schedule_create(std::chrono::milliseconds delay);

    // ── LAW 2: the shared membership-call pacer (2026-08-29) ────────────────
    // Stamps m_membership_next_at kMembershipCallSpacing into the future.
    // Called immediately after ANY membership call reaches Zoom -- the
    // CreateChannel in nomination_create_next() and the
    // Begin/Add/ExecuteBatchInviteUsers in membership_pump_invite() -- because
    // the rate limit ZComms measured counts CALLS, not call kinds, and a pacer
    // that only counted one of the two is the defect this change exists to
    // remove. Stamped on the CALL, not on its answer: a refused call was still
    // a call, and it is exactly the calls Zoom refuses that must not be
    // followed instantly by another.
    void stamp_membership_call();

    // Issues AT MOST ONE invite SDK call and returns whether it did. Called
    // only from nomination_tick(), only when no create is due, and only once
    // per turn -- that is the whole pacing mechanism on the invite side.
    //
    // It may LOOP internally, and that is deliberate rather than sloppy: a
    // queued invite whose nominee is not currently in the meeting resolves to
    // user id 0 and makes NO SDK call at all, so consuming the 600ms budget
    // for it would let a plan full of absent names starve the ones who are
    // present. The loop skips those (reporting each, exactly as the old
    // synchronous path did) and stops at the first entry that actually reaches
    // Zoom.
    //
    // ROUND-ROBIN, per ZComms's law: among the entries eligible right now it
    // prefers one whose channel is NOT the channel the last invite went to.
    // A 10-member all-talent channel enqueues ten at once, and strict FIFO
    // would leave the last talent's own private channel waiting six seconds
    // behind a queue that has nothing to do with them.
    bool membership_pump_invite();

    // Queues an invite instead of issuing it. Was invite_nominee(), which
    // called Begin/Add/Execute inline; LAW 2 is that it may not, because two
    // of its call sites (onCreateChannelResponse's per-channel member loop and
    // resolve_roster_change()'s per-name loop) fire in BURSTS -- all members of
    // a channel back to back, and every re-resolved name at once -- which is
    // precisely the shape Zoom answers with SDKERR_TOO_FREQUENT_CALL.
    //
    // Deduped on (channel, name) against the queue itself, so a burst of the
    // five roster callbacks for one join cannot enqueue the same invite five
    // times. That is the SECOND half of the idempotence
    // resolve_roster_change() already had against m_nomination_pending_invites:
    // before this change an invite became "pending" the instant it was issued,
    // and now there is a window where it is queued but not yet issued, which
    // that check alone cannot see.
    void enqueue_invite(const std::basic_string<zchar_t> &channel_id_z,
                        const std::string &channel_id_utf8,
                        const std::string &name);

    // ── LAW 1: this client's own meeting audio (2026-08-29) ─────────────────
    // Reads the authoritative self-mute state (IUserInfo::IsAudioMuted() on
    // GetMySelfUser(), the participants controller's own answer -- there is no
    // "am I muted" on IMeetingAudioController) and, if muted, UnMuteAudio()s
    // this user. Returns whether the mic ended up OPEN.
    //
    // `when` is "session_start" or "tick" and decides the reporting volume:
    // the key edge always reports, a tick reports only when it actually found
    // the mic shut (a host re-muting the bot mid-key is worth a line; "still
    // open" every 2s for the length of a latched key is the message-storm
    // shape this codebase already has a live incident about).
    //
    // NOT A GATE. A meeting that forbids self-unmute leaves the key LIVE and
    // reports "mic":"blocked" -- refusing the key would take away the
    // director's only remaining option (ask the host to unmute the bot) while
    // they are trying to talk, and the audio path is genuinely intact; what is
    // missing is a permission only the host can grant. Fail LOUD, not closed,
    // for this one.
    bool ensure_mic_open(const char *when);

    // The other half of ensure_mic_open(): if this file muted-to-unmuted the
    // client for THIS key, put it back on release.
    //
    // THE SEMANTIC, stated because the alternative is defensible and this is a
    // choice: restore iff THIS FILE EVER TOOK THE CLIENT FROM MUTED TO
    // UNMUTED DURING THIS KEY -- at session_start(), or at any mic_tick()
    // re-assert after a host re-muted the bot mid-key -- and that unmute
    // succeeded.
    //
    // REVIEW ROUND 1, m3: this used to say "observed MUTED at the moment the
    // key opened", which described a narrower rule than ensure_mic_open()
    // implements -- it arms m_mic_restore_pending regardless of `when`. The
    // CODE is the better of the two and is what stands: a bot the host muted
    // at second 30 of a latched key was still muted-by-the-host when the key
    // closes, and leaving it hot because the mute happened after the press
    // rather than before it would be an accident of timing deciding whether a
    // control room's mic stays open. The comment is corrected to the code
    // rather than the code narrowed to the comment.
    //
    // A bot the host had muted
    // must not be left hot after the key closes -- from the room's side an
    // open mic on a machine running a live production is the worse failure --
    // and a bot that was already unmuted is left exactly as found. There is no
    // "re-key pending" exception, because this engine has no such state: every
    // press is its own session_start()/session_stop() pair (latch included),
    // and inventing a pending-re-key window would mean guessing at an operator
    // intent the wire never carries. The cost is one UnMuteAudio per press on
    // a host-muted bot, which is one membership-unrelated SDK call on the
    // RELEASE path, where latency does not reach the audience.
    void restore_mic_state();

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

    // Task 5 fix round 3 (N6, Major). Every path that ends a nomination
    // ladder by tearing down what it (or the replace step before it) had
    // provisioned funnels through here: clears the not-yet-created queue,
    // destroys whatever IS provisioned, and reports the terminal outcome --
    // ONE function, so a future abort branch cannot tear down without also
    // reporting, because the report is no longer a separate step to forget.
    // The round-2 re-review found the report missing on three of five
    // structurally identical branches (only two, both inside
    // nomination_create_next(), had been fixed after N1); this is what makes
    // that specific omission inexpressible going forward. `reason` becomes
    // the report's `"reason"` field; the report always carries
    // `"channels_destroyed":true` (see src/talkback-nomination.h /
    // src/talkback-nomination-dispatch.h on the plugin side for why that
    // field, not the reason string, decides the plugin's response). Reported
    // AFTER the destroy, same discipline as every other report/SDK-call
    // ordering in this file: never under m_chan_mtx.
    //
    // FINAL REVIEW, C2 (CRITICAL) -- the THIRD responsibility: un-live a
    // session this teardown just orphaned. A key press IS allowed while a
    // ladder runs (session_start() gates only on `still_coming` for its own
    // target, and refusing "all" because an unrelated private channel is
    // still creating would deny a ready target for an unrelated reason), so
    // "all" can be live when a later create fails. This function then
    // batch-destroys every provisioned channel and empties the selection
    // underneath that press. Nothing else reports post-live state, so before
    // the fix the engine kept m_session_live true, the plugin kept
    // TalkbackSessionStatus.live true, the tally stayed red, the OPEN cue had
    // already played -- and nothing reached Zoom. The director believes they
    // are on air: the worst failure this feature has. So this function stops
    // the session (session_stop(), which restores the duck while the channels
    // still exist) and emits report_session_state(false,
    // "channels_destroyed"), which the plugin's evaluate() already treats as
    // an explicit failure and closes the key on.
    void nomination_abort_ladder(const std::string &reason);

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
    // Never called with m_chan_mtx held (dereferences m_sdk); *attempts (if
    // non-null) receives how many tries it took, for callers that report it.
    //
    // Task 1 (2026-09-04): takes the UTF-8 channel id (every caller already
    // has one) and returns TalkbackResult rather than a raw ZOOMSDK::SDKError
    // -- global constraint: the ladder never sees a raw SDK error code.
    // Internally calls m_sdk->destroy_channels() with a single-element list
    // per attempt, which is exactly the one-channel
    // Begin/Add/ExecuteBatchDestroyChannels sequence this used to spell out
    // by hand.
    TalkbackResult destroy_channel_retrying(const std::string &channel_id_utf8,
                                            uint32_t *attempts);

    // Resolves `name` to a live user id and, if found, invites it into the
    // already-created channel `channel_id_z`. Deliberately independent of
    // the create-queue machinery above -- it takes a channel id and a name,
    // nothing about m_nomination_pending or the arbiter -- so a future
    // roster-driven re-invite (Task 4, ruled to run on the SDK callback
    // thread and to NEVER call CreateChannel there) can call this same
    // primitive without touching create-side state at all. A name not
    // currently in the meeting is reported and skipped, not an error --
    // that is the expected shape for someone who has not joined yet.
    //
    // LAW 2 (2026-08-29) changed WHO calls this and what it returns. It is no
    // longer called from the provisioning loop or from the roster path -- both
    // enqueue_invite() now -- and its ONE caller is membership_pump_invite(),
    // which has already spent the pacer's turn on it. The return value is
    // "did an SDK membership call actually reach Zoom", so the pump can tell a
    // spent turn from a skipped name (uid 0, nobody by that name in the
    // meeting right now) without re-deriving it.
    //
    // `retries` is how many SDKERR_TOO_FREQUENT_CALL refusals this
    // (channel, name) has already ridden out, carried in from the queue entry
    // so an 18 can re-queue itself with a doubling backoff instead of being
    // reported as a failure. THAT IS THE OTHER HALF OF LAW 2: an 18 on an
    // invite means exactly what an 18 on a create means -- Zoom saying "not
    // yet" -- and the pre-Law-2 code reported it as a plain `"stage":"invite"`
    // failure and then relied on some later roster event to try again, which
    // for a name already marked present or failed would never come.
    bool invite_nominee(const std::basic_string<zchar_t> &channel_id_z,
                        const std::string &channel_id_utf8,
                        const std::string &name, uint32_t retries);

    ZOOMSDK::IMeetingService          *m_svc  = nullptr;
    // TalkbackSdk seam (Task 1, 2026-09-04): every SDK OPERATION this file
    // needs (create/invite/destroy/send/volume/is_supported) now crosses
    // through this normalised interface (src/talkback-sdk.h) instead of a raw
    // ZOOMSDK::IMeetingTalkbackController*, so the ladder's own logic never
    // sees a Zoom type or a raw SDKError. Set via set_sdk(), which this file
    // itself never calls: production (engine/src/main.cpp) constructs a
    // TalkbackWinSdk wrapping whatever m_svc->GetMeetingTalkbackController()
    // returns and injects it before probe()/nominate()/session_start(); a
    // test injects a FakeTalkbackSdk the same way. This deliberately REMOVES
    // the internal `m_sdk = m_svc->GetMeetingTalkbackController()` refresh
    // every entry point used to do for itself -- see set_sdk()'s own comment
    // for why re-deriving it here would silently discard whatever a caller
    // (test or production) just injected.
    TalkbackSdk *m_sdk = nullptr;

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
    // Begin/Add/ExecuteBatchDestroyChannels loop against m_sdk. Neither
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
    // Fix round 1, M4: `present` needs the id it was confirmed under, not
    // just the name -- onChannelUserLeaveResponse (a CHANNEL-side removal,
    // distinct from a MEETING departure) carries (channelID, userID) and
    // nothing else, and names are deliberately never sent to the SDK, so the
    // id captured at confirmation time is the only key that response can be
    // matched against without a second participants-controller lookup (which
    // could fail anyway if the person already left the meeting entirely by
    // the time the response arrives). The id is purely a LOCAL correlation
    // key for THIS presence stint -- never compared against a plan, never
    // used to decide who to invite (that is always resolve_participant()
    // re-resolving the NAME at invite time) -- so storing it here does not
    // reintroduce the meeting-scoped-id problem this whole file is built to
    // avoid: a rejoin gets a brand new TalkbackPresentMember under its own
    // fresh id, not a stale one reused.
    struct TalkbackPresentMember {
        std::string name;           // the plan's identity; see above
        unsigned int user_id = 0;   // this stint's id; see above
    };

    struct TalkbackProvisionedChannel {
        std::basic_string<zchar_t> channel_id_z;
        std::string channel_id;              // UTF-8, reporting only
        std::vector<std::string> members;    // by NAME, see above
        bool is_all_talent = false;
        // Task 4: the subset of `members` this file currently believes are
        // actually in the channel -- populated by onChannelUserJoinResponse
        // when a nomination invite is confirmed (TALKBACK_ERROR_OK or
        // TALKBACK_ERROR_ALREADY_EXIST, both treated as success), pruned by
        // onChannelUserLeaveResponse on a CHANNEL-side removal (fix round 1,
        // M4) and by resolve_roster_change() when a member drops out of the
        // MEETING roster entirely. NOT the same fact as "is in `members`" --
        // that is the PLAN, this is what Zoom has actually confirmed -- and
        // not the same fact as "is in the meeting" either: being in the
        // meeting and being in THIS channel are different things, and only
        // an invite (or leave) response confirms which one applies. This is
        // what session_start()'s "N of M present" report line counts, and
        // what resolve_roster_change() reads to decide who still needs
        // inviting (present_here && !was_present) and who has left
        // (!present_here && was_present) -- see that function's own comment.
        std::vector<TalkbackPresentMember> present;
        // Fix round 1, M1 (Major): names resolve_roster_change() invited and
        // received a NON-OK, NON-ALREADY_EXIST response for (a permanent
        // gate, most commonly IsSupportTalkback() == false) -- set by
        // onChannelUserJoinResponse, cleared ONLY when the person's name
        // leaves the roster (resolve_roster_change()'s departure branch).
        // Gates present_here && !was_present the same way `present` does, so
        // a permanently-failing invite is attempted exactly ONCE per
        // presence stint rather than on every roster event -- see
        // resolve_roster_change()'s header comment for why a person-scoped
        // clear, not a timer, is the right trigger. Never counted in
        // "N of M present": failing to reach someone is not the same fact as
        // reaching them.
        std::vector<std::string> failed;
    };
    std::vector<TalkbackProvisionedChannel> m_provisioned_channels;

    // Task 4: a nomination invite this file is waiting on Zoom to confirm or
    // refuse via onChannelUserJoinResponse. Needed because
    // ExecuteBatchInviteUsers is asynchronous -- its own SDK doc comment
    // says so -- and its SYNCHRONOUS return value is only "the batch call
    // was accepted", never "the user is in the channel". Without this
    // correlation there is nowhere for TALKBACK_ERROR_ALREADY_EXIST (which
    // can ONLY ever arrive via this async response, never synchronously) to
    // land, and before this task every nomination invite response was
    // silently discarded by onChannelUserJoinResponse's own m_phase ==
    // AwaitingInvite guard, which belongs to the PROBE's ladder and has no
    // idea a nomination invite happened at all.
    //
    // Guarded by m_chan_mtx, like every other channel-id/membership state in
    // this class. Writers: invite_nominee() pushes an entry after a
    // synchronously-successful ExecuteBatchInviteUsers (called from both the
    // initial provisioning loop in onCreateChannelResponse and from
    // resolve_roster_change()); onChannelUserJoinResponse erases the
    // matching entry when its response arrives; resolve_roster_change()'s
    // sweep (below) erases an entry that can never usefully resolve. Bounded
    // by the number of outstanding invites -- resolve_roster_change() will
    // not issue a second invite for a name that already has one pending --
    // so this never grows past the size of the nomination plan.
    //
    // FIX ROUND 1, C1 (CRITICAL -- the cost below was WRONG). This used to
    // read: "An entry whose response never arrives at all ... is left here
    // permanently ... the cost of that gap is a few stray bytes per orphaned
    // entry ... not a wedge of the feature ... so it is accepted rather than
    // built." That was false, and the false costing is what made accepting
    // the gap look reasonable: the suppression check
    // (resolve_roster_change()) keys on (channel, NAME); the only clearer
    // (onChannelUserJoinResponse) keys on (channel, USER ID). A stuck entry
    // for a stale id therefore blocks every future invite for that NAME
    // forever, which is a permanent, silent suppression of the rejoin this
    // whole feature exists to guarantee -- proved live by two sequences:
    //   A) join -> invite -> response never arrives -> leave -> rejoin under
    //      a new id: every later roster event finds the stale entry still
    //      "pending" for that name and never invites the new id.
    //   B) join -> invite -> leave BEFORE the response -> rejoin under a new
    //      id -> the STALE response for the OLD id finally arrives, is
    //      matched (ids are matched, not staleness-checked), and marks the
    //      NAME present -- the operator sees "1 of 1 present" for a talent
    //      who was never invited under the id they are actually in the
    //      meeting with.
    // TWO INDEPENDENT FIXES, because they close different halves: a DEADLINE
    // (`deadline` below, swept in resolve_roster_change()'s lock scope,
    // reusing kAwaitTimeout -- same bound tick()'s AwaitingChannel timeout
    // and expire_stale_pending_create_locked() use for the identical "this
    // SDK swallows responses" reason) closes sequence A; dropping any entry
    // whose `user_id` has left the roster (checked against the same
    // current_roster() snapshot resolve_roster_change() already has to
    // fetch) is the SEMANTICALLY RIGHT trigger and closes sequence B
    // immediately rather than after up to kAwaitTimeout -- an id that no
    // longer exists in the meeting cannot receive a response that means
    // anything. Keep both: the deadline is the backstop for "no roster event
    // ever tells us the id left" (e.g. the meeting itself is winding down),
    // the roster check is the fast path for the common case.
    struct TalkbackPendingInvite {
        std::basic_string<zchar_t> channel_id_z;
        unsigned int user_id = 0;
        std::string name;   // for reporting only -- never compared against
                             // the SDK, same rule as every name in this file
        std::chrono::steady_clock::time_point deadline;
    };
    std::vector<TalkbackPendingInvite> m_nomination_pending_invites;

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

    // LIVE GATE RUN 1 (2026-08-26): the not-before deadline for the ladder's
    // NEXT CreateChannel, and whether one is armed at all. See
    // nomination_schedule_create()'s comment for why the ladder is paced and
    // why this is deliberately NOT an arbiter claim.
    //
    // Guarded by m_chan_mtx, NOT atomic like m_nomination_create_deadline --
    // and that is the point, not an inconsistency: nominate()'s refusal gate
    // has to read this in the SAME lock scope as the arbiter check, or a
    // re-nomination could slip between "no create outstanding" and "no create
    // scheduled" and start a second ladder over the top of a running one.
    // The pair is written and read together everywhere for that reason;
    // treat them as one field with two halves.
    //
    // Writers, all command-loop thread: nomination_schedule_create() (arms),
    // nomination_tick() (disarms as it issues), nomination_create_next()
    // (disarms on entry -- it IS the scheduled create, however it was
    // reached), nomination_abort_ladder() and nomination_reset() (disarm, so
    // a dead ladder's scheduled create can never fire into a live one).
    bool m_nomination_create_scheduled = false;
    std::chrono::steady_clock::time_point m_nomination_next_create_at{};

    // ── LAW 2 (2026-08-29): the ONE membership-call not-before ──────────────
    // Zoom rate-limits membership CALLS, so creates and invites share this
    // deadline: whichever kind goes out stamps it, and neither kind may leave
    // until it has passed. m_nomination_next_create_at above did not become
    // redundant -- it is the create's OWN schedule (is a create even wanted
    // yet, and after a code-18 backoff that can be far longer than the shared
    // spacing), while this is the floor under every membership call whatever
    // its kind. Both must be clear before a create is issued.
    //
    // Guarded by m_chan_mtx for the same reason its create-side sibling is:
    // the pump reads it in the same lock scope as the queue and the schedule,
    // and a decision assembled from three separately-locked reads is three
    // instants pretending to be one.
    std::chrono::steady_clock::time_point m_membership_next_at{};

    // An invite this file has decided on but not yet issued -- see
    // enqueue_invite()/membership_pump_invite(). Guarded by m_chan_mtx like
    // every other membership table here.
    //
    // A QUEUED INVITE MUST NEVER OUTLIVE ITS CHANNEL, the same rule
    // m_nomination_pending_invites and m_session_channels are both cleared
    // under: nomination_destroy_provisioned() empties this in the identical
    // lock scope that empties the provisioned table, so "queued for a channel
    // Zoom no longer has" is not a state that exists rather than one argued to
    // be unreachable.
    struct TalkbackQueuedInvite {
        std::basic_string<zchar_t> channel_id_z;
        std::string channel_id;   // UTF-8, reporting only
        std::string name;         // resolved to a live id at ISSUE time, never here
        // Not-before for THIS entry, distinct from the pacer's own floor: it
        // is how a code-18 refusal backs itself off (kInviteRateLimitBackoff
        // doubling) without holding up every other channel's invites, which a
        // single shared deadline would.
        std::chrono::steady_clock::time_point not_before;
        uint32_t retries = 0;
    };
    std::vector<TalkbackQueuedInvite> m_invite_queue;

    // The channel the last issued invite went to, so the pump can round-robin
    // away from it. Guarded by m_chan_mtx. Not a fairness nicety: with one
    // membership call per 600ms, a 13-channel plan's FIFO order decides
    // whether the last talent's own private channel is confirmed at second 2
    // or at second 20 -- and it is the private channels the director keys.
    std::basic_string<zchar_t> m_last_invite_channel;

    // ── LAW 1 (2026-08-29): this client's own meeting audio ─────────────────
    // Command-loop thread only, like the audio path itself -- session_start(),
    // session_stop() and mic_tick() are the only writers and all three run
    // there. Not under m_chan_mtx: none of this is channel-id state, and
    // nothing off the command loop reads it.
    //
    // m_mic_open is the LAST OBSERVED answer to "can anything this key sends
    // actually be heard", and it is what the session report's "mic" field
    // carries. It has exactly ONE reader, and that reader is what makes the
    // sentence true rather than aspirational: mic_tick() compares it against
    // the fresh answer and re-emits report_session_state() only on the EDGE
    // (review round 1, M1b -- before that this field was written in six places
    // and read in none, while this comment already described the code above).
    // m_mic_restore_pending is set only when THIS file took the client from
    // muted to unmuted during the current key -- see restore_mic_state() for
    // the semantic and why there is no re-key exception.
    bool m_mic_open = false;
    bool m_mic_restore_pending = false;
    std::chrono::steady_clock::time_point m_mic_next_assert_at{};

    // Consecutive SDKERR_TOO_FREQUENT_CALL retries for the channel currently
    // at the front of m_nomination_pending. Reset to 0 whenever that front
    // advances (onCreateChannelResponse's successful pop), so the budget is
    // PER CHANNEL rather than per ladder -- a plan that trips the rate limit
    // once on channel 2 should not arrive at channel 12 with nothing left.
    // Bounded by kMaxNominationCreateRetries; exhausting it is terminal.
    uint32_t m_nomination_create_retries = 0;

    // Final-review C1 (CRITICAL): the attempt id of the ladder currently
    // running -- claimed by nominate() at the same moment it claims the
    // arbiter, and echoed by every TERMINAL report that ladder can still
    // produce after nominate() has returned (nomination_abort_ladder()'s
    // report from any of its five call sites, and "nominate_done" from
    // onCreateChannelResponse). nominate()'s own early refusals do NOT read
    // this: they belong to the attempt being refused, not to the ladder still
    // running, and reporting the running ladder's id for a refusal is exactly
    // the confusion the id exists to remove.
    //
    // Command-loop thread only, like the ladder itself -- not under
    // m_chan_mtx, because it is not channel-id state and nothing off that
    // thread reads it.
    uint32_t m_nomination_attempt = 0;
};
