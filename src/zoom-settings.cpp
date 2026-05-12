#include "zoom-settings.h"
#include <obs-frontend-api.h>
#include <util/config-file.h>
static constexpr const char *SECTION = "ZoomPlugin";
ZoomPluginSettings ZoomPluginSettings::load()
{
    config_t *cfg = obs_frontend_get_global_config();
    ZoomPluginSettings s;
    s.sdk_key             = config_get_string(cfg, SECTION, "SdkKey")           ?: "";
    s.sdk_secret          = config_get_string(cfg, SECTION, "SdkSecret")        ?: "";
    s.jwt_token           = config_get_string(cfg, SECTION, "JwtToken")         ?: "";
    s.control_server_port = static_cast<uint16_t>(
        config_get_uint(cfg, SECTION, "ControlServerPort"));
    if (s.control_server_port == 0) s.control_server_port = 19870;
    s.osc_server_port = static_cast<uint16_t>(
        config_get_uint(cfg, SECTION, "OscServerPort"));
    if (s.osc_server_port == 0) s.osc_server_port = 19871;
    s.control_token = config_get_string(cfg, SECTION, "ControlToken") ?: "";
    return s;
}
void ZoomPluginSettings::save() const
{
    config_t *cfg = obs_frontend_get_global_config();
    config_set_string(cfg, SECTION, "SdkKey",             sdk_key.c_str());
    config_set_string(cfg, SECTION, "SdkSecret",          sdk_secret.c_str());
    config_set_string(cfg, SECTION, "JwtToken",           jwt_token.c_str());
    config_set_uint  (cfg, SECTION, "ControlServerPort",  control_server_port);
    config_set_uint  (cfg, SECTION, "OscServerPort",      osc_server_port);
    config_set_string(cfg, SECTION, "ControlToken",       control_token.c_str());
    config_save_safe(cfg, "tmp", nullptr);
}
