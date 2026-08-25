#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include "zoom-source.h"
#include "zoom-participant-audio-source.h"
#include "zoom-supersource.h"
#include "zoom-engine-client.h"
#include "zoom-reconnect.h"
#include "zoom-settings.h"
#include "zoom-settings-dialog.h"
#include "zoom-output-dialog.h"
#include "zoom-diagnostics-dialog.h"
#include "zoom-dock.h"
#include "zoom-iso-recorder.h"
#include "zoom-iso-panel.h"
#include "zoom-control-server.h"
#include "zoom-osc-server.h"
#include "cv-ffmpeg-loader.h"
#include "talkback-controller.h"
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
static QPointer<ZoomIsoPanel> g_iso_panel;
static QPointer<ZoomOutputDialog> g_output_panel;
static QPointer<ZoomDiagnosticsDialog> g_diagnostics_panel;
static QPointer<QDockWidget> g_zoom_dock_host;
static QPointer<QDockWidget> g_iso_dock_host;
static QPointer<QDockWidget> g_output_dock_host;
static QPointer<QDockWidget> g_diagnostics_dock_host;
static bool g_frontend_callback_registered = false;
static bool g_shutdown_started = false;

static ZoomDock *ensure_zoom_dock();
static ZoomIsoPanel *ensure_iso_panel();
static ZoomOutputDialog *ensure_output_panel();
static ZoomDiagnosticsDialog *ensure_diagnostics_panel();
static QMainWindow *obs_main_window();

static QMainWindow *obs_main_window()
{
    return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

static void shutdown_corevideo()
{
    if (g_shutdown_started)
        return;
    g_shutdown_started = true;
    blog(LOG_INFO, "[obs-zoom-plugin] Shutting down CoreVideo runtime");
    if (g_dock)
        g_dock->prepare_shutdown();
    if (g_iso_panel)
        g_iso_panel->prepare_shutdown();
    if (g_output_panel)
        g_output_panel->prepare_shutdown();
    if (g_diagnostics_panel)
        g_diagnostics_panel->prepare_shutdown();
    ZoomControlServer::instance().stop();
    ZoomOscServer::instance().stop();
    ZoomIsoRecorder::instance().stop();
    // Before the engine goes down: stops the dead-man timer and, if a key is
    // still open, releases the OBS tap (capture callback, source ref, SHM
    // region) explicitly rather than leaving it to static destruction order.
    TalkbackController::instance().stop();
    ZoomEngineClient::instance().stop();
}

static void frontend_event_callback(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        ensure_zoom_dock();
        ensure_iso_panel();
        ensure_output_panel();
        ensure_diagnostics_panel();
        // Touch the singleton so its QTimer is constructed here, on the Qt
        // main thread, rather than lazily on whatever thread first calls in.
        TalkbackController::instance();
    }
    // Gates Tiles per-participant audio reconciliation off for the duration
    // of a scene-collection teardown+load, and sweeps every Tiles source once
    // it is confirmed finished. See zoom_supersource_set_collection_loading's
    // own comment: obs_queue_task(OBS_TASK_UI, ...) is not reliably deferred
    // when called from the UI thread, which a collection load runs on, so
    // without this gate a restored Tiles source can reconcile audio against a
    // still-loading collection and create a duplicate of its own saved source.
    //
    // CLEANUP is what actually makes the gate reliable, and CHANGING alone is
    // not enough. Two load paths never fire CHANGING at all —
    // OBSBasic::SetupDuplicateSceneCollection and SetupRenameSceneCollection
    // (frontend/widgets/OBSBasic_SceneCollections.cpp:159-206 and :208-240)
    // both call ActivateSceneCollection, and therefore the full
    // ClearSceneData + obs_load_sources sequence, with no CHANGING first.
    // CLEANUP has none of that asymmetry: it is emitted from ClearSceneData
    // itself (:1544), on every path that tears a collection down, immediately
    // before the Qt event-queue pumps at :1550-1559 that can otherwise run an
    // already-posted reconcile task mid-teardown. It is also the one event
    // deliberately exempt from the disableSaving filter that drops every other
    // frontend event during a load (OBSStudioAPI.cpp:711), so it is delivered
    // even on the very first load at startup. CHANGING is kept as well: it
    // fires earlier still on the paths that do have it, and closing the gate
    // sooner is free.
    //
    // CHANGED and FINISHED_LOADING both mark "safe to reconcile now".
    // CHANGED is emitted on every ActivateSceneCollection path
    // (:789) once Load() has fully returned — including the very first load
    // at startup, where OBSBasic.cpp:1109-1111 momentarily drops disableSaving
    // specifically so it is delivered. FINISHED_LOADING is kept as a
    // belt-and-braces second clear for startup.
    if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING ||
        event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP)
        zoom_supersource_set_collection_loading(true);
    if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED ||
        event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
        zoom_supersource_set_collection_loading(false);
    if (event == OBS_FRONTEND_EVENT_EXIT)
        shutdown_corevideo();
}

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

