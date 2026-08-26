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

// What the current, not-yet-confirmed nominate attempt has reported so far.
// Discarded (never read again) once the attempt either commits or is
// refused -- see talkback_nomination_commit()/_note_refused() below.
struct TalkbackNominationPending {
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
inline void talkback_nomination_begin(TalkbackNominationPending &pending,
                                      const std::vector<std::string> &requested)
{
    pending = TalkbackNominationPending{};
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

// F2: the confirmed plan describes a real channel set that a Leave or an
// engine restart just destroyed. Reset to "nothing has ever been confirmed"
// -- the same state a brand new meeting starts in, which
// talkback_target_known_unprovisioned() (src/talkback-plan.h) already
// treats as "refuse every target".
inline void talkback_nomination_reset(TalkbackNominationPlan &confirmed)
{
    confirmed = TalkbackNominationPlan{};
}
