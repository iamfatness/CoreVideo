#pragma once

#include <cstdint>
#include <functional>
#include <string>

enum class MeetingState { Idle, Joining, InMeeting, Leaving, Recovering, Failed };
enum class RecoveryReason {
    EngineCrash,
    MeetingDisconnect,
    NetworkDrop,
    AuthFailure,
    SdkError,
    HostEndedMeeting,
    LicenseError,
};
enum class AudioChannelMode { Mono = 0, Stereo = 1 };
enum class VideoResolution { P360 = 0, P720 = 1, P1080 = 2 };
enum class VideoLossMode { LastFrame = 0, Black = 1 };

struct ParticipantInfo {
    uint32_t    user_id = 0;
    std::string display_name;
    bool        has_video = false;
    bool        is_talking = false;
    bool        is_muted = false;
};

using ZoomPreviewCallback = std::function<void(uint32_t w, uint32_t h,
    const uint8_t *y, const uint8_t *u, const uint8_t *v,
    uint32_t stride_y, uint32_t stride_uv)>;
