// Pins the real macOS SDK meeting-status translation that feeds the join
// watchdog. This is Objective-C++ so the symbolic ZoomSDK enum values are
// compiled from the installed framework rather than duplicated as integers.
#import <ZoomSDK/ZoomSDKErrors.h>

#include "macos-admission-state.h"
#include "join-watchdog.h"

#include <iostream>
#include <string>
#include <vector>

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
    std::vector<std::string> delivered;
    const auto deliver_status = [&](ZoomSDKMeetingStatus status) {
        delivered.clear();
        macos_dispatch_meeting_status(
            status,
            [&](const std::string &event) {
                delivered.push_back(event);
                awaiting_admission =
                    event.find("\"active\":true") != std::string::npos;
            },
            [&] {
                if (status == ZoomSDKMeetingStatus_InMeeting)
                    delivered.emplace_back("joined-handler");
                else if (status == ZoomSDKMeetingStatus_Ended ||
                         status == ZoomSDKMeetingStatus_Failed ||
                         status == ZoomSDKMeetingStatus_Disconnecting)
                    delivered.emplace_back("terminal-handler");
                else
                    delivered.emplace_back("status-handler");
            });
    };

    deliver_status(ZoomSDKMeetingStatus_WaitingForHost);
    check(delivered.size() == 2 && delivered[0] ==
              R"({"cmd":"awaiting_admission","active":true})" &&
              delivered[1] == "status-handler",
          "WaitingForHost did not produce the production admission-wait event");
    check(join_watchdog_action(true, false, 1000, 181000, false,
                               awaiting_admission,
                               kJoinWatchdogTimeoutMs) ==
              JoinWatchdogAction::HoldWindow,
          "waiting for the host at 180 seconds did not hold the watchdog");

    deliver_status(ZoomSDKMeetingStatus_InWaitingRoom);
    check(delivered.size() == 2 && delivered[0] ==
              R"({"cmd":"awaiting_admission","active":true})" &&
              delivered[1] == "status-handler",
          "InWaitingRoom did not produce the production admission-wait event");

    const uint64_t admitted_at = 200000;
    deliver_status(ZoomSDKMeetingStatus_Connecting);
    check(delivered.size() == 2 && delivered[0] ==
              R"({"cmd":"awaiting_admission","active":false})" &&
              delivered[1] == "status-handler",
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
    check(delivered.size() == 2 && delivered[0] ==
              R"({"cmd":"awaiting_admission","active":false})" &&
              delivered[1] == "terminal-handler",
          "terminal handling ran before the admission state was cleared");
    deliver_status(ZoomSDKMeetingStatus_WaitingForHost);
    deliver_status(ZoomSDKMeetingStatus_InMeeting);
    check(!awaiting_admission,
          "a leave/rejoin cycle retained admission state after joining");
    check(delivered.size() == 2 && delivered[0] ==
              R"({"cmd":"awaiting_admission","active":false})" &&
              delivered[1] == "joined-handler",
          "joined handling ran before the admission state was cleared");

    if (failures == 0)
        std::cout << "macos-admission-state: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
