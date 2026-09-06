#pragma once

#include <string>

// ZoomSDKMeetingStatus must be declared by <ZoomSDK/ZoomSDKErrors.h> before
// this header is included. Keeping the SDK symbols here makes this the single
// production translation seam exercised by the macOS regression test.
inline std::string
macos_awaiting_admission_event(ZoomSDKMeetingStatus status)
{
    const bool awaiting = status == ZoomSDKMeetingStatus_WaitingForHost ||
        status == ZoomSDKMeetingStatus_InWaitingRoom;
    return std::string(R"({"cmd":"awaiting_admission","active":)") +
        (awaiting ? "true" : "false") + "}";
}
