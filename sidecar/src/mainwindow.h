#pragma once
#include "sidebar.h"
#include "layout-template.h"
#include "obs-client.h"
#include "show-theme.h"
#include "macro.h"
#include "participant-panel.h"
#include "sidecar-control-server.h"
#include "look.h"
#include "me-bus.h"
#include "preview-canvas.h"
#include "zoom-control-client.h"
#include <QMainWindow>
#include <optional>

class PreviewCanvas;
class TemplatePanel;
class LookPanel;
class ParticipantPanel;
class ThemePanel;
class ScenesPanel;
class MacrosPanel;
class SettingsPage;
class OverlayPanel;
class QLabel;
class QPushButton;
class QComboBox;
class QPlainTextEdit;
class QDockWidget;
class QStackedWidget;
class CommandPalette;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    enum class ShowPhase { PreShow, Live, PostShow };

    // Optional launch-time overrides — typically supplied by the parent
    // OBS plugin via CLI flags so the user gets a one-click connected sidecar.
    struct StartupConfig {
        std::optional<QString> hostOverride;
        std::optional<int>     portOverride;
        std::optional<QString> passwordOverride;
        bool                   autoConnect = false;
    };

    explicit MainWindow(const StartupConfig &startup, QWidget *parent = nullptr);

private slots:
    void onPageSelected(Sidebar::Page p);
    void onTemplateSelected(const LayoutTemplate &tmpl);
    void onLookSelected(const Look &look);
    void onThemeSelected(const ShowTheme &theme);
    void onApplyLayout();
    void onTake();
    void onAuto();
    void onFTB();
    void onSwapBuses();
    void onEngineToggle();
    void onObsConnect();
    void onObsState(OBSClient::State s);
    void onObsLog(const QString &msg);
    void onScenesReceived(const QStringList &scenes);
    void onVirtualCamToggle();
    void onVirtualCamState(bool active);
    void onSettingsChanged();
    void onSceneActivated(const QString &name);
    void onMacroTriggered(const Macro &macro);
    void onPhaseSelected(ShowPhase phase);
    void onSlotAssigned(int slotIndex, int participantId);
    void onSlotClicked(int slotIndex);
    void onParticipantAssignClicked(int participantId);
    void openParticipantMappingWindow();
    void openCommandPalette();
    void populateCommandPalette();

private:
    void buildTopBar(QWidget *parent);
    void buildCenterArea(QWidget *parent);
    void buildRightPanel(QWidget *parent);
    void buildLogDock();
    void loadMockParticipants();
    void provisionPlaceholderSources();
    void provisionLookScenes();
    QVector<PreviewCanvas::Participant> participantsForLook(const Look &look) const;
    QStringList sourceNamesForSlots(int slotCount) const;
    QString obsSceneNameForLook(const Look &look) const;

    // Top bar
    QWidget     *m_topBar         = nullptr;
    QWidget     *m_canvasArea     = nullptr;
    QLabel      *m_showNameLabel  = nullptr;
    QLabel      *m_obsStatusLabel = nullptr;
    QPushButton *m_engineBtn      = nullptr;
    QPushButton *m_obsBtn         = nullptr;
    bool         m_engineOn       = false;

    // Show phase segment
    QPushButton *m_preShowBtn  = nullptr;
    QPushButton *m_liveBtn     = nullptr;
    QPushButton *m_postShowBtn = nullptr;
    ShowPhase    m_phase       = ShowPhase::PreShow;

    // Toolbar buttons (kept for state updates)
    QPushButton *m_vcamBtn = nullptr;
    QPushButton *m_takeBtn = nullptr;
    QPushButton *m_autoBtn = nullptr;
    QPushButton *m_ftbBtn  = nullptr;
    QPushButton *m_swapBtn = nullptr;
    QPushButton *m_mapBtn  = nullptr;
    QComboBox   *m_autoDurationCombo = nullptr;

    // Center — m_liveCanvas renders PGM (on-air), m_sceneCanvas renders PVW.
    PreviewCanvas *m_liveCanvas   = nullptr;
    PreviewCanvas *m_sceneCanvas  = nullptr;
    QLabel        *m_pgmLabel     = nullptr;
    QLabel        *m_pvwLabel     = nullptr;

    // Right panel — stacked pages
    QStackedWidget   *m_rightStack       = nullptr;
    TemplatePanel    *m_templatePanel    = nullptr;
    LookPanel        *m_lookPanel        = nullptr;
    ParticipantPanel *m_participantPanel = nullptr;
    ThemePanel       *m_themePanel       = nullptr;
    ScenesPanel      *m_scenesPanel      = nullptr;
    MacrosPanel      *m_macrosPanel      = nullptr;
    SettingsPage     *m_settingsPage     = nullptr;
    OverlayPanel     *m_overlayPanel     = nullptr;

    // Right panel page indices
    int m_pageTemplates = 0;
    int m_pageThemes    = 0;
    int m_pageScenes    = 0;
    int m_pageMacros    = 0;
    int m_pageSettings  = 0;
    int m_pageOverlays  = 0;

    // Log
    QDockWidget    *m_logDock = nullptr;
    QPlainTextEdit *m_logView = nullptr;

    // State
    LayoutTemplate             m_currentTemplate;   // mirrors m_working.tmpl for legacy callers
    Look                       m_working;           // in-progress staged Look (mutated by panels)
    MEBus                     *m_bus            = nullptr;
    OBSClient                 *m_obsClient      = nullptr;
    OBSClient::Config          m_obsConfig;
    Sidebar                   *m_sidebar        = nullptr;
    QVector<ParticipantInfo>   m_participants;
    QStringList                m_outputSources;
    QStringList                m_lastScenes;
    SidecarControlServer      *m_controlServer  = nullptr;
    ZoomControlClient         *m_zoomClient     = nullptr;

    // Click-to-assign mode: when ≥ 0, the next participant card click is
    // routed to this slot index instead of starting a drag flow.
    int                        m_assignTargetSlot = -1;

    CommandPalette            *m_commandPalette = nullptr;
};
