#pragma once
#include "hw-video-pipeline.h"
#include "zoom-reconnect.h"
#include <cstdint>
#include <string>

struct ZoomPluginSettings {
    std::string         sdk_key, sdk_secret, jwt_token;
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
    void save() const;
};
