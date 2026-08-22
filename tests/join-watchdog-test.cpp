// tests/join-watchdog-test.cpp
// When the join watchdog may cancel a join, and when it must stand down.
//
// The defect this guards (2026-08-22, live meeting): the plugin sat in a Zoom
// waiting room for 114s, the watchdog counted that as "no join progress",
// auto-left at 120s and marked the attempt Failed. The host admitted the retry
// 49s later. Joining early and waiting is the normal broadcast workflow, so the
// watchdog was most likely to fire when nothing was wrong.
//
// Both directions matter and both are pinned here: a legitimate wait must never
// be cancelled, and a genuine wedge must still be caught once the wait ends.
#include "join-watchdog.h"

#include <iostream>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

static constexpr uint64_t T = kJoinWatchdogTimeoutMs; // 120000

int main()
{
    // --- Not joining, or nothing being timed: never acts ---
    check(join_watchdog_action(false, false, 1000, 1000 + 10 * T, false, false, T) ==
              JoinWatchdogAction::None,
          "the watchdog acted while not in the Joining state");
    check(join_watchdog_action(true, false, 0, 10 * T, false, false, T) ==
              JoinWatchdogAction::None,
          "the watchdog acted with no join being timed (join_started_ms == 0)");

    // --- Inside the window: nothing happens ---
    check(join_watchdog_action(true, false, 1000, 1000 + T - 1, false, false, T) ==
              JoinWatchdogAction::None,
          "the watchdog fired one millisecond before the deadline");
    check(join_watchdog_action(true, false, 1000, 1000 + T, false, false, T) ==
              JoinWatchdogAction::None,
          "the watchdog fired exactly at the deadline -- the window is a "
          "strict '> timeout', matching the original condition");

    // --- A genuine wedge past the window: fires ---
    check(join_watchdog_action(true, false, 1000, 1000 + T + 1, false, false, T) ==
              JoinWatchdogAction::Fire,
          "a genuinely stalled join past the deadline was not cancelled -- the "
          "dock would sit on 'Joining' forever, which is why this watchdog "
          "exists at all");

    // --- Fires only once ---
    check(join_watchdog_action(true, true, 1000, 1000 + 10 * T, false, false, T) ==
              JoinWatchdogAction::None,
          "the watchdog fired again after already reporting -- the operator "
          "would get a repeating dialog");

    // --- THE REGRESSION: a waiting room is not a stalled join ---
    check(join_watchdog_action(true, false, 1000, 1000 + T + 1, false, true, T) ==
              JoinWatchdogAction::HoldWindow,
          "sitting in a waiting room past the deadline was treated as a "
          "stalled join -- this is the live defect: auto-leaving a meeting "
          "the host simply had not admitted yet");
    check(join_watchdog_action(true, false, 1000, 1000 + 100 * T, false, true, T) ==
              JoinWatchdogAction::HoldWindow,
          "a long wait for admission eventually got cancelled -- a host taking "
          "their time has no upper bound, so no timeout is long enough and the "
          "window must be held, not extended");

    // --- Holding beats reporting: a wait already flagged still holds ---
    check(join_watchdog_action(true, true, 1000, 1000 + 10 * T, false, true, T) ==
              JoinWatchdogAction::HoldWindow,
          "an admission wait did not hold the window once already_reported was "
          "set; the window must reopen so the post-wait phase is timed afresh");

    // --- The pre-existing suppressor still works, and composes ---
    check(join_watchdog_action(true, false, 1000, 1000 + T + 1, true, false, T) ==
              JoinWatchdogAction::HoldWindow,
          "a pending SDK init retry no longer holds the window -- the 2026-08 "
          "orphaned-engine case would be charged against the join deadline");
    check(join_watchdog_action(true, false, 1000, 1000 + T + 1, true, true, T) ==
              JoinWatchdogAction::HoldWindow,
          "both suppressors active did not hold the window");

    // --- Once the wait ends, the watchdog is armed again ---
    // The whole point of holding rather than disabling: a wedge that happens
    // AFTER admission must still be caught.
    {
        const uint64_t admitted_at = 500000; // window restarted on admission
        check(join_watchdog_action(true, false, admitted_at,
                                   admitted_at + T - 1, false, false, T) ==
                  JoinWatchdogAction::None,
              "the freshly restarted window did not give the post-admission "
              "phase its own full two minutes");
        check(join_watchdog_action(true, false, admitted_at,
                                   admitted_at + T + 1, false, false, T) ==
                  JoinWatchdogAction::Fire,
              "a join that wedged AFTER leaving the waiting room was not "
              "caught -- suppressing the wait must not disarm the watchdog");
    }

    // --- Clock going backwards must not cancel a healthy join ---
    check(join_watchdog_action(true, false, 5000, 1000, false, false, T) ==
              JoinWatchdogAction::None,
          "a backwards clock underflowed into a huge elapsed time and "
          "cancelled a join that had barely started");
    check(join_watchdog_action(true, false, 1000, 1000, false, false, T) ==
              JoinWatchdogAction::None,
          "a zero elapsed time was treated as past the deadline");

    if (failures == 0)
        std::cout << "join-watchdog: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
