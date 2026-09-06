#pragma once
#include <string>

// Actual participant-video errors emitted by the Mac engine. Both belong
// to source recovery; neither is a join/authentication failure.
inline bool zoom_source_video_failure(const std::string &message)
{
    return message == "video_subscribe_failed" || message == "raw_data_controller_unavailable";
}

inline bool zoom_persistent_source_media_failure(const std::string &message)
{
    return message == "shm_create_failed" || message == "subscribe_rejected" ||
           message == "shm_name_collision";
}

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
