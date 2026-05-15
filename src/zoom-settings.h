#pragma once
#include "hw-video-pipeline.h"
#include "zoom-reconnect.h"
#include <cstdint>
#include <string>

struct ZoomPluginSettings {
    std::string         sdk_key, sdk_secret, jwt_token;
    std::string         oauth_client_id;
    std::string         oauth_client_secret;
    std::string         oauth_authorization_url;
    std::string         oauth_redirect_uri = "corevideo://oauth/callback";
    std::string         oauth_scopes = "user:read:zak user:read:user";
    std::string         oauth_access_token;
    std::string         oauth_refresh_token;
    int64_t             oauth_expires_at = 0;
    uint16_t            control_server_port  = 19870;
    uint16_t            osc_server_port      = 19871;
    std::string         control_token;
    HwAccelMode         hw_accel_mode        = HwAccelMode::None;
    ZoomReconnectPolicy reconnect_policy;

    // Last successful join, used to repopulate the dock on next launch.
    std::string         last_meeting_id;
    std::string         last_display_name;
    bool                last_was_webinar     = false;

    static ZoomPluginSettings load();
    std::string resolved_jwt_token() const;
    void save() const;
};
