#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include "zoom-source.h"
#include "zoom-engine-client.h"
#include "zoom-reconnect.h"
#include "zoom-settings.h"
#include "zoom-settings-dialog.h"
#include "zoom-output-dialog.h"
#include "zoom-dock.h"
#include "zoom-control-server.h"
#include "zoom-osc-server.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QDockWidget>
#include <QMainWindow>
#include <QPointer>
#if defined(WIN32)
#include <windows.h>
extern "C" IMAGE_DOS_HEADER __ImageBase;
#endif
#if !defined(WIN32)
#include <csignal>
#endif

static QPointer<ZoomDock> g_dock;

static void configure_qt_plugin_paths()
{
#if defined(WIN32)
    wchar_t module_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase),
                            module_path, MAX_PATH)) {
        return;
    }

    const QFileInfo plugin_info(QString::fromWCharArray(module_path));
    const QDir plugin_dir = plugin_info.dir();
    const QStringList candidates = {
        plugin_dir.absoluteFilePath("plugins"),
        plugin_dir.absoluteFilePath("qt/plugins"),
        plugin_dir.absoluteFilePath("../plugins"),
    };

    for (const QString &path : candidates) {
        if (QFileInfo::exists(path))
            QCoreApplication::addLibraryPath(path);
    }

    blog(LOG_INFO, "[obs-zoom-plugin] Qt library paths: %s",
         QCoreApplication::libraryPaths().join(";").toUtf8().constData());
#else
    (void)QCoreApplication::libraryPaths();
#endif
}

static ZoomDock *ensure_zoom_dock()
{
    auto *main_win = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (!main_win) {
        blog(LOG_WARNING, "[obs-zoom-plugin] obs_frontend_get_main_window() returned null — dock not created");
        return nullptr;
    }

    if (!g_dock) {
        g_dock = new ZoomDock(main_win);
        obs_frontend_add_dock_by_id("ZoomControlDock", "Zoom Control", g_dock);
    }
    return g_dock;
}

static void show_zoom_dock()
{
    ZoomDock *dock_widget = ensure_zoom_dock();
    if (!dock_widget) return;

    QWidget *parent = dock_widget;
    while (parent && !qobject_cast<QDockWidget *>(parent))
        parent = parent->parentWidget();

    if (auto *dock = qobject_cast<QDockWidget *>(parent)) {
        dock->show();
        dock->raise();
        dock->activateWindow();
        dock_widget->setFocus();
        return;
    }

    dock_widget->show();
    dock_widget->raise();
    dock_widget->activateWindow();
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-zoom-plugin", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "OBS Zoom Plugin — stream and record Zoom meetings directly from OBS";
}

bool obs_module_load(void)
{
    blog(LOG_INFO, "[obs-zoom-plugin] Loading plugin v%s", OBS_ZOOM_PLUGIN_VERSION);
    configure_qt_plugin_paths();

#if !defined(WIN32)
    // Writing to a closed pipe (engine crashed) raises SIGPIPE on POSIX,
    // which would kill the host OBS process. Ignore it so the write returns
    // EPIPE instead and we can route the failure through normal error paths.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    zoom_source_register();

    ZoomPluginSettings s = ZoomPluginSettings::load();
    ZoomReconnectManager::instance().set_policy(s.reconnect_policy);
    ZoomControlServer::instance().set_token(s.control_token);
    if (!ZoomControlServer::instance().start(s.control_server_port))
        blog(LOG_WARNING, "[obs-zoom-plugin] TCP control server unavailable — continuing without it");
    if (!ZoomOscServer::instance().start(s.osc_server_port))
        blog(LOG_WARNING, "[obs-zoom-plugin] OSC server unavailable — continuing without it");

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

    obs_frontend_add_tools_menu_item("Zoom Control", [](void *) {
        show_zoom_dock();
    }, nullptr);

    obs_frontend_add_event_callback(
        [](enum obs_frontend_event event, void *) {
            if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
                ensure_zoom_dock();
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
