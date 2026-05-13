#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include "zoom-source.h"
#include "zoom-engine-client.h"
#include "zoom-settings.h"
#include "zoom-settings-dialog.h"
#include "zoom-output-dialog.h"
#include "zoom-dock.h"
#include "zoom-control-server.h"
#include "zoom-osc-server.h"
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

    ZoomPluginSettings s = ZoomPluginSettings::load();
    ZoomControlServer::instance().set_token(s.control_token);
    ZoomControlServer::instance().start(s.control_server_port);
    ZoomOscServer::instance().start(s.osc_server_port);

    obs_frontend_add_tools_menu_item("Zoom Plugin Settings", [](void *) {
        auto *main_win = static_cast<QMainWindow *>(obs_frontend_get_main_window());
        ZoomSettingsDialog dlg(main_win);
        dlg.exec();
    }, nullptr);

    obs_frontend_add_tools_menu_item("Zoom Output Manager", [](void *) {
        auto *main_win = static_cast<QMainWindow *>(obs_frontend_get_main_window());
        ZoomOutputDialog dlg(main_win);
        dlg.exec();
    }, nullptr);

    obs_frontend_add_event_callback(
        [](enum obs_frontend_event event, void *) {
            if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
                auto *main_win = static_cast<QMainWindow *>(obs_frontend_get_main_window());
                auto *dock = new ZoomDock(main_win);
                obs_frontend_add_dock(dock);
            }
            if (event == OBS_FRONTEND_EVENT_EXIT)
                ZoomEngineClient::instance().stop();
        }, nullptr);

    blog(LOG_INFO, "[obs-zoom-plugin] Plugin loaded successfully");
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[obs-zoom-plugin] Unloading plugin");
    ZoomControlServer::instance().stop();
    ZoomOscServer::instance().stop();
    ZoomEngineClient::instance().stop();
}
