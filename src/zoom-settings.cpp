#include "zoom-settings.h"
#include <obs-frontend-api.h>
#include <util/config-file.h>
static constexpr const char *SECTION = "ZoomPlugin";
ZoomPluginSettings ZoomPluginSettings::load()
{
    config_t *cfg = obs_frontend_get_global_config();
    ZoomPluginSettings s;
    s.sdk_key    = config_get_string(cfg, SECTION, "SdkKey")    ?: "";
    s.sdk_secret = config_get_string(cfg, SECTION, "SdkSecret") ?: "";
    s.jwt_token  = config_get_string(cfg, SECTION, "JwtToken")  ?: "";
    return s;
}
void ZoomPluginSettings::save() const
{
    config_t *cfg = obs_frontend_get_global_config();
    config_set_string(cfg, SECTION, "SdkKey",    sdk_key.c_str());
    config_set_string(cfg, SECTION, "SdkSecret", sdk_secret.c_str());
    config_set_string(cfg, SECTION, "JwtToken",  jwt_token.c_str());
    config_save_safe(cfg, "tmp", nullptr);
}