static QDockWidget *dock_host_for(QWidget *widget)
{
    QWidget *parent = widget;
    while (parent && !qobject_cast<QDockWidget *>(parent))
        parent = parent->parentWidget();
    return qobject_cast<QDockWidget *>(parent);
}

static QDockWidget *find_dock_host_by_id(const char *dock_id)
{
    auto *main_win = obs_main_window();
    if (!main_win || !dock_id || !*dock_id)
        return nullptr;

    const QString id = QString::fromUtf8(dock_id);
    const auto docks = main_win->findChildren<QDockWidget *>();
    for (QDockWidget *dock : docks) {
        if (!dock)
            continue;
        if (dock->objectName() == id || dock->windowTitle() == id)
            return dock;
        if (dock->widget() && dock->widget()->objectName() == id)
            return dock;
    }
    return nullptr;
}

template <typename T>
static void observe_dock_widget(QWidget *widget,
                                QPointer<QDockWidget> &dock_host,
                                QPointer<T> &widget_ptr,
                                const char *dock_id)
{
    if (!widget)
        return;

    auto *dock_host_ptr = &dock_host;
    auto *widget_ptr_ptr = &widget_ptr;
    QObject::connect(widget, &QObject::destroyed, widget, [dock_id, dock_host_ptr, widget_ptr_ptr]() {
        blog(LOG_INFO, "[obs-zoom-plugin] Dock widget destroyed: %s", dock_id);
        dock_host_ptr->clear();
        widget_ptr_ptr->clear();
    });
}

static void observe_dock_host(QDockWidget *host,
                              QPointer<QDockWidget> &dock_host,
                              const char *dock_id)
{
    if (!host)
        return;

    auto *dock_host_ptr = &dock_host;
    QObject::connect(host, &QObject::destroyed, host, [dock_id, dock_host_ptr]() {
        blog(LOG_INFO, "[obs-zoom-plugin] Dock host destroyed: %s", dock_id);
        dock_host_ptr->clear();
    });
}

static void show_registered_dock(const char *dock_id,
                                 const char *dock_title,
                                 QWidget *widget,
                                 QPointer<QDockWidget> &dock_host)
{
    if (!widget)
        return;

    if (!dock_host)
        dock_host = dock_host_for(widget);
    if (!dock_host)
        dock_host = find_dock_host_by_id(dock_id);
    if (!dock_host && dock_id && dock_title) {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] Dock host missing for %s; re-registering dock",
             dock_id);
        obs_frontend_add_dock_by_id(dock_id, dock_title, widget);
        dock_host = dock_host_for(widget);
        observe_dock_host(dock_host, dock_host, dock_id);
    }

    if (dock_host) {
        if (dock_host->widget() != widget)
            dock_host->setWidget(widget);
        widget->show();
        dock_host->setVisible(true);
        dock_host->show();
        dock_host->raise();
        dock_host->activateWindow();
        widget->setFocus();
        blog(LOG_INFO, "[obs-zoom-plugin] Showing dock: %s", dock_id);
        return;
    }

    widget->show();
    widget->raise();
    widget->activateWindow();
    blog(LOG_WARNING, "[obs-zoom-plugin] Showing dock without host: %s", dock_id);
}

static ZoomDock *ensure_zoom_dock()
{
    auto *main_win = obs_main_window();
    if (!main_win) {
        blog(LOG_WARNING, "[obs-zoom-plugin] obs_frontend_get_main_window() returned null - dock not created");
        return nullptr;
    }

    if (!g_dock) {
        g_dock = new ZoomDock(main_win);
        g_dock->setObjectName(QStringLiteral("ZoomControlDock"));
        obs_frontend_add_dock_by_id("ZoomControlDock", "Zoom Control", g_dock);
        g_zoom_dock_host = dock_host_for(g_dock);
        observe_dock_widget(g_dock, g_zoom_dock_host, g_dock,
                            "ZoomControlDock");
        observe_dock_host(g_zoom_dock_host, g_zoom_dock_host,
                          "ZoomControlDock");
        blog(LOG_INFO, "[obs-zoom-plugin] Registered dock: ZoomControlDock");
    }
    return g_dock;
}

