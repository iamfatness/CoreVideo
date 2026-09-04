#pragma once
//
// talkback-nomination.h — Task 5 fix round 1: the plugin's own record of a
// nomination's outcome, kept honest about what the engine has actually
// CONFIRMED versus what was merely SENT.
//
// F1 (Major, fix round 1). talkback_nominate() used to overwrite the
// confirmed plan at SEND time, before the engine had agreed to anything. The
// engine refuses a nomination on seven paths (session_live, probe_busy,
// not_in_meeting, no_controller, not_supported, target_name_collision,
// create_busy -- engine/src/engine-talkback.cpp's nominate()) and on every
// one of them leaves the standing provisioned channel set EXACTLY as it was
// (nominate()'s own comment above the arbiter gate says so explicitly).
// Overwriting the plugin's record anyway meant a refused re-nomination --
// exactly the operator's mistake-recovery path, under pressure, mid-show --
// made key_on() refuse a target whose channel was still standing and that
// session_start() would have selected. Free of Qt/OBS/SDK so this exact
// scenario can be pinned by a test with no engine and no meeting (see
// tests/talkback-nomination-test.cpp), the same reason src/talkback-plan.h
// is header-only.
//
// The fix: TalkbackNominationPlan (the CONFIRMED record --
// ZoomEngineClient::TalkbackNominationStatus is this type) is written ONLY
// by talkback_nomination_commit(), which fires on the engine's own
// "nominate_done" for an attempt that was never refused. A refusal touches
// only last_attempt_ok/last_attempt_reason -- diagnostic fields that
// talkback_target_known_unprovisioned() (src/talkback-plan.h) must never
// read. Everything reported for an in-flight attempt
// (uncovered_private/unreachable/plan stage lines, which arrive one at a
// time, well before nominate_done) is staged in TalkbackNominationPending
// first, so a partial or ultimately-refused attempt can never leak into
// what key_on() trusts as provisioned.
//
// F2 (Major, fix round 1). The confirmed plan also has to become false once
// the thing it describes stops being true -- a Leave (engine's
// nomination_reset(), engine/src/main.cpp) or an engine restart both wipe
// the real channel set to nothing, but nothing was clearing the plugin's
// side of it, so `talkback_status` kept advertising a plan that no longer
// existed and key_on()'s pre-check kept passing -- reopening the exact
// open-then-retract flicker this whole pre-check exists to close.
// talkback_nomination_reset() is that world-reset, wired into
// ZoomEngineClient at the same two points that already reset other
// per-meeting/per-process state (the "left" event handler and start()'s
// fresh-launch path) -- not a new hook.
#include <cstdint>
#include <string>
#include <vector>

// C1 (CRITICAL, final whole-branch review 2026-08-26). The staging slot had
// no ATTEMPT IDENTITY, and talkback_nominate() resets it at SEND time. A
// second nominate sent while the first ladder was still provisioning
// therefore wiped the first attempt's staging, and the first ladder's
// eventual "nominate_done" committed the SECOND attempt's nominee list
// against the FIRST ladder's channels: a standing channel refused locally
// ("Sarah has no talkback channel" while Sarah's channel was up), an
// unprovisioned name passing the pre-check and opening then retracting, and
// the first attempt's uncovered_private/unreachable shortfalls vanishing out
// of the polled talkback_status field. The interleaving is not exotic -- the
// engine refuses the second attempt with "create_busy" and leaves the first
// ladder running, which is its documented, correct behaviour.
//
// The fix is an explicit id, not an ordering argument: the plugin stamps a
// monotonically increasing "attempt" into every talkback_nominate request,
// the engine echoes it in every TERMINAL report for that attempt, and
// talkback_nomination_apply_report() (src/talkback-nomination-dispatch.h)
// acts only on reports whose attempt matches the one staged here. A FIFO of
// staged attempts was considered and rejected: this milestone has already
// shipped one Critical out of an un-popped queue, and an id makes the match
// structural rather than positional.
//
// What the current, not-yet-confirmed nominate attempt has reported so far.
// Discarded (never read again) once the attempt either commits or is
// refused -- see talkback_nomination_commit()/_note_refused() below.
struct TalkbackNominationPending {
    // Which nominate attempt this slot stages. Assigned by
    // talkback_nomination_begin() from ZoomEngineClient's process-wide
    // monotonic counter; 0 means "nothing staged yet". Never reset by a
    // world-reset to a value it has used before -- see that counter's
    // comment in zoom-engine-client.h for why re-use would be the bug.
    uint32_t attempt = 0;
    std::vector<std::string> requested; // this attempt's own nominee list
    uint32_t channels = 0;
    bool all_talent_complete = true;
    std::vector<std::string> uncovered_private;
    std::vector<std::string> unreachable;
};

