#pragma once
#include <string>

// The callback boundary keeps raw-media failures out of meeting/reconnect
// effects; callers retain their existing meeting-error classification.
template<class MediaFailure, class MeetingFailure>
inline void dispatch_zoom_engine_failure(const std::string &command,
                                         const std::string &message,
                                         bool privilege_requested,
                                         MediaFailure media_failure,
                                         MeetingFailure meeting_failure)
{
    if (command == "error" && message == "raw_media_start_failed")
        media_failure(privilege_requested);
    else
        meeting_failure();
}
