#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include "zoom-source.h"
#include "zoom-share-delegate.h"
#include "zoom-participant-audio-source.h"
#include "zoom-auth.h"
#include "zoom-settings.h"
#include "zoom-settings-dialog.h"
#include <QMainWindow>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-zoom-plugin", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "OBS Zoom Plugin — stream and record Zoom meetings directly from OBS";
}

bool obs_module_load(void)
{
    blog(LOG_INFO, "[obs-zoom-plugin] Loading plugin v%s", OBS_ZOOM_PLUGIN_VERSION);

    zoom_source_register();
    zoom_share_source_register();
    zoom_participant_audio_source_register();

    ZoomPluginSettings s = ZoomPluginSettings::load();
    if (!s.sdk_key.empty() && !s.sdk_secret.empty()) {
        if (ZoomAuth::instance().init(s.sdk_key, s.sdk_secret) && !s.jwt_token.empty())
            ZoomAuth::instance().authenticate(s.jwt_token);
    }

    obs_frontend_add_tools_menu_item("Zoom Plugin Settings", [](void *) {
        auto *main_win = static_cast<QMainWindow *>(obs_frontend_get_main_window());
        ZoomSettingsDialog dlg(main_win);
        dlg.exec();
    }, nullptr);

    obs_frontend_add_event_callback(
        [](enum obs_frontend_event event, void *) {
            if (event == OBS_FRONTEND_EVENT_EXIT)
                ZoomAuth::instance().shutdown();
        }, nullptr);

    blog(LOG_INFO, "[obs-zoom-plugin] Plugin loaded successfully");
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[obs-zoom-plugin] Unloading plugin");
    ZoomAuth::instance().shutdown();
}
