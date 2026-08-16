#pragma once
#include "hw-video-pipeline.h"
#include "zoom-reconnect.h"
#include <cstdint>
#include <string>

struct ZoomPluginSettings {
    std::string         sdk_key, sdk_secret, jwt_token;
    std::string         sdk_public_app_key;
    std::string         meeting_sdk_auth_mode = "public_app_key";
    // OAuth client ID baked in at build time. global.ini can override this
    // only in development builds where the embedded value is blank.
    std::string         oauth_client_id;
    // Optional global.ini override for the Zoom authorization URL (dev/staging).
    std::string         oauth_authorization_url;
    std::string         oauth_redirect_uri = "corevideo://oauth/callback";
    std::string         oauth_scopes = "user:read:zak user:read:user user:read:token";
    std::string         oauth_access_token;
    std::string         oauth_refresh_token;
    int64_t             oauth_expires_at = 0;
    uint16_t            control_server_port  = 19870;
    uint16_t            osc_server_port      = 19871;
    std::string         control_token;
    HwAccelMode         hw_accel_mode        = HwAccelMode::None;
    ZoomReconnectPolicy reconnect_policy;

    // Once-per-session GitHub Releases check for a newer CoreVideo build.
    // Plain unauthenticated GET, no telemetry - see docs/policies/privacy-policy.md.
    bool                check_for_updates_on_startup = true;

    // Last successful join, used to repopulate the dock on next launch.
    std::string         last_meeting_id;
    std::string         last_display_name;
    bool                last_was_webinar     = false;

    // Hide camera-off participants from VIDEO-assignment pickers. Audio
    // pickers ignore this -- see src/participant-filter.h for why.
    bool                hide_participants_without_video = false;

    // ISO recorder panel defaults.
    std::string         iso_output_dir;
    std::string         iso_ffmpeg_path = "ffmpeg";
    std::string         iso_video_encoder = "auto";
    bool                iso_record_program = true;

    // Active speaker director defaults.
    uint32_t            speaker_sensitivity_ms = 500;
    uint32_t            speaker_hold_ms = 2000;
    uint32_t            speaker_exclude_participant_1 = 0;
    uint32_t            speaker_exclude_participant_2 = 0;
    bool                speaker_require_video = true;

    static ZoomPluginSettings load();
    std::string resolved_meeting_sdk_public_app_key() const;
    std::string resolved_jwt_token() const;
    bool use_broker_sdk_jwt() const;
    void save() const;
};