static void show_zoom_dock()
{
    ZoomDock *dock_widget = ensure_zoom_dock();
    show_registered_dock("ZoomControlDock", "Zoom Control", dock_widget,
                         g_zoom_dock_host);
}

static ZoomIsoPanel *ensure_iso_panel()
{
    auto *main_win = obs_main_window();
    if (!main_win) {
        blog(LOG_WARNING, "[obs-zoom-plugin] obs_frontend_get_main_window() returned null - ISO panel not created");
        return nullptr;
    }

    if (!g_iso_panel) {
        g_iso_panel = new ZoomIsoPanel(main_win);
        g_iso_panel->setObjectName(QStringLiteral("ZoomIsoRecorderDock"));
        obs_frontend_add_dock_by_id("ZoomIsoRecorderDock", "Zoom ISO Recorder", g_iso_panel);
        g_iso_dock_host = dock_host_for(g_iso_panel);
        observe_dock_widget(g_iso_panel, g_iso_dock_host, g_iso_panel,
                            "ZoomIsoRecorderDock");
        observe_dock_host(g_iso_dock_host, g_iso_dock_host,
                          "ZoomIsoRecorderDock");
        blog(LOG_INFO, "[obs-zoom-plugin] Registered dock: ZoomIsoRecorderDock");
    }
    return g_iso_panel;
}

static void show_iso_panel()
{
    ZoomIsoPanel *panel = ensure_iso_panel();
    if (panel)
        panel->refresh_now();
    show_registered_dock("ZoomIsoRecorderDock", "Zoom ISO Recorder", panel,
                         g_iso_dock_host);
}

static ZoomOutputDialog *ensure_output_panel()
{
    auto *main_win = obs_main_window();
    if (!main_win) {
        blog(LOG_WARNING, "[obs-zoom-plugin] obs_frontend_get_main_window() returned null - Output Manager dock not created");
        return nullptr;
    }

    if (!g_output_panel) {
        g_output_panel = new ZoomOutputDialog(main_win);
        g_output_panel->setObjectName(QStringLiteral("ZoomOutputManagerDock"));
        obs_frontend_add_dock_by_id("ZoomOutputManagerDock", "Zoom Output Manager", g_output_panel);
        g_output_dock_host = dock_host_for(g_output_panel);
        observe_dock_widget(g_output_panel, g_output_dock_host, g_output_panel,
                            "ZoomOutputManagerDock");
        observe_dock_host(g_output_dock_host, g_output_dock_host,
                          "ZoomOutputManagerDock");
        blog(LOG_INFO, "[obs-zoom-plugin] Registered dock: ZoomOutputManagerDock");
    }
    return g_output_panel;
}

static void show_output_panel()
{
    ZoomOutputDialog *panel = ensure_output_panel();
    if (panel)
        panel->refresh_now();
    show_registered_dock("ZoomOutputManagerDock", "Zoom Output Manager", panel,
                         g_output_dock_host);
}

void corevideo_show_output_manager_dock()
{
    show_output_panel();
}

static ZoomDiagnosticsDialog *ensure_diagnostics_panel()
{
    auto *main_win = obs_main_window();
    if (!main_win) {
        blog(LOG_WARNING, "[obs-zoom-plugin] obs_frontend_get_main_window() returned null - Diagnostics dock not created");
        return nullptr;
    }

    if (!g_diagnostics_panel) {
        g_diagnostics_panel = new ZoomDiagnosticsDialog(main_win);
        g_diagnostics_panel->setObjectName(QStringLiteral("ZoomDiagnosticsDock"));
        obs_frontend_add_dock_by_id("ZoomDiagnosticsDock", "Zoom Diagnostics", g_diagnostics_panel);
        g_diagnostics_dock_host = dock_host_for(g_diagnostics_panel);
        observe_dock_widget(g_diagnostics_panel, g_diagnostics_dock_host,
                            g_diagnostics_panel, "ZoomDiagnosticsDock");
        observe_dock_host(g_diagnostics_dock_host, g_diagnostics_dock_host,
                          "ZoomDiagnosticsDock");
        blog(LOG_INFO, "[obs-zoom-plugin] Registered dock: ZoomDiagnosticsDock");
    }
    return g_diagnostics_panel;
}