struct TalkbackNominationPlan {
    // True once a nomination has ever been CONFIRMED by the engine. Never
    // set by a send, and never set by a refusal.
    bool done = false;
    uint32_t channels = 0;
    bool all_talent_complete = true;
    std::vector<std::string> requested;
    std::vector<std::string> uncovered_private;
    std::vector<std::string> unreachable;

    // The most recent nominate ATTEMPT's outcome, which may be newer than --
    // and disagree with -- the confirmed fields above (e.g. a re-nomination
    // the engine refused while a key was open, or while an earlier ladder
    // was still creating channels). Purely diagnostic: key_on()'s pre-check
    // (src/talkback-controller.cpp) must only ever read the confirmed
    // fields above, never these two.
    bool last_attempt_ok = true;
    std::string last_attempt_reason;
};

// A fresh talkback_nominate() is about to be sent: start staging this
// attempt's report. Does NOT touch `confirmed` -- see the header comment
// above for why the confirmed plan must not move until (and unless) this
// attempt is actually accepted.
// `attempt` is this send's own identity (C1): the value stamped into the
// wire request, and the only value a report may carry for its terminal to be
// allowed to move `confirmed`.
inline void talkback_nomination_begin(TalkbackNominationPending &pending,
                                      const std::vector<std::string> &requested,
                                      uint32_t attempt)
{
    pending = TalkbackNominationPending{};
    pending.attempt   = attempt;
    pending.requested = requested;
}

inline void talkback_nomination_note_uncovered(TalkbackNominationPending &pending,
                                               const std::string &name)
{
    pending.uncovered_private.push_back(name);
}

inline void talkback_nomination_note_unreachable(TalkbackNominationPending &pending,
                                                 const std::string &name)
{
    pending.unreachable.push_back(name);
}

inline void talkback_nomination_note_plan(TalkbackNominationPending &pending,
                                          uint32_t channels,
                                          bool all_talent_complete)
{
    pending.channels = channels;
    pending.all_talent_complete = all_talent_complete;
}

// The engine's own "nominate_done" for THIS attempt: it was accepted, so
// promote the staged report to the confirmed plan. `final_channels` is
// nominate_done's own count, not the earlier "plan" stage's -- they can
// differ if a create failed partway through provisioning (see
// engine-talkback.cpp's nomination_create_next()).
inline void talkback_nomination_commit(TalkbackNominationPlan &confirmed,
                                       const TalkbackNominationPending &pending,
                                       uint32_t final_channels)
{
    confirmed.done                = true;
    confirmed.channels             = final_channels;
    confirmed.all_talent_complete  = pending.all_talent_complete;
    confirmed.requested            = pending.requested;
    confirmed.uncovered_private    = pending.uncovered_private;
    confirmed.unreachable          = pending.unreachable;
    confirmed.last_attempt_ok      = true;
    confirmed.last_attempt_reason.clear();
}

// The engine refused THIS attempt outright (one of the seven paths named in
// the header comment). The confirmed plan must NOT move -- only the
// diagnostic fields change, so an operator can see "your last nominate call
// failed" without that failure corrupting what key_on() trusts.
inline void talkback_nomination_note_refused(TalkbackNominationPlan &confirmed,
                                             const std::string &reason)
{
    confirmed.last_attempt_ok     = false;
    confirmed.last_attempt_reason = reason;
}

