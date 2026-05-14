#pragma once
#include "hw-video-pipeline.h"
#include <cstdint>
#include <string>

struct ZoomPluginSettings {
    std::string sdk_key, sdk_secret, jwt_token;
    uint16_t    control_server_port  = 19870;
    uint16_t    osc_server_port      = 19871;
    std::string control_token;
    HwAccelMode hw_accel_mode        = HwAccelMode::None;
    static ZoomPluginSettings load();
    void save() const;
};
