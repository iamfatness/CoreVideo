#pragma once

// When the join watchdog is allowed to cancel a join attempt.
//
// Extracted from src/zoom-dock.cpp so the decision can be tested without Qt or
// libobs, the same treatment director-handover.h gets and for the same reason:
// both failure directions are invisible until they happen on a live show.
//
// The watchdog exists because a join can wedge inside the SDK with no error and
// no state change, leaving the dock stuck on "Joining" forever. After two
// minutes it leaves, marks the attempt Failed, and tells the operator to retry.
//
// THE DEFECT THIS EXISTS FOR (2026-08-22, live meeting). Not every long join is
// a wedged one. Two states are open-ended waits that are entirely outside our
// control and are progressing normally:
//
//   * MEETING_STATUS_IN_WAITING_ROOM  -- the host has not admitted us yet
//   * MEETING_STATUS_WAITINGFORHOST   -- the meeting has not started yet
//
// Neither produces any further status change until a human acts, so to the
// watchdog they were indistinguishable from a hang. Measured live: the plugin
// sat in a waiting room for 114 s, the watchdog fired at 120 s and auto-left,
// and the host admitted the retry 49 s later. For a broadcast tool this is the
// wrong way round -- the normal workflow is to join early and wait, so the
// watchdog was most likely to fire precisely when everything was fine, minutes
// before a show. (zoom-dock.cpp's own comment had anticipated this case; only
// the SDK-init-retry half of it was ever implemented.)
//
// Suppressing rather than extending is deliberate. There is no timeout long
// enough to be both "safely longer than a host taking their time" and "short
// enough to catch a wedge", because the first has no upper bound. So while a
// legitimate wait is in progress the window is held open, and the moment it
// ends the full two minutes are available again to catch a genuine wedge in
// the phase that follows.

#include <cstdint>

// Two minutes of no progress before a join is treated as wedged.
constexpr uint64_t kJoinWatchdogTimeoutMs = 120000;

enum class JoinWatchdogAction {
    // Nothing to do: not joining, already handled, or still inside the window.
    None,
    // A legitimate open-ended wait is in progress (waiting room / waiting for
    // host / SDK init retry). Hold the window open by restarting it at now;
    // do NOT cancel the join.
    HoldWindow,
    // Genuinely stalled. Cancel the attempt.
    Fire,
};

// `join_started_ms` is 0 when no join is being timed.
//
// HoldWindow takes precedence over Fire, and is returned regardless of how much
// time has elapsed: a wait that has already outlasted the window must still be
// held, or the watchdog would fire the instant it was consulted.
inline JoinWatchdogAction join_watchdog_action(bool     joining,
                                               bool     already_reported,
                                               uint64_t join_started_ms,
                                               uint64_t now_ms,
                                               bool     init_retry_pending,
                                               bool     awaiting_admission,
                                               uint64_t timeout_ms)
{
    if (!joining || join_started_ms == 0)
        return JoinWatchdogAction::None;

    if (init_retry_pending || awaiting_admission)
        return JoinWatchdogAction::HoldWindow;

    if (already_reported)
        return JoinWatchdogAction::None;

    // Guard the subtraction: a clock that went backwards must not underflow
    // into an enormous elapsed time and cancel a healthy join.
    if (now_ms <= join_started_ms)
        return JoinWatchdogAction::None;

    return (now_ms - join_started_ms) > timeout_ms ? JoinWatchdogAction::Fire
                                                   : JoinWatchdogAction::None;
}