// Fix round 2 (N1, Major). A THIRD outcome the {confirmed, refused} model
// above did not represent: nominate() destroys the standing provisioned set
// BEFORE planning (its own comment: "destroying is safe HERE and nowhere
// earlier"), so if the ladder then aborts part-way
// (engine-talkback.cpp's nomination_create_next(): the arbiter is busy, or
// CreateChannel itself fails), the engine ends this attempt having already
// destroyed whatever it was replacing -- and, on the CreateChannel-failure
// branch, without ever emitting the "nominate_done" that would let
// talkback_nomination_commit() run. Routing this outcome through
// talkback_nomination_note_refused() (as round 1 did for every ok:false)
// would leave the confirmed plan describing channels that no longer exist --
// the exact F2 flicker, on a trigger neither Leave nor an engine restart
// covers, because nothing here is either of those.
//
// The engine marks this outcome with an explicit "channels_destroyed":true
// field (see report_nomination() call sites in nomination_create_next()) --
// NOT inferred from the "reason" string, because nominate()'s OWN early gate
// check reports the identical "create_busy" reason for a refusal that does
// NOT destroy anything (it runs before the replace step). Reason strings
// collide; this field does not.
//
// Unlike a plain talkback_nomination_reset() (Leave, engine restart -- no
// single attempt to blame), this keeps the reason as a diagnostic: the
// operator still needs to know WHY there is suddenly no nomination.
inline void talkback_nomination_note_failed_after_destroy(TalkbackNominationPlan &confirmed,
                                                           const std::string &reason)
{
    confirmed = TalkbackNominationPlan{};
    confirmed.last_attempt_ok     = false;
    confirmed.last_attempt_reason = reason;
}

// C1 (Critical, final review): a TERMINAL report arrived for an attempt that
// is no longer the one staged -- an earlier ladder finishing after a newer
// nominate was sent. Its staged report is gone (one slot, deliberately: see
// TalkbackNominationPending's comment), so there is nothing to commit and
// nothing this plugin can reconstruct about what that ladder actually
// provisioned.
//
// FAIL CLOSED rather than leave the old record standing. The superseded
// ladder's own nominate() destroyed whatever the confirmed plan described
// before it started building (nominate()'s replace path), so keeping that
// record would leave key_on()'s pre-check permanently trusting a channel set
// that no longer exists -- F2's exact symptom, on a trigger neither Leave nor
// an engine restart covers. Resetting means every target is refused with "no
// one has been nominated yet" until the operator re-nominates: recoverable in
// one command, and the reason string says exactly that. The reason is kept as
// a diagnostic for the same purpose talkback_nomination_note_failed_after_
// destroy() keeps its own.
inline void talkback_nomination_note_superseded(TalkbackNominationPlan &confirmed,
                                                const std::string &reason)
{
    // Deliberately delegated, not hand-copied: the OUTCOME is identical to a
    // ladder that aborted after destroying (reset to "nothing confirmed",
    // keep the reason), only the cause differs, and two hand-written copies
    // of the same three assignments is how this file's siblings have drifted
    // before.
    talkback_nomination_note_failed_after_destroy(confirmed, reason);
}

// F2: the confirmed plan describes a real channel set that a Leave or an
// engine restart just destroyed. Reset to "nothing has ever been confirmed"
// -- the same state a brand new meeting starts in, which
// talkback_target_known_unprovisioned() (src/talkback-plan.h) already
// treats as "refuse every target".
inline void talkback_nomination_reset(TalkbackNominationPlan &confirmed)
{
    confirmed = TalkbackNominationPlan{};
}