static void show_diagnostics_panel()
{
    ZoomDiagnosticsDialog *panel = ensure_diagnostics_panel();
    if (panel)
        panel->refresh_now();
    show_registered_dock("ZoomDiagnosticsDock", "Zoom Diagnostics", panel,
                         g_diagnostics_dock_host);
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-zoom-plugin", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "OBS Zoom Plugin - stream and record Zoom meetings directly from OBS";
}

bool obs_module_load(void)
{
    blog(LOG_INFO, "[obs-zoom-plugin] Loading plugin v%s", OBS_ZOOM_PLUGIN_VERSION);
#ifdef COREVIDEO_HW_ACCEL
    cv_ffmpeg_loader_init();
    blog(LOG_INFO, "[obs-zoom-plugin] Hardware video acceleration: %s",
         cv_ffmpeg_runtime_describe());
#else
    blog(LOG_INFO, "[obs-zoom-plugin] Hardware video acceleration: disabled (built without ENABLE_FFMPEG_HW_ACCEL - CPU path only)");
#endif
    configure_qt_plugin_paths();

#if !defined(WIN32)
    // Writing to a closed pipe (engine crashed) raises SIGPIPE on POSIX,
    // which would kill the host OBS process. Ignore it so the write returns
    // EPIPE instead and we can route the failure through normal error paths.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    zoom_source_register();
    zoom_participant_audio_source_register();
    zoom_supersource_register();
    zoom_supersource_load_gfx();
    blog(LOG_INFO, "[obs-zoom-plugin] Registered CoreVideo source kinds");

    ZoomPluginSettings s = ZoomPluginSettings::load();
    // Seed the dedicated audio path's global delay trim once, here, so the
    // operator's saved value is in force from the first buffer rather than
    // only from whenever some source next re-applies its own settings.
    corevideo_set_global_audio_delay_ms(s.audio_delay_ms);
    ZoomReconnectManager::instance().set_policy(s.reconnect_policy);
    ZoomControlServer::instance().set_token(s.control_token);
    if (!ZoomControlServer::instance().start(s.control_server_port))
        blog(LOG_WARNING, "[obs-zoom-plugin] TCP control server unavailable - continuing without it");
    if (!ZoomOscServer::instance().start(s.osc_server_port))
        blog(LOG_WARNING, "[obs-zoom-plugin] OSC server unavailable - continuing without it");

    obs_frontend_add_tools_menu_item("Zoom Plugin Settings", [](void *) {
        auto *main_win = static_cast<QMainWindow *>(obs_frontend_get_main_window());
        ZoomSettingsDialog dlg(main_win);
        dlg.exec();
    }, nullptr);

    obs_frontend_add_tools_menu_item("Zoom Output Manager", [](void *) {
        show_output_panel();
    }, nullptr);

    obs_frontend_add_tools_menu_item("Zoom Diagnostics", [](void *) {
        show_diagnostics_panel();
    }, nullptr);

    obs_frontend_add_tools_menu_item("Zoom Control", [](void *) {
        show_zoom_dock();
    }, nullptr);

    obs_frontend_add_tools_menu_item("Zoom ISO Recorder", [](void *) {
        show_iso_panel();
    }, nullptr);

    obs_frontend_add_event_callback(frontend_event_callback, nullptr);
    g_frontend_callback_registered = true;

    blog(LOG_INFO, "[obs-zoom-plugin] Plugin loaded successfully");
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[obs-zoom-plugin] Unloading plugin");
    if (g_frontend_callback_registered) {
        obs_frontend_remove_event_callback(frontend_event_callback, nullptr);
        g_frontend_callback_registered = false;
    }
    shutdown_corevideo();
    zoom_supersource_unload_gfx();
    g_dock.clear();
    g_iso_panel.clear();
    g_output_panel.clear();
    g_diagnostics_panel.clear();
    g_zoom_dock_host.clear();
    g_iso_dock_host.clear();
    g_output_dock_host.clear();
    g_diagnostics_dock_host.clear();
}
