// Pins the real macOS SDK meeting-status translation that feeds the join
// watchdog. This is Objective-C++ so the symbolic ZoomSDK enum values are
// compiled from the installed framework rather than duplicated as integers.
#import <ZoomSDK/ZoomSDKErrors.h>

#include "macos-admission-state.h"
#include "join-watchdog.h"

#include <iostream>
#include <string>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

int main()
{
    bool awaiting_admission = false;
    const auto deliver_status = [&](ZoomSDKMeetingStatus status) {
        const std::string event = macos_awaiting_admission_event(status);
        awaiting_admission = event.find("\"active\":true") != std::string::npos;
        return event;
    };

    check(deliver_status(ZoomSDKMeetingStatus_WaitingForHost) ==
              R"({"cmd":"awaiting_admission","active":true})",
          "WaitingForHost did not produce the production admission-wait event");
    check(join_watchdog_action(true, false, 1000, 181000, false,
                               awaiting_admission,
                               kJoinWatchdogTimeoutMs) ==
              JoinWatchdogAction::HoldWindow,
          "waiting for the host at 180 seconds did not hold the watchdog");

    check(deliver_status(ZoomSDKMeetingStatus_InWaitingRoom) ==
              R"({"cmd":"awaiting_admission","active":true})",
          "InWaitingRoom did not produce the production admission-wait event");

    const uint64_t admitted_at = 200000;
    check(deliver_status(ZoomSDKMeetingStatus_Connecting) ==
              R"({"cmd":"awaiting_admission","active":false})",
          "leaving an admission wait did not clear the production event");
    check(join_watchdog_action(true, false, admitted_at,
                               admitted_at + kJoinWatchdogTimeoutMs, false,
                               awaiting_admission,
                               kJoinWatchdogTimeoutMs) == JoinWatchdogAction::None,
          "admission did not receive a fresh 120-second watchdog window");
    check(join_watchdog_action(true, false, admitted_at,
                               admitted_at + kJoinWatchdogTimeoutMs + 1, false,
                               awaiting_admission,
                               kJoinWatchdogTimeoutMs) == JoinWatchdogAction::Fire,
          "a genuine post-admission stall did not time out");

    deliver_status(ZoomSDKMeetingStatus_InWaitingRoom);
    deliver_status(ZoomSDKMeetingStatus_Ended);
    check(!awaiting_admission,
          "leaving a meeting retained the previous admission-wait state");
    deliver_status(ZoomSDKMeetingStatus_WaitingForHost);
    deliver_status(ZoomSDKMeetingStatus_InMeeting);
    check(!awaiting_admission,
          "a leave/rejoin cycle retained admission state after joining");

    if (failures == 0)
        std::cout << "macos-admission-state: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