// ── Per-person channel presence (Milestone 7, the intercom-grid dock) ───────
//
// WHY THIS EXISTS. The confirmed plan above answers "does this person have a
// channel". It cannot answer the question the operator actually had on
// 2026-08-29, looking at a grid of seven talent: "is this person IN it".
// Those are different, and the difference was live that morning. From the
// engine's own nomination stage lines for that show:
//
//     "stage":"invite","name":"John Wallace",...,"code":2
//     "stage":"participant_talkback_support","name":"Grant Whitehead","supported":false
//     "stage":"invite","name":"Grant Whitehead",...,"code":3
//     "stage":"member_invited","name":"Chris Fritsche",...
//
// John Wallace had a private channel and a completed plan (nominate_done,
// 7 channels) and could hear NOTHING: every invite for him was refused
// SDKERR_WRONG_USAGE (2), because he was in a different breakout room from
// the engine and Zoom's talkback reaches only the room the inviter is in.
// Grant Whitehead's client does not support talkback at all
// (supported:false, then SDKERR_INVALID_PARAMETER (3) on the invite). Both
// looked identical to a dock that only knew the plan: a ready, pressable
// key, on a person who hears silence.
//
// WHAT THIS IS NOT. It is not authority over keying. Nothing here gates
// key_on(); the enablement rules in src/talkback-dock-state.h are unchanged
// and this only decides what a cell SAYS about a person. That is deliberate:
// this view is assembled from edges (see
// talkback_channel_presence_apply_report(), src/talkback-nomination-dispatch.h)
// and can lag or be Unknown, and refusing a key on a stale absence would be
// strictly worse than letting a press fail closed the way it already does.
//
// TWO LIMITS, both stated rather than hidden:
//   * it is per PERSON, not per channel. Someone can be in their private
//     channel and not in the all-talent one -- the 2026-08-29 log has three
//     such people, refused SDKERR_TOO_FREQUENT_CALL (18) on the all-talent
//     invite and admitted to their own channel a second later. The last
//     edge for a name wins, so those read Present, which is what their own
//     key button does.
//   * Unknown is the starting state and is rendered as "ready", NOT as
//     absent. An engine that reports none of these stages, or a plugin that
//     has simply not seen them yet, must not paint every person amber.
enum class TalkbackPersonPresence {
    // Nothing observed. Rendered as ready: this record never manufactures a
    // doubt it has no evidence for.
    Unknown,
    // Zoom confirmed them into a talkback channel (member_invited, which
    // includes TALKBACK_ERROR_ALREADY_EXIST -- see engine-talkback.cpp).
    Present,
    // An invite for them was refused, they were not in the meeting to
    // invite, or they left a channel they had been in.
    NotInChannel,
    // Their Zoom client reported no talkback support
    // (participant_talkback_support "supported":false). Strictly worse than
    // NotInChannel: nothing the director does reaches them.
    NoTalkback,
};

// The live per-person view. A small flat vector, not a map: this is at most
// the nominee list (16 channels x 10 people is the hard ceiling) and it is
// read once per person per dock tick.
struct TalkbackChannelPresence {
    struct Entry {
        std::string name;
        TalkbackPersonPresence state = TalkbackPersonPresence::Unknown;
    };
    std::vector<Entry> people;
};

inline TalkbackPersonPresence talkback_presence_for(
    const TalkbackChannelPresence &presence, const std::string &name)
{
    for (const auto &e : presence.people)
        if (e.name == name) return e.state;
    return TalkbackPersonPresence::Unknown;
}

// Records one observation. Last edge wins, with ONE exception:
// NoTalkback survives a later NotInChannel.
//
// That exception is not a preference, it is what the wire does. A client
// with no talkback support produces BOTH stages, in this order:
// participant_talkback_support "supported":false, then the invite refused
// SDKERR_INVALID_PARAMETER. Plain last-wins would throw away the specific
// diagnosis ("their Zoom cannot do this") and keep the generic consequence
// ("they are not in the channel"), telling the operator to re-assign
// channels for someone no re-assignment can reach. Only a confirmed
// Present clears it -- that is real evidence the client CAN, and it
// outranks a stale capability report.
inline void talkback_presence_note(TalkbackChannelPresence &presence,
                                   const std::string &name,
                                   TalkbackPersonPresence state)
{
    if (name.empty() || state == TalkbackPersonPresence::Unknown) return;
    for (auto &e : presence.people) {
        if (e.name != name) continue;
        if (state != TalkbackPersonPresence::Present &&
            e.state == TalkbackPersonPresence::NoTalkback)
            return;
        e.state = state;
        return;
    }
    presence.people.push_back(TalkbackChannelPresence::Entry{name, state});
}

// The world-reset, wired into exactly the points talkback_nomination_reset()
// already is (a Leave, an engine restart) plus the send of a new nominate.
// The send is included because a nomination REPLACES the standing channel
// set: every observation this record holds is about channels the engine is
// destroying, and carrying them forward would describe the new ladder with
// the old one's failures. Clearing to Unknown fails SOFT -- everyone reads
// "ready" until the new ladder's own invites report -- which is the right
// direction for a record that is display-only.
inline void talkback_presence_reset(TalkbackChannelPresence &presence)
{
    presence = TalkbackChannelPresence{};
}
