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

// What kind of session is being joined. Used by the engine to call the
// correct Zoom SDK join API (Webinar uses a different SDK entry point).
enum class MeetingKind { Meeting = 0, Webinar = 1 };

struct ZoomJoinAuthTokens {
    std::string on_behalf_token;
    std::string user_zak;
    std::string app_privilege_token;
};

// How a ZoomSource selects which participant's feed to render.
//   Participant   - fixed participant ID
//   ActiveSpeaker - whoever is currently speaking (legacy "active speaker" mode)
//   SpotlightIndex- spotlight slot N (1-based: ZoomISO-style "Spotlight 1/2/3")
//   ScreenShare   - the active screen-share feed
enum class AssignmentMode {
    Participant    = 0,
    ActiveSpeaker  = 1,
    SpotlightIndex = 2,
    ScreenShare    = 3,
};

struct ParticipantInfo {
    uint32_t    user_id = 0;
    std::string display_name;
    bool        has_video = false;
    bool        is_talking = false;
    bool        is_muted = false;
    bool        is_host = false;
    bool        is_co_host = false;
    bool        raised_hand = false;
    // 0 = not spotlighted; 1..N = 1-based spotlight slot index.
    uint32_t    spotlight_index = 0;
    // True if this participant is currently sharing their screen.
    bool        is_sharing_screen = false;
};

using ZoomPreviewCallback = std::function<void(uint32_t w, uint32_t h,
    const uint8_t *y, const uint8_t *u, const uint8_t *v,
    uint32_t stride_y, uint32_t stride_uv)>;
