#include "mainwindow.h"
#include "preview-canvas.h"
#include "template-panel.h"
#include "look-panel.h"
#include "look-library.h"
#include "theme-panel.h"
#include "participant-panel.h"
#include "scenes-panel.h"
#include "macros-panel.h"
#include "overlay-panel.h"
#include "template-manager.h"
#include "settings-page.h"
#include "obs-connect-dialog.h"
#include "obs-look-renderer.h"
#include "command-palette.h"
#include "sidecar-style.h"
#include "zoom-control-client.h"
#include <QShortcut>
#include <QKeySequence>
#include <QTimer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QDialog>
#include <QFrame>
#include <QGridLayout>
#include <QPlainTextEdit>
#include <QDockWidget>
#include <QStackedWidget>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QDateTime>
#include <QApplication>
#include <QStyle>
#include <QSizePolicy>
#include <QFileDialog>
#include <QInputDialog>
#include <QStandardPaths>
#include <QDir>
#include <QUuid>
#include <QLineEdit>
#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QDialogButtonBox>

MainWindow::MainWindow(const StartupConfig &startup, QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("CoreVideo Sidecar");
    setMinimumSize(1200, 720);
    resize(1440, 860);

    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *rootH = new QHBoxLayout(central);
    rootH->setContentsMargins(0, 0, 0, 0);
    rootH->setSpacing(0);

    // ── Left sidebar ──────────────────────────────────────────────────────────
    m_sidebar = new Sidebar(this);
    m_sidebar->setFixedWidth(200);
    rootH->addWidget(m_sidebar);

    // ── Center column ─────────────────────────────────────────────────────────
    auto *centerCol = new QWidget(central);
    centerCol->setObjectName("centerCol");
    centerCol->setStyleSheet("#centerCol { background: #0d0d12; }");
    auto *centerV = new QVBoxLayout(centerCol);
    centerV->setContentsMargins(0, 0, 0, 0);
    centerV->setSpacing(0);

    buildTopBar(centerCol);
    centerV->addWidget(m_topBar);

    buildCenterArea(centerCol);
    centerV->addWidget(m_canvasArea, 1);

    // Bottom toolbar
    auto *toolbar = new QWidget(centerCol);
    toolbar->setObjectName("bottomBar");
    toolbar->setFixedHeight(76);
    auto *tbRow = new QHBoxLayout(toolbar);
    tbRow->setContentsMargins(12, 10, 12, 10);
    tbRow->setSpacing(8);

    auto makeBtn = [&](const QString &label, bool primary = false) -> QPushButton * {
        auto *btn = new QPushButton(label, toolbar);
        btn->setObjectName("toolBtn");
        btn->setFixedHeight(44);
        if (primary) btn->setProperty("primary", "true");
        return btn;
    };

    m_swapBtn       = makeBtn("⇄  SWAP");
    m_takeBtn       = makeBtn("⏵  TAKE", true);
    m_autoBtn       = makeBtn("⏬  AUTO");
    m_ftbBtn        = makeBtn("■  FTB");
    auto *streamBtn = makeBtn("⏩  Stream");
    m_vcamBtn       = makeBtn("⏺  V-Cam OFF");

    m_renderPreviewBtn = makeBtn("Render PVW");
    m_mapBtn        = makeBtn("Map");

    m_renderPreviewBtn->setFixedWidth(110);
    m_takeBtn->setFixedWidth(110);
    m_autoBtn->setFixedWidth(100);
    m_ftbBtn->setFixedWidth(80);
    m_swapBtn->setFixedWidth(90);
    m_mapBtn->setFixedWidth(82);

    // AUTO duration picker — common broadcast values 0.5s..5s.
    m_autoDurationCombo = new QComboBox(toolbar);
    m_autoDurationCombo->setFixedHeight(36);
    m_autoDurationCombo->setFixedWidth(76);
    m_autoDurationCombo->addItem("0.5s",  500);
    m_autoDurationCombo->addItem("1.0s", 1000);
    m_autoDurationCombo->addItem("1.5s", 1500);
    m_autoDurationCombo->addItem("2.0s", 2000);
    m_autoDurationCombo->addItem("5.0s", 5000);
    m_autoDurationCombo->setCurrentIndex(2); // 1.5s default

    tbRow->addWidget(m_swapBtn);
    tbRow->addWidget(m_renderPreviewBtn);
    tbRow->addWidget(m_takeBtn);
    tbRow->addWidget(m_autoBtn);
    tbRow->addWidget(m_autoDurationCombo);
    tbRow->addWidget(m_ftbBtn);
    tbRow->addWidget(m_mapBtn);
    tbRow->addStretch(1);
    tbRow->addWidget(m_vcamBtn);
    tbRow->addWidget(streamBtn);

    centerV->addWidget(toolbar);
    rootH->addWidget(centerCol, 1);

    // ── Right panel ───────────────────────────────────────────────────────────
    auto *rightOuter = new QWidget(central);
    rightOuter->setObjectName("rightPanel");
    rightOuter->setFixedWidth(272);
    buildRightPanel(rightOuter);
    rootH->addWidget(rightOuter);

    // ── Log dock ──────────────────────────────────────────────────────────────
    buildLogDock();

    // ── OBS config from settings ──────────────────────────────────────────────
    QSettings cfg;
    m_obsConfig.host          = cfg.value("obs/host", "localhost").toString();
    m_obsConfig.port          = cfg.value("obs/port", 4455).toInt();
    m_obsConfig.password      = cfg.value("obs/password", "").toString();
    m_obsConfig.autoReconnect = cfg.value("obs/autoReconnect", true).toBool();

    // CLI / launch-time overrides — typically supplied by the parent OBS
    // plugin so "Launch Sidecar" delivers a one-click connected session.
    if (startup.hostOverride)     m_obsConfig.host     = *startup.hostOverride;
    if (startup.portOverride)     m_obsConfig.port     = *startup.portOverride;
    if (startup.passwordOverride) m_obsConfig.password = *startup.passwordOverride;

    // ── M/E bus ──────────────────────────────────────────────────────────────
    m_bus = new MEBus(this);
    connect(m_bus, &MEBus::previewChanged, this, [this](const Look &l) {
        m_sceneCanvas->setTemplate(l.tmpl);
        m_sceneCanvas->setParticipants(participantsForLook(l));
        m_sceneCanvas->setOverlays(l.overlays);
        m_sceneCanvas->setBackgroundImage(l.backgroundImagePath);
        m_sceneCanvas->setTileStyle(l.tileStyle);
        if (m_overlayPanel) m_overlayPanel->setActiveOverlays(l.overlays);
    });
    connect(m_bus, &MEBus::programChanged, this, [this](const Look &l) {
        m_liveCanvas->setTemplate(l.tmpl);
        m_liveCanvas->setParticipants(participantsForLook(l));
        m_liveCanvas->setOverlays(l.overlays);
        m_liveCanvas->setBackgroundImage(l.backgroundImagePath);
        m_liveCanvas->setTileStyle(l.tileStyle);
    });
    // TAKE is the only path that pushes to OBS.
    connect(m_bus, &MEBus::tookProgram, this, [this](const Look &l) {
        m_currentTemplate = l.tmpl;
        m_controlServer->notifyTemplateChanged(l.tmpl.id, l.tmpl.name);
        onApplyLayout();
    });

    // ── Connections ───────────────────────────────────────────────────────────
    connect(m_sidebar, &Sidebar::pageSelected, this, &MainWindow::onPageSelected);
    connect(m_takeBtn, &QPushButton::clicked,  this, &MainWindow::onTake);
    connect(m_autoBtn, &QPushButton::clicked,  this, &MainWindow::onAuto);
    connect(m_ftbBtn,  &QPushButton::clicked,  this, &MainWindow::onFTB);
    connect(m_swapBtn, &QPushButton::clicked,  this, &MainWindow::onSwapBuses);
    connect(m_engineBtn, &QPushButton::clicked, this, &MainWindow::onEngineToggle);
    connect(m_renderPreviewBtn, &QPushButton::clicked, this, &MainWindow::onRenderPreview);
    connect(m_mapBtn, &QPushButton::clicked, this, &MainWindow::openParticipantMappingWindow);
    connect(m_obsBtn,  &QPushButton::clicked,  this, &MainWindow::onObsConnect);
    connect(m_vcamBtn, &QPushButton::clicked,  this, &MainWindow::onVirtualCamToggle);

    // Spacebar = TAKE, Enter = AUTO, F12 = FTB
    auto *takeShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
    takeShortcut->setContext(Qt::ApplicationShortcut);
    connect(takeShortcut, &QShortcut::activated, this, &MainWindow::onTake);

    auto *autoShortcut = new QShortcut(QKeySequence(Qt::Key_Return), this);
    autoShortcut->setContext(Qt::ApplicationShortcut);
    connect(autoShortcut, &QShortcut::activated, this, &MainWindow::onAuto);

    auto *autoShortcut2 = new QShortcut(QKeySequence(Qt::Key_Enter), this);
    autoShortcut2->setContext(Qt::ApplicationShortcut);
    connect(autoShortcut2, &QShortcut::activated, this, &MainWindow::onAuto);

    auto *ftbShortcut = new QShortcut(QKeySequence(Qt::Key_F12), this);
    ftbShortcut->setContext(Qt::ApplicationShortcut);
    connect(ftbShortcut, &QShortcut::activated, this, &MainWindow::onFTB);

    // Bus drives PGM canvas opacity during AUTO / FTB.
    connect(m_bus, &MEBus::pgmOpacityChanged, this, [this](float o) {
        if (m_liveCanvas) m_liveCanvas->setOpacity(o);
    });
    // Disable transition buttons while a transition is in flight to keep
    // the gesture atomic — no overlapping AUTOs.
    connect(m_bus, &MEBus::transitionStarted, this,
            [this](MEBus::TransitionKind k, int) {
        m_takeBtn->setEnabled(false);
        m_autoBtn->setEnabled(false);
        m_swapBtn->setEnabled(false);
        // FTB stays enabled only when itself is the active transition, so
        // the operator can interrupt — but for slice 4 just lock it out.
        m_ftbBtn->setEnabled(false);
        if (k == MEBus::FTBOn || k == MEBus::FTBOff) {
            m_ftbBtn->setText((k == MEBus::FTBOn) ? "■  …→BLK" : "■  BLK→…");
        }
    });
    connect(m_bus, &MEBus::transitionEnded, this, [this]() {
        m_takeBtn->setEnabled(true);
        m_autoBtn->setEnabled(true);
        m_swapBtn->setEnabled(true);
        m_ftbBtn->setEnabled(true);
        m_ftbBtn->setText(m_bus->isFTBActive() ? "■  FTB ON" : "■  FTB");
    });

    m_obsClient = new OBSClient(this);
    connect(m_obsClient, &OBSClient::stateChanged,       this, &MainWindow::onObsState);
    connect(m_obsClient, &OBSClient::log,                this, &MainWindow::onObsLog);
    connect(m_obsClient, &OBSClient::scenesReceived,     this, &MainWindow::onScenesReceived);
    connect(m_obsClient, &OBSClient::sceneItemsReceived, this,
            [this](const QString &, const QVector<OBSClient::SceneItem> &) {
        updateSceneSyncStatus();
    });
    connect(m_obsClient, &OBSClient::sceneChanged,       this, [this](const QString &name) {
        m_scenesPanel->setCurrentScene(name);
        stageLookFromObsScene(name);
    });
    connect(m_obsClient, &OBSClient::virtualCamStateChanged, this, &MainWindow::onVirtualCamState);
    connect(m_obsClient, &OBSClient::templateApplied,    this,
            [this](const QString &name, int n) {
                onObsLog(QStringLiteral("✓ Applied '%1' (%2 items).").arg(name).arg(n));
            });
    connect(m_obsClient, &OBSClient::requestFailed, this, [this](const QString &summary) {
        if (!m_sceneSyncStatusLabel)
            return;
        m_obsSyncState = ObsSyncState::Error;
        m_lastSyncError = summary;
        m_sceneSyncStatusLabel->setText(QStringLiteral("Sync error"));
        m_sceneSyncStatusLabel->setToolTip(summary);
        m_sceneSyncStatusLabel->setStyleSheet("color: #e04040; font-size: 11px; background: transparent;");
    });
    connect(m_obsClient, &OBSClient::inventoryReady, this, [this]() {
        provisionLookScenes();
        updateSceneSyncStatus();
    });

    m_controlServer = new SidecarControlServer(this);
    m_controlServer->start();

    connect(m_obsClient, &OBSClient::stateChanged, this, [this](OBSClient::State) {
        m_controlServer->notifyOBSState(m_obsClient->stateLabel());
    });
    connect(m_obsClient, &OBSClient::scenesReceived, this, [this](const QStringList &scenes) {
        m_controlServer->notifyScenesUpdated(scenes);
    });
    connect(m_obsClient, &OBSClient::sceneChanged, this, [this](const QString &scene) {
        m_controlServer->notifySceneChanged(scene);
    });

    connect(m_controlServer, &SidecarControlServer::phaseChangeRequested,
            this, [this](const QString &phase) {
        if (phase == "live")           onPhaseSelected(ShowPhase::Live);
        else if (phase == "post_show") onPhaseSelected(ShowPhase::PostShow);
        else                           onPhaseSelected(ShowPhase::PreShow);
    });
    // Companion / remote "apply template" = stage on PVW + immediate TAKE.
    connect(m_controlServer, &SidecarControlServer::templateApplyRequested,
            this, [this](const QString &id) {
        auto &tm = TemplateManager::instance();
        if (const auto *t = tm.findById(id)) {
            onTemplateSelected(*t);
            onTake();
        }
    });
    connect(m_controlServer, &SidecarControlServer::sceneChangeRequested,
            this, [this](const QString &scene) {
        onSceneActivated(scene);
    });

    m_zoomClient = new ZoomControlClient(this);
    connect(m_zoomClient, &ZoomControlClient::log, this, &MainWindow::onObsLog);
    connect(m_zoomClient, &ZoomControlClient::participantsUpdated,
            this, [this](const QVector<ParticipantInfo> &participants) {
        reconcileParticipantSlots(participants);
        m_participantPanel->setParticipants(m_participants);
        if (m_sceneCanvas) m_sceneCanvas->setParticipants(participantsForLook(m_working));
        if (m_liveCanvas && m_bus) m_liveCanvas->setParticipants(participantsForLook(m_bus->program()));
        syncZoomOutputAssignments();
    });
    connect(m_zoomClient, &ZoomControlClient::outputSourcesUpdated,
            this, [this](const QStringList &sourceNames) {
        m_outputSources = sourceNames;
    });
    m_zoomClient->start();

    // Canvas slot assignment from drag-and-drop
    connect(m_liveCanvas,  &PreviewCanvas::slotAssigned, this, &MainWindow::onSlotAssigned);
    connect(m_sceneCanvas, &PreviewCanvas::slotAssigned, this, &MainWindow::onSlotAssigned);

    // Click-to-assign — selecting a slot focuses the Templates page and arms
    // the participant panel so the next card click fills that slot.
    connect(m_liveCanvas,  &PreviewCanvas::slotClicked, this, &MainWindow::onSlotClicked);
    connect(m_sceneCanvas, &PreviewCanvas::slotClicked, this, &MainWindow::onSlotClicked);

    // Right-click on a slot cycles its audio routing (Mix → Iso → Aud → Mix).
    connect(m_liveCanvas,  &PreviewCanvas::slotRoutingCycleRequested,
            this, &MainWindow::onSlotRoutingCycle);
    connect(m_sceneCanvas, &PreviewCanvas::slotRoutingCycleRequested,
            this, &MainWindow::onSlotRoutingCycle);

    // Participant card click — consumed only while assign mode is armed
    connect(m_participantPanel, &ParticipantPanel::assignRequested, this,
            [this](int pid, int slot) { onSlotAssigned(slot, pid); });
    connect(m_participantPanel, &ParticipantPanel::participantClicked,
            this, &MainWindow::onParticipantAssignClicked);

    // Command palette (Ctrl+K / Cmd+K)
    m_commandPalette = new CommandPalette(this);
    auto *paletteShortcut = new QShortcut(
        QKeySequence(QKeySequence::Find), this);
    paletteShortcut->setContext(Qt::ApplicationShortcut);
    connect(paletteShortcut, &QShortcut::activated,
            this, &MainWindow::openCommandPalette);
    auto *paletteShortcutK = new QShortcut(
        QKeySequence(Qt::CTRL | Qt::Key_K), this);
    paletteShortcutK->setContext(Qt::ApplicationShortcut);
    connect(paletteShortcutK, &QShortcut::activated,
            this, &MainWindow::openCommandPalette);

    // Templates (used by command palette + LookLibrary resolution)
    auto &tm = TemplateManager::instance();
    tm.loadBuiltIn();
    m_templatePanel->loadTemplates(tm.templates());

    // Broadcast-ready Looks library — load after templates so each Look's
    // templateId can be resolved into an in-memory LayoutTemplate.
    auto &ll = LookLibrary::instance();
    ll.loadBuiltIn();
    loadCustomLooks();
    refreshLookPanel();

    // Themes
    m_themePanel->loadThemes(ShowTheme::builtIns());

    // Stage a sensible default on PVW. Prefer the first preset Look; if
    // the library is empty, fall back to the bare 4-up template.
    if (!ll.looks().isEmpty()) {
        onLookSelected(ll.looks().first());
    } else if (const auto *grid4 = tm.findById("4-up-grid")) {
        onTemplateSelected(*grid4);
    }

    // Mock data — also stages slot assignments onto PVW.
    loadMockParticipants();

    // Commit the staged default to PGM so first paint matches PVW.
    m_bus->take();

    // Initial UI state
    onObsState(OBSClient::State::Disconnected);
    onObsLog("Sidecar ready. Click 'OBS' to connect, or press Ctrl+K for the command palette.");

    // Auto-connect on launch if the parent process asked for it (e.g. when
    // launched from the OBS plugin's "Launch Sidecar" button).
    if (startup.autoConnect) {
        QSettings s;
        s.setValue("obs/host",     m_obsConfig.host);
        s.setValue("obs/port",     m_obsConfig.port);
        s.setValue("obs/password", m_obsConfig.password);
        // Defer one tick so the window paints first
        QTimer::singleShot(0, this, [this]() {
            onObsLog(QStringLiteral("Auto-connecting to %1:%2 …")
                         .arg(m_obsConfig.host).arg(m_obsConfig.port));
            m_obsClient->connectToOBS(m_obsConfig);
        });
    }
}

// ── Top bar ───────────────────────────────────────────────────────────────────
void MainWindow::buildTopBar(QWidget *parent)
{
    m_topBar = new QWidget(parent);
    m_topBar->setObjectName("topBar");
    m_topBar->setFixedHeight(52);

    auto *row = new QHBoxLayout(m_topBar);
    row->setContentsMargins(16, 0, 12, 0);
    row->setSpacing(10);

    m_showNameLabel = new QLabel("Morning Show — Episode 142", m_topBar);
    m_showNameLabel->setStyleSheet("color: #e0e0f0; font-size: 15px; font-weight: 700;");

    // Show phase segmented control
    auto *phaseBar = new QWidget(m_topBar);
    phaseBar->setObjectName("phaseSegment");
    auto *phl = new QHBoxLayout(phaseBar);
    phl->setContentsMargins(2, 2, 2, 2);
    phl->setSpacing(0);

    m_preShowBtn  = new QPushButton("PRE",     phaseBar);
    m_liveBtn     = new QPushButton("● LIVE",  phaseBar);
    m_postShowBtn = new QPushButton("POST",    phaseBar);

    m_preShowBtn->setObjectName("phasePreBtn");
    m_liveBtn->setObjectName("phaseLiveBtn");
    m_postShowBtn->setObjectName("phasePostBtn");

    for (auto *b : {m_preShowBtn, m_liveBtn, m_postShowBtn}) {
        b->setCheckable(true);
        b->setFixedHeight(28);
        phl->addWidget(b);
    }
    m_preShowBtn->setChecked(true);

    connect(m_preShowBtn,  &QPushButton::clicked, this,
            [this]{ onPhaseSelected(ShowPhase::PreShow);  });
    connect(m_liveBtn,     &QPushButton::clicked, this,
            [this]{ onPhaseSelected(ShowPhase::Live);     });
    connect(m_postShowBtn, &QPushButton::clicked, this,
            [this]{ onPhaseSelected(ShowPhase::PostShow); });

    m_obsStatusLabel = new QLabel("Disconnected", m_topBar);
    m_obsStatusLabel->setStyleSheet("color: #8080a0; font-size: 11px; background: transparent;");

    m_sceneSyncStatusLabel = new QLabel("Sync idle", m_topBar);
    m_sceneSyncStatusLabel->setStyleSheet("color: #8080a0; font-size: 11px; background: transparent;");

    m_engineBtn = new QPushButton("Scene Sync OFF", m_topBar);
    m_engineBtn->setObjectName("engineOffBtn");
    m_engineBtn->setFixedHeight(34);

    m_obsBtn = new QPushButton("OBS  ○", m_topBar);
    m_obsBtn->setObjectName("obsBtn");
    m_obsBtn->setFixedHeight(34);

    row->addWidget(m_showNameLabel);
    row->addStretch(1);
    row->addWidget(phaseBar);
    row->addWidget(m_obsStatusLabel);
    row->addWidget(m_sceneSyncStatusLabel);
    row->addSpacing(4);
    row->addWidget(m_obsBtn);
    row->addWidget(m_engineBtn);
}

// ── Center canvas area ────────────────────────────────────────────────────────
void MainWindow::buildCenterArea(QWidget *parent)
{
    m_canvasArea = new QWidget(parent);
    m_canvasArea->setMaximumHeight(560);
    m_canvasArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *row = new QHBoxLayout(m_canvasArea);
    row->setContentsMargins(12, 10, 12, 4);
    row->setSpacing(12);

    auto buildPane = [&](const QString &label, const QString &color,
                         PreviewCanvas *&out, QLabel *&outLbl) {
        auto *wrap = new QWidget(m_canvasArea);
        auto *v = new QVBoxLayout(wrap);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);
        outLbl = new QLabel(label, wrap);
        outLbl->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: 800; "
                                      "letter-spacing: 0.1em; background: transparent;")
                                  .arg(color));
        out = new PreviewCanvas(wrap);
        out->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        v->addWidget(outLbl);
        v->addWidget(out, 1);
        return wrap;
    };

    // PVW on the left (where you build), PGM on the right (what's on air) —
    // matches the visual flow "stage → take → on air."
    row->addWidget(buildPane("PVW  ◉  PREVIEW", "#20c460", m_sceneCanvas, m_pvwLabel), 1);
    row->addWidget(buildPane("PGM  ●  ON AIR",  "#ff4040", m_liveCanvas,  m_pgmLabel), 1);
}

static QString customLooksPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return QDir(dir).filePath(QStringLiteral("custom-looks.json"));
}

void MainWindow::loadCustomLooks()
{
    m_customLooks.clear();
    QFile f(customLooksPath());
    if (!f.open(QIODevice::ReadOnly))
        return;

    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return;

    auto &tm = TemplateManager::instance();
    for (const auto &v : doc.array()) {
        Look look = Look::fromJson(v.toObject());
        if (const auto *t = tm.findById(look.templateId))
            look.tmpl = *t;
        if (look.isValid())
            m_customLooks.append(look);
    }
}

void MainWindow::saveCustomLooks() const
{
    QJsonArray arr;
    for (const auto &look : m_customLooks)
        arr.append(look.toJson());

    QFile f(customLooksPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
}

QVector<Look> MainWindow::allLooks() const
{
    QVector<Look> looks = LookLibrary::instance().looks();
    looks += m_customLooks;
    return looks;
}

void MainWindow::refreshLookPanel()
{
    if (m_lookPanel)
        m_lookPanel->loadLooks(allLooks());
}

OBSLookRenderer::Config MainWindow::obsRendererConfig() const
{
    OBSLookRenderer::Config config;
    if (m_settingsPage) {
        config.sourcePattern = m_settingsPage->sourcePattern();
        config.fallbackSceneName = m_settingsPage->targetScene();
        config.canvasWidth = m_settingsPage->canvasWidth();
        config.canvasHeight = m_settingsPage->canvasHeight();
    }
    config.normalizeBroadcastCanvas();
    return config;
}

OBSLookRenderer MainWindow::obsRenderer() const
{
    return OBSLookRenderer(m_obsClient, obsRendererConfig());
}

QVector<PreviewCanvas::Participant>
MainWindow::participantsForLook(const Look &look) const
{
    const int nSlots = look.tmpl.slotList.size();
    QVector<PreviewCanvas::Participant> cp(nSlots);
    const QVector<int> participantSlots = participantSlotIndexesForLook(look);
    for (const auto &s : look.slotAssignments) {
        if (!participantSlots.contains(s.slotIndex)) continue;
        if (s.slotIndex < 0 || s.slotIndex >= nSlots) continue;
        for (const auto &p : m_participants) {
            if (p.id == s.participantId) {
                if (look.tileStyle.excludeNoVideo && !p.hasVideo)
                    break;
                cp[s.slotIndex] = {p.name, p.initials, p.color, p.isTalking, p.hasVideo};
                break;
            }
        }
    }
    return cp;
}

QStringList MainWindow::slotLabelsForLook(const Look &look) const
{
    QStringList labels;
    labels.reserve(8);
    for (int i = 0; i < 8; ++i)
        labels << QStringLiteral("Slot %1").arg(i + 1);

    const QVector<int> participantSlots = participantSlotIndexesForLook(look);
    for (const auto &s : look.slotAssignments) {
        if (!participantSlots.contains(s.slotIndex) || s.slotIndex < 0)
            continue;
        for (const auto &p : m_participants) {
            if (p.id != s.participantId)
                continue;
            if (look.tileStyle.excludeNoVideo && !p.hasVideo)
                break;
            while (labels.size() <= s.slotIndex)
                labels << QStringLiteral("Slot %1").arg(labels.size() + 1);
            labels[s.slotIndex] = p.name;
            break;
        }
    }
    return labels;
}

QVector<int> MainWindow::participantSlotIndexesForLook(const Look &look) const
{
    QVector<int> participantSlots;
    for (const TemplateSlot &slot : look.tmpl.slotList) {
        if ((look.templateId == QStringLiteral("speaker-screenshare")
             || look.tmpl.id == QStringLiteral("speaker-screenshare"))
            && slot.index == 1) {
            continue;
        }
        participantSlots.append(slot.index);
    }
    return participantSlots;
}

Look MainWindow::lookWithCurrentAssignments(const Look &look) const
{
    Look staged = look;
    const QVector<int> participantSlots = participantSlotIndexesForLook(staged);
    staged.slotAssignments.clear();

    for (const auto &participant : m_participants) {
        if (participant.slotAssign < 0 || !participantSlots.contains(participant.slotAssign))
            continue;
        staged.slotAssignments.append({participant.slotAssign, participant.id});
    }

    return staged;
}

QStringList MainWindow::sourceNamesForSlots(int slotCount) const
{
    return obsRenderer().sourceNamesForSlots(slotCount);
}

QStringList MainWindow::sourceNamesForLook(const Look &look) const
{
    return obsRenderer().sourceNamesForLook(look);
}

QStringList MainWindow::lookSceneNames() const
{
    return obsRenderer().sceneNamesForLooks(allLooks());
}

QString MainWindow::obsSceneNameForLook(const Look &look) const
{
    return obsRenderer().sceneNameForLook(look);
}

const Look *MainWindow::lookForObsSceneName(const QString &sceneName) const
{
    const QString normalized = sceneName.trimmed();
    for (const auto &look : LookLibrary::instance().looks()) {
        if (!look.tmpl.isValid())
            continue;
        if (obsSceneNameForLook(look) == normalized)
            return &look;
    }
    for (const auto &look : m_customLooks) {
        if (!look.tmpl.isValid())
            continue;
        if (obsSceneNameForLook(look) == normalized)
            return &look;
    }
    return nullptr;
}

void MainWindow::stageLookFromObsScene(const QString &sceneName)
{
    const Look *look = lookForObsSceneName(sceneName);
    if (!look)
        return;

    m_working = lookWithCurrentAssignments(*look);
    m_working.templateId = look->tmpl.id.isEmpty() ? look->templateId : look->tmpl.id;
    if (m_bus)
        m_bus->stageLook(m_working);
    onObsLog(QStringLiteral("OBS scene linked to Look: %1.").arg(m_working.name));
}

// ── Right panel ───────────────────────────────────────────────────────────────
void MainWindow::buildRightPanel(QWidget *parent)
{
    auto *vl = new QVBoxLayout(parent);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    m_rightStack = new QStackedWidget(parent);

    // Page 0: Looks gallery + Participants (combined scroll)
    auto *tmplPage = new QWidget;
    auto *tmplV = new QVBoxLayout(tmplPage);
    tmplV->setContentsMargins(0, 0, 0, 0);
    tmplV->setSpacing(0);
    m_lookPanel = new LookPanel(tmplPage);
    connect(m_lookPanel, &LookPanel::lookSelected,
            this, &MainWindow::onLookSelected);
    connect(m_lookPanel, &LookPanel::createLookRequested,
            this, &MainWindow::onCreateLookRequested);
    connect(m_lookPanel, &LookPanel::setBackgroundRequested,
            this, &MainWindow::onSetBackgroundRequested);
    connect(m_lookPanel, &LookPanel::designLookRequested,
            this, &MainWindow::onDesignLookRequested);
    tmplV->addWidget(m_lookPanel, 3);
    auto *div = new QFrame(tmplPage);
    div->setFrameShape(QFrame::HLine);
    div->setStyleSheet("color: #1e1e2e;");
    tmplV->addWidget(div);
    m_participantPanel = new ParticipantPanel(tmplPage);
    m_participantPanel->setMinimumHeight(300);
    tmplV->addWidget(m_participantPanel, 2);
    m_pageTemplates = m_rightStack->addWidget(tmplPage);

    // TemplatePanel still constructed (off-screen) so the command-palette
    // path that references TemplateManager can stage raw layouts. Kept for
    // back-compat; not visible in the right panel.
    m_templatePanel = new TemplatePanel;
    m_templatePanel->hide();
    connect(m_templatePanel, &TemplatePanel::templateSelected,
            this, &MainWindow::onTemplateSelected);

    // Page 1: Themes
    m_themePanel = new ThemePanel;
    connect(m_themePanel, &ThemePanel::themeSelected,
            this, &MainWindow::onThemeSelected);
    m_pageThemes = m_rightStack->addWidget(m_themePanel);

    // Page 2: Scenes
    m_scenesPanel = new ScenesPanel;
    connect(m_scenesPanel, &ScenesPanel::sceneActivated,
            this, &MainWindow::onSceneActivated);
    connect(m_scenesPanel, &ScenesPanel::refreshRequested, this, [this]() {
        m_obsClient->refreshInventory();
    });
    m_pageScenes = m_rightStack->addWidget(m_scenesPanel);

    // Page 3: Macros
    m_macrosPanel = new MacrosPanel;
    connect(m_macrosPanel, &MacrosPanel::macroTriggered,
            this, &MainWindow::onMacroTriggered);
    m_pageMacros = m_rightStack->addWidget(m_macrosPanel);

    // Page: Overlays
    m_overlayPanel = new OverlayPanel;
    connect(m_overlayPanel, &OverlayPanel::overlayRequested,
            this, [this](const Overlay &ov) {
        m_working.overlays.append(ov);
        m_bus->stageLook(m_working);
        m_overlayPanel->setActiveOverlays(m_working.overlays);
        onObsLog(QStringLiteral("Overlay staged on PVW: %1").arg(Overlay::humanLabel(ov)));
    });
    connect(m_overlayPanel, &OverlayPanel::removeOverlayRequested,
            this, [this](int idx) {
        if (idx < 0 || idx >= m_working.overlays.size()) return;
        m_working.overlays.remove(idx);
        m_bus->stageLook(m_working);
        m_overlayPanel->setActiveOverlays(m_working.overlays);
    });
    connect(m_overlayPanel, &OverlayPanel::clearOverlaysRequested,
            this, [this]() {
        m_working.overlays.clear();
        m_bus->stageLook(m_working);
        m_overlayPanel->setActiveOverlays(m_working.overlays);
        onObsLog("Cleared PVW overlays.");
    });
    m_pageOverlays = m_rightStack->addWidget(m_overlayPanel);

    // Page 4: Settings
    m_settingsPage = new SettingsPage;
    connect(m_settingsPage, &SettingsPage::settingsChanged,
            this, &MainWindow::onSettingsChanged);
    m_pageSettings = m_rightStack->addWidget(m_settingsPage);

    vl->addWidget(m_rightStack, 1);
}

// ── Log dock ──────────────────────────────────────────────────────────────────
void MainWindow::buildLogDock()
{
    m_logDock = new QDockWidget("OBS Log", this);
    m_logDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    m_logView = new QPlainTextEdit(m_logDock);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(500);
    m_logView->setStyleSheet(
        "QPlainTextEdit { background-color: #0a0a10; color: #b0b0d0; "
        "font-family: 'Menlo', 'Consolas', monospace; font-size: 11px; "
        "border: none; padding: 6px; }");
    m_logDock->setMaximumHeight(160);
    m_logDock->setWidget(m_logView);
    addDockWidget(Qt::BottomDockWidgetArea, m_logDock);
    m_logDock->hide();
}

// ── Mock data ─────────────────────────────────────────────────────────────────
void MainWindow::loadMockParticipants()
{
    m_participants = {
        {1, "Placeholder 1", "P1", QColor(0x1e, 0x6a, 0xe0), true, false, false, 0},
        {2, "Placeholder 2", "P2", QColor(0x20, 0xa0, 0x60), true, false, false, 1},
        {3, "Placeholder 3", "P3", QColor(0x9b, 0x40, 0xd0), true, false, false, 2},
        {4, "Placeholder 4", "P4", QColor(0xe0, 0x60, 0x20), true, false, false, 3},
        {5, "Placeholder 5", "P5", QColor(0x29, 0x79, 0xff), true, false, false, 4},
        {6, "Placeholder 6", "P6", QColor(0xd0, 0x40, 0x90), true, false, false, 5},
        {7, "Placeholder 7", "P7", QColor(0xb0, 0x90, 0x20), true, false, false, 6},
        {8, "Placeholder 8", "P8", QColor(0x40, 0xa0, 0xc0), true, false, false, 7},
    };
    m_participantPanel->setParticipants(m_participants);

    // Seed the working Look's slot assignments from each participant's
    // default slotAssign so first-paint shows people in slots.
    m_working.slotAssignments.clear();
    const QVector<int> participantSlots = participantSlotIndexesForLook(m_working);
    for (const auto &p : m_participants) {
        if (p.slotAssign >= 0 && participantSlots.contains(p.slotAssign))
            m_working.slotAssignments.append({p.slotAssign, p.id});
    }
    if (m_bus) m_bus->stageLook(m_working);
}

// ── Slots ─────────────────────────────────────────────────────────────────────
void MainWindow::provisionPlaceholderSources()
{
    if (!m_obsClient || !m_obsClient->isConnected() || !m_settingsPage)
        return;

    obsRenderer().provisionPlaceholders(8);
    QTimer::singleShot(3400, this, &MainWindow::updateSceneSyncStatus);
}

void MainWindow::provisionLookScenes()
{
    if (!m_obsClient || !m_obsClient->isConnected() || !m_settingsPage)
        return;

    obsRenderer().provisionLooks(allLooks());
    onObsLog("Control ON: provisioning shared sources and Look scenes in OBS.");
    QTimer::singleShot(3600, this, &MainWindow::updateSceneSyncStatus);
}

void MainWindow::repairCoreVideoDuplicates()
{
    if (!m_obsClient || !m_obsClient->isConnected()) {
        onObsLog("Repair skipped: OBS is not connected.");
        return;
    }
    m_obsClient->removeStaleCoreVideoDuplicates(sourceNamesForSlots(8), lookSceneNames());
}

void MainWindow::renderLookToOBS(const Look &look, bool makeProgram)
{
    if (!m_obsClient || !m_obsClient->isConnected() || !m_settingsPage)
        return;
    if (!look.tmpl.isValid()) {
        onObsLog(QStringLiteral("Look '%1' has no valid template to render.").arg(look.name));
        return;
    }

    const QString scene = obsSceneNameForLook(look);
    m_obsSyncState = ObsSyncState::Applying;
    m_lastSyncError.clear();
    m_lastRenderedLookName = look.name;
    m_lastRenderedSceneName = scene;
    m_lastExpectedDesignLayers = obsRenderer().designLayerSourceNames(look);
    if (m_sceneSyncStatusLabel) {
        m_sceneSyncStatusLabel->setText(QStringLiteral("Applying %1").arg(look.name.left(24)));
        m_sceneSyncStatusLabel->setToolTip(QStringLiteral("Rendering Look '%1' into OBS scene '%2'.")
                                           .arg(look.name, scene));
        m_sceneSyncStatusLabel->setStyleSheet("color: #e0a020; font-size: 11px; background: transparent;");
    }
    obsRenderer().renderLook(look, makeProgram, slotLabelsForLook(look));
    QTimer::singleShot(3300, this, [this, scene]() {
        if (m_obsClient && m_obsClient->isConnected())
            m_obsClient->requestSceneItems(scene);
    });
    QTimer::singleShot(3800, this, [this]() {
        if (m_obsSyncState == ObsSyncState::Applying)
            m_obsSyncState = ObsSyncState::Dirty;
        updateSceneSyncStatus();
    });
    onObsLog(QStringLiteral("Rendered Look '%1' to OBS scene '%2'.")
                 .arg(look.name, scene));
}

void MainWindow::reconcileParticipantSlots(const QVector<ParticipantInfo> &participants)
{
    QVector<ParticipantInfo> merged = participants;
    QSet<int> usedSlots;

    for (auto &participant : merged) {
        for (const auto &existing : m_participants) {
            if (existing.id != participant.id)
                continue;
            if (existing.slotAssign >= 0 && existing.slotAssign < 8
                && !usedSlots.contains(existing.slotAssign)) {
                participant.slotAssign = existing.slotAssign;
                usedSlots.insert(existing.slotAssign);
            }
            break;
        }
    }

    int nextSlot = 0;
    for (auto &participant : merged) {
        if (participant.slotAssign >= 0 || !participant.hasVideo)
            continue;
        while (nextSlot < 8 && usedSlots.contains(nextSlot))
            ++nextSlot;
        if (nextSlot >= 8)
            break;
        participant.slotAssign = nextSlot;
        usedSlots.insert(nextSlot);
    }

    m_participants = merged;
    m_working = lookWithCurrentAssignments(m_working);
    if (m_bus)
        m_bus->stageLook(m_working);
}

void MainWindow::syncZoomOutputAssignments()
{
    if (!m_zoomClient || !m_settingsPage)
        return;

    QHash<int, int> current;
    for (const auto &participant : m_participants) {
        if (!participant.hasVideo || participant.slotAssign < 0 || participant.slotAssign >= 8)
            continue;
        current.insert(participant.slotAssign, participant.id);
    }

    const QStringList sources = sourceNamesForSlots(8);
    for (auto it = current.constBegin(); it != current.constEnd(); ++it) {
        if (m_lastSyncedSlotParticipants.value(it.key(), -1) == it.value())
            continue;
        const QString sourceName = sources.value(it.key());
        if (!sourceName.isEmpty()) {
            const AudioRouting routing =
                m_slotRouting.value(it.key(), AudioRouting::Mixed);
            m_zoomClient->assignOutput(sourceName, it.value(), routing, "mono");
        }
    }

    for (auto it = m_lastSyncedSlotParticipants.constBegin();
         it != m_lastSyncedSlotParticipants.constEnd(); ++it) {
        if (current.contains(it.key()))
            continue;
        const QString sourceName = sources.value(it.key());
        if (!sourceName.isEmpty())
            m_zoomClient->assignOutput(sourceName, 0, AudioRouting::Mixed, "mono");
    }

    m_lastSyncedSlotParticipants = current;
}

void MainWindow::openParticipantMappingWindow()
{
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("CoreVideo Participant Mapping");
    dlg->resize(760, 520);

    auto *root = new QVBoxLayout(dlg);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto *status = new QLabel(dlg);
    status->setWordWrap(true);
    root->addWidget(status);

    auto refreshStatus = [this, status]() {
        const QString obs = (m_obsClient && m_obsClient->isConnected())
            ? QStringLiteral("connected")
            : QStringLiteral("not connected");
        status->setText(QStringLiteral("OBS %1. Zoom video participants: %2. Known CoreVideo sources: %3.")
                            .arg(obs)
                            .arg(m_participants.size())
                            .arg(m_outputSources.size()));
    };
    refreshStatus();

    auto *actions = new QHBoxLayout;
    auto *refreshBtn = new QPushButton("Refresh Zoom/OBS", dlg);
    auto *sourceBtn = new QPushButton("Create Source Scene", dlg);
    auto *looksBtn = new QPushButton("Create Sources + Looks", dlg);
    auto *repairBtn = new QPushButton("Repair Duplicates", dlg);
    auto *openSourcesBtn = new QPushButton("Open Source Scene", dlg);
    actions->addWidget(refreshBtn);
    actions->addWidget(sourceBtn);
    actions->addWidget(looksBtn);
    actions->addWidget(repairBtn);
    actions->addWidget(openSourcesBtn);
    actions->addStretch(1);
    root->addLayout(actions);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    grid->addWidget(new QLabel("Slot", dlg), 0, 0);
    grid->addWidget(new QLabel("OBS source", dlg), 0, 1);
    grid->addWidget(new QLabel("Zoom participant", dlg), 0, 2);
    grid->addWidget(new QLabel("Action", dlg), 0, 3);

    QStringList sources = sourceNamesForSlots(8);
    while (sources.size() < 8)
        sources << m_settingsPage->sourcePattern().arg(sources.size() + 1);

    QVector<QComboBox *> combos;
    combos.reserve(8);
    for (int i = 0; i < 8; ++i) {
        auto *slotLabel = new QLabel(QStringLiteral("Slot %1").arg(i + 1), dlg);
        auto *sourceLabel = new QLabel(sources.value(i), dlg);
        sourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        auto *combo = new QComboBox(dlg);
        combo->addItem("Unassigned", -1);
        for (const auto &participant : m_participants) {
            if (!participant.hasVideo)
                continue;
            combo->addItem(participant.name, participant.id);
            if (participant.slotAssign == i)
                combo->setCurrentIndex(combo->count() - 1);
        }
        combos << combo;

        auto *assignBtn = new QPushButton("Assign", dlg);
        connect(assignBtn, &QPushButton::clicked, dlg, [this, combo, i]() {
            const int participantId = combo->currentData().toInt();
            if (participantId >= 0)
                onSlotAssigned(i, participantId);
        });

        const int row = i + 1;
        grid->addWidget(slotLabel, row, 0);
        grid->addWidget(sourceLabel, row, 1);
        grid->addWidget(combo, row, 2);
        grid->addWidget(assignBtn, row, 3);
    }
    root->addLayout(grid);

    auto *footer = new QHBoxLayout;
    auto *applyBtn = new QPushButton("Apply Mapping", dlg);
    auto *closeBtn = new QPushButton("Close", dlg);
    footer->addStretch(1);
    footer->addWidget(applyBtn);
    footer->addWidget(closeBtn);
    root->addLayout(footer);

    connect(refreshBtn, &QPushButton::clicked, dlg, [this, refreshStatus]() {
        if (m_zoomClient) {
            m_zoomClient->refreshParticipants();
            m_zoomClient->refreshOutputs();
        }
        if (m_obsClient && m_obsClient->isConnected())
            m_obsClient->refreshInventory();
        refreshStatus();
    });
    connect(sourceBtn, &QPushButton::clicked, dlg, [this, refreshStatus]() {
        provisionPlaceholderSources();
        onObsLog("Requested CoreVideo Sources scene and 8 participant sources in OBS.");
        refreshStatus();
    });
    connect(looksBtn, &QPushButton::clicked, dlg, [this, refreshStatus]() {
        provisionLookScenes();
        refreshStatus();
    });
    connect(repairBtn, &QPushButton::clicked, dlg, [this, refreshStatus]() {
        repairCoreVideoDuplicates();
        refreshStatus();
    });
    connect(openSourcesBtn, &QPushButton::clicked, dlg, [this]() {
        provisionPlaceholderSources();
        QTimer::singleShot(500, this, [this]() {
            if (m_obsClient && m_obsClient->isConnected())
                m_obsClient->setCurrentScene(QStringLiteral("CoreVideo Sources"));
        });
    });
    connect(applyBtn, &QPushButton::clicked, dlg, [this, combos]() {
        for (int i = 0; i < combos.size(); ++i) {
            const int participantId = combos[i]->currentData().toInt();
            if (participantId >= 0)
                onSlotAssigned(i, participantId);
        }
    });
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);

    dlg->show();
}

void MainWindow::onPageSelected(Sidebar::Page p)
{
    using P = Sidebar::Page;
    switch (p) {
    case P::Templates:
    case P::Participants:
        m_rightStack->setCurrentIndex(m_pageTemplates);  break;
    case P::Themes:
        m_rightStack->setCurrentIndex(m_pageThemes);     break;
    case P::Scenes:
        m_rightStack->setCurrentIndex(m_pageScenes);
        m_obsClient->refreshInventory();
        break;
    case P::Overlays:
        m_rightStack->setCurrentIndex(m_pageOverlays);   break;
    case P::Macros:
        m_rightStack->setCurrentIndex(m_pageMacros);     break;
    case P::Settings:
        m_rightStack->setCurrentIndex(m_pageSettings);   break;
    }
}

void MainWindow::onTemplateSelected(const LayoutTemplate &tmpl)
{
    // Selecting a template stages it on PVW. It does NOT go on air until
    // the operator takes — that's the whole point of the M/E model.
    m_working.tmpl = tmpl;
    m_working.id   = tmpl.id;
    m_working.name = tmpl.name;
    m_working.templateId = tmpl.id;
    m_bus->stageLook(m_working);
    onObsLog(QStringLiteral("Staged on PVW: %1 — press TAKE to go on air.").arg(tmpl.name));
}

void MainWindow::onLookSelected(const Look &look)
{
    // Stage the full Look on PVW: layout + identity + (until overlays land
    // in a later slice) apply the Look's theme app-wide. Existing slot
    // assignments are carried over so swapping a Look doesn't blank the
    // participants the operator already placed.
    m_working            = lookWithCurrentAssignments(look);
    m_working.templateId = look.tmpl.id.isEmpty() ? look.templateId
                                                  : look.tmpl.id;
    m_bus->stageLook(m_working);

    if (!look.themeId.isEmpty()) {
        for (const auto &t : ShowTheme::builtIns()) {
            if (t.id == look.themeId) { onThemeSelected(t); break; }
        }
    }

    onObsLog(QStringLiteral("Staged Look on PVW: %1 — press TAKE to go on air.")
                 .arg(look.name));
    renderLookToOBS(m_working, false);
}

void MainWindow::onCreateLookRequested()
{
    if (!m_working.isValid()) {
        onObsLog("Create Look skipped: no staged layout.");
        return;
    }

    bool ok = false;
    const QString defaultName = m_working.name.isEmpty()
        ? QStringLiteral("Custom Look")
        : QStringLiteral("%1 Custom").arg(m_working.name);
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("New Look"),
                                               QStringLiteral("Look name"),
                                               QLineEdit::Normal,
                                               defaultName,
                                               &ok).trimmed();
    if (!ok || name.isEmpty())
        return;

    Look custom = m_working;
    custom.id = QStringLiteral("custom-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    custom.name = name;
    custom.category = QStringLiteral("Custom");
    custom.description = QStringLiteral("User-created look");
    custom.templateId = custom.tmpl.id.isEmpty() ? custom.templateId : custom.tmpl.id;

    m_customLooks.append(custom);
    saveCustomLooks();
    refreshLookPanel();
    onLookSelected(custom);
    onObsLog(QStringLiteral("Created custom Look: %1.").arg(custom.name));
}

void MainWindow::onSetBackgroundRequested()
{
    if (!m_working.isValid()) {
        onObsLog("Background skipped: no staged Look.");
        return;
    }

    const QString file = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Choose Look Background"),
        QString(),
        QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.webp);;All files (*.*)"));
    if (file.isEmpty())
        return;

    m_working.backgroundImagePath = file;
    m_bus->stageLook(m_working);

    bool updated = false;
    for (auto &look : m_customLooks) {
        if (look.id != m_working.id)
            continue;
        look.backgroundImagePath = file;
        look.slotAssignments = m_working.slotAssignments;
        look.overlays = m_working.overlays;
        updated = true;
        break;
    }
    if (updated) {
        saveCustomLooks();
        refreshLookPanel();
    } else {
        onObsLog("Background staged. Use New Look to save it as a reusable custom Look.");
    }

    renderLookToOBS(m_working, false);
}

void MainWindow::onDesignLookRequested()
{
    if (!m_working.isValid()) {
        onObsLog("Design skipped: no staged Look.");
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Look Designer"));
    dlg.resize(1120, 680);
    dlg.setStyleSheet(
        "QDialog { background: #141418; color: #f4f4ff; }"
        "QLabel { color: #d8d8ea; font-size: 12px; }"
        "QLabel#designerTitle { color: white; font-size: 18px; font-weight: 800; }"
        "QLabel#designerHint { color: #8d8da8; font-size: 11px; }"
        "QFrame#stage { background: #202028; border: 1px solid #323246; border-radius: 8px; }"
        "QFrame#inspector { background: #1a1a22; border-left: 1px solid #303044; }"
        "QDoubleSpinBox { background: #101018; color: #f0f0ff; border: 1px solid #34344a; border-radius: 4px; padding: 4px; }"
        "QPushButton { background: #262638; color: white; border: 1px solid #3a3a55; border-radius: 6px; padding: 7px 10px; }"
        "QPushButton:hover { background: #303050; }"
        "QCheckBox { color: #d8d8ea; spacing: 8px; }");

    auto *root = new QHBoxLayout(&dlg);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *stage = new QFrame(&dlg);
    stage->setObjectName("stage");
    auto *stageV = new QVBoxLayout(stage);
    stageV->setContentsMargins(24, 20, 24, 24);
    stageV->setSpacing(12);
    auto *title = new QLabel(m_working.name, stage);
    title->setObjectName("designerTitle");
    auto *hint = new QLabel(QStringLiteral("Design the reusable broadcast look; participant slots remain linked to shared OBS sources."), stage);
    hint->setObjectName("designerHint");
    auto *designerPreview = new PreviewCanvas(stage);
    designerPreview->setMinimumSize(720, 405);
    designerPreview->setTemplate(m_working.tmpl);
    designerPreview->setParticipants(participantsForLook(m_working));
    designerPreview->setOverlays(m_working.overlays);
    designerPreview->setBackgroundImage(m_working.backgroundImagePath);
    designerPreview->setTileStyle(m_working.tileStyle);
    stageV->addWidget(title);
    stageV->addWidget(hint);
    stageV->addWidget(designerPreview, 1);
    root->addWidget(stage, 1);

    auto *inspector = new QFrame(&dlg);
    inspector->setObjectName("inspector");
    inspector->setFixedWidth(320);
    auto *layout = new QVBoxLayout(inspector);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(10);
    auto *inspectorTitle = new QLabel(QStringLiteral("Gallery"), inspector);
    inspectorTitle->setObjectName("designerTitle");
    auto *inspectorHint = new QLabel(QStringLiteral("Canvas, tile, and participant display"), inspector);
    inspectorHint->setObjectName("designerHint");
    layout->addWidget(inspectorTitle);
    layout->addWidget(inspectorHint);

    auto *borderWidth = new QDoubleSpinBox(&dlg);
    borderWidth->setRange(0.0, 16.0);
    borderWidth->setSingleStep(0.5);
    borderWidth->setValue(m_working.tileStyle.borderWidth);
    auto *radius = new QDoubleSpinBox(&dlg);
    radius->setRange(0.0, 80.0);
    radius->setSingleStep(1.0);
    radius->setValue(m_working.tileStyle.cornerRadius);
    radius->setEnabled(false);
    radius->setToolTip(QStringLiteral("Rounded corners need an OBS mask/matte implementation before they can be WYSIWYG."));
    auto *opacity = new QDoubleSpinBox(&dlg);
    opacity->setRange(0.1, 1.0);
    opacity->setSingleStep(0.05);
    opacity->setValue(m_working.tileStyle.opacity);

    auto *shadow = new QCheckBox(QStringLiteral("Drop shadow"), &dlg);
    shadow->setChecked(m_working.tileStyle.dropShadow);
    auto *names = new QCheckBox(QStringLiteral("Show name tags"), &dlg);
    names->setChecked(m_working.tileStyle.showNameTag);
    auto *videoOnly = new QCheckBox(QStringLiteral("Exclude participants without video"), &dlg);
    videoOnly->setChecked(m_working.tileStyle.excludeNoVideo);

    QColor selectedCanvas = m_working.tileStyle.canvasColor;
    QColor selectedBorder = m_working.tileStyle.borderColor;
    auto *canvasColor = new QPushButton(selectedCanvas.name(), &dlg);
    connect(canvasColor, &QPushButton::clicked, &dlg, [&]() {
        const QColor c = QColorDialog::getColor(selectedCanvas, &dlg, QStringLiteral("Canvas Color"));
        if (!c.isValid()) return;
        selectedCanvas = c;
        canvasColor->setText(c.name());
        TileStyle s = m_working.tileStyle;
        s.canvasColor = selectedCanvas;
        designerPreview->setTileStyle(s);
    });
    auto *borderColor = new QPushButton(selectedBorder.name(), &dlg);
    connect(borderColor, &QPushButton::clicked, &dlg, [&]() {
        const QColor c = QColorDialog::getColor(selectedBorder, &dlg, QStringLiteral("Tile Border"));
        if (!c.isValid()) return;
        selectedBorder = c;
        borderColor->setText(c.name());
        TileStyle s = m_working.tileStyle;
        s.canvasColor = selectedCanvas;
        s.borderColor = selectedBorder;
        designerPreview->setTileStyle(s);
    });

    auto refreshDesignerPreview = [&]() {
        TileStyle s = m_working.tileStyle;
        s.canvasColor = selectedCanvas;
        s.borderColor = selectedBorder;
        s.borderWidth = borderWidth->value();
        s.cornerRadius = radius->value();
        s.opacity = opacity->value();
        s.dropShadow = shadow->isChecked();
        s.showNameTag = names->isChecked();
        s.excludeNoVideo = videoOnly->isChecked();
        designerPreview->setTileStyle(s);
    };
    connect(borderWidth, qOverload<double>(&QDoubleSpinBox::valueChanged), &dlg, refreshDesignerPreview);
    connect(radius, qOverload<double>(&QDoubleSpinBox::valueChanged), &dlg, refreshDesignerPreview);
    connect(opacity, qOverload<double>(&QDoubleSpinBox::valueChanged), &dlg, refreshDesignerPreview);
    connect(shadow, &QCheckBox::toggled, &dlg, refreshDesignerPreview);
    connect(names, &QCheckBox::toggled, &dlg, refreshDesignerPreview);
    connect(videoOnly, &QCheckBox::toggled, &dlg, refreshDesignerPreview);

    auto addRow = [&](const QString &label, QWidget *control) {
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(label, &dlg));
        row->addWidget(control);
        layout->addLayout(row);
    };
    addRow(QStringLiteral("Canvas color"), canvasColor);
    addRow(QStringLiteral("Border width"), borderWidth);
    addRow(QStringLiteral("Corner radius"), radius);
    addRow(QStringLiteral("Tile opacity"), opacity);
    addRow(QStringLiteral("Border color"), borderColor);
    layout->addWidget(shadow);
    layout->addWidget(names);
    layout->addWidget(videoOnly);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addStretch(1);
    layout->addWidget(buttons);
    root->addWidget(inspector);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;

    m_working.tileStyle.canvasColor = selectedCanvas;
    m_working.tileStyle.borderColor = selectedBorder;
    m_working.tileStyle.borderWidth = borderWidth->value();
    m_working.tileStyle.cornerRadius = radius->value();
    m_working.tileStyle.opacity = opacity->value();
    m_working.tileStyle.dropShadow = shadow->isChecked();
    m_working.tileStyle.showNameTag = names->isChecked();
    m_working.tileStyle.excludeNoVideo = videoOnly->isChecked();
    m_working = lookWithCurrentAssignments(m_working);
    m_bus->stageLook(m_working);

    for (auto &look : m_customLooks) {
        if (look.id != m_working.id)
            continue;
        look.tileStyle = m_working.tileStyle;
        look.slotAssignments = m_working.slotAssignments;
        saveCustomLooks();
        refreshLookPanel();
        break;
    }

    renderLookToOBS(m_working, false);
    onObsLog(QStringLiteral("Updated Look design: %1.").arg(m_working.name));
}

void MainWindow::onThemeSelected(const ShowTheme &theme)
{
    qApp->setStyleSheet(sidecar_stylesheet(&theme));
    if (m_liveCanvas)  m_liveCanvas->setAccent(theme.accent);
    if (m_sceneCanvas) m_sceneCanvas->setAccent(theme.accent);
    onObsLog(QStringLiteral("Theme applied: %1.").arg(theme.name));
}

void MainWindow::onTake()
{
    if (!m_working.isValid()) { onObsLog("Nothing staged on PVW."); return; }
    m_bus->take();
    onObsLog(QStringLiteral("TAKE → %1 on air.").arg(m_bus->program().name));
}

void MainWindow::onAuto()
{
    if (!m_working.isValid()) { onObsLog("Nothing staged on PVW."); return; }
    const int ms = m_autoDurationCombo
        ? m_autoDurationCombo->currentData().toInt()
        : 1500;
    m_bus->autoTake(ms);
    onObsLog(QStringLiteral("AUTO %1ms → %2 on air.")
                 .arg(ms).arg(m_bus->preview().name));
}

void MainWindow::onFTB()
{
    const bool wasActive = m_bus->isFTBActive();
    m_bus->ftbToggle(500);
    onObsLog(wasActive ? "FTB OFF — restoring program."
                       : "FTB ON — fading to black.");
}

void MainWindow::onSwapBuses()
{
    m_bus->swap();
    onObsLog("Swapped PGM ⇄ PVW (off-air only).");
}

void MainWindow::onApplyLayout()
{
    if (!m_currentTemplate.isValid()) { onObsLog("No template on PGM."); return; }
    if (!m_obsClient->isConnected())  { onObsLog("Not connected — preview only."); return; }

    renderLookToOBS(m_bus->program(), true);
}

void MainWindow::onRenderPreview()
{
    if (!m_working.isValid()) { onObsLog("Nothing staged on PVW."); return; }
    if (!m_obsClient || !m_obsClient->isConnected()) {
        onObsLog("Not connected to OBS; staged Look remains sidecar-only.");
        return;
    }

    renderLookToOBS(m_working, false);
}

void MainWindow::onEngineToggle()
{
    m_engineOn = !m_engineOn;
    m_engineBtn->setObjectName(m_engineOn ? "engineOnBtn" : "engineOffBtn");
    m_engineBtn->setText(m_engineOn ? "Scene Sync ON" : "Scene Sync OFF");
    m_engineBtn->style()->unpolish(m_engineBtn);
    m_engineBtn->style()->polish(m_engineBtn);
    if (m_engineOn) {
        if (m_obsClient && m_obsClient->isConnected())
            m_obsClient->refreshInventory();
        provisionLookScenes();
    }
    updateSceneSyncStatus();
}

void MainWindow::onObsConnect()
{
    if (m_obsClient->isConnected()
        || m_obsClient->state() == OBSClient::State::Connecting
        || m_obsClient->state() == OBSClient::State::Reconnecting) {
        m_obsClient->disconnectFromOBS();
        return;
    }

    // Pre-populate dialog from settings page values (if user has saved)
    OBSConnectDialog dlg(m_settingsPage->obsConfig(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    m_obsConfig = dlg.config();

    QSettings s;
    s.setValue("obs/host",          m_obsConfig.host);
    s.setValue("obs/port",          m_obsConfig.port);
    s.setValue("obs/password",      m_obsConfig.password);
    s.setValue("obs/autoReconnect", m_obsConfig.autoReconnect);

    m_obsClient->connectToOBS(m_obsConfig);
}

void MainWindow::onObsState(OBSClient::State s)
{
    m_obsStatusLabel->setText(m_obsClient->stateLabel());
    const bool ok = (s == OBSClient::State::Connected);
    m_obsBtn->setProperty("connected", ok ? "true" : "false");
    m_obsBtn->setText(ok ? "OBS  ●" : "OBS  ○");
    m_obsBtn->style()->unpolish(m_obsBtn);
    m_obsBtn->style()->polish(m_obsBtn);

    QString color = "#8080a0";
    switch (s) {
    case OBSClient::State::Connected:                          color = "#20c460"; break;
    case OBSClient::State::Connecting:
    case OBSClient::State::Authenticating:
    case OBSClient::State::Reconnecting:                       color = "#e0a020"; break;
    case OBSClient::State::Failed:                             color = "#e04040"; break;
    case OBSClient::State::Disconnected:                       color = "#8080a0"; break;
    }
    m_obsStatusLabel->setStyleSheet(
        QString("color: %1; font-size: 11px; background: transparent;").arg(color));
    updateSceneSyncStatus();
}

void MainWindow::updateSceneSyncStatus()
{
    if (!m_sceneSyncStatusLabel || !m_obsClient || !m_settingsPage)
        return;

    if (!m_obsClient->isConnected()) {
        m_obsSyncState = ObsSyncState::Offline;
        m_sceneSyncStatusLabel->setText("Sync offline");
        m_sceneSyncStatusLabel->setToolTip("OBS is not connected.");
        m_sceneSyncStatusLabel->setStyleSheet("color: #8080a0; font-size: 11px; background: transparent;");
        return;
    }

    if (m_obsSyncState == ObsSyncState::Error) {
        m_sceneSyncStatusLabel->setText(QStringLiteral("Sync error"));
        m_sceneSyncStatusLabel->setToolTip(m_lastSyncError.isEmpty()
            ? QStringLiteral("OBS reported a request failure.")
            : m_lastSyncError);
        m_sceneSyncStatusLabel->setStyleSheet("color: #e04040; font-size: 11px; background: transparent;");
        return;
    }

    if (m_obsSyncState == ObsSyncState::Applying) {
        m_sceneSyncStatusLabel->setText(QStringLiteral("Applying %1").arg(m_lastRenderedLookName.left(24)));
        m_sceneSyncStatusLabel->setToolTip(QStringLiteral("Rendering Look '%1' into OBS scene '%2'.")
                                           .arg(m_lastRenderedLookName, m_lastRenderedSceneName));
        m_sceneSyncStatusLabel->setStyleSheet("color: #e0a020; font-size: 11px; background: transparent;");
        return;
    }

    const auto status = m_obsClient->coreVideoSyncStatus(sourceNamesForSlots(8), lookSceneNames());
    QStringList missingDesignLayers;
    if (!m_lastRenderedSceneName.isEmpty() && !m_lastExpectedDesignLayers.isEmpty()) {
        QSet<QString> present;
        for (const QString &source : m_obsClient->sceneItemSourceNames(m_lastRenderedSceneName))
            present.insert(source);
        for (const QString &source : m_lastExpectedDesignLayers) {
            if (!present.contains(source))
                missingDesignLayers << source;
        }
    }
    const bool complete = status.inventoryReady
        && status.presentParticipantSources == status.expectedParticipantSources
        && status.presentSlotScenes == status.expectedSlotScenes
        && status.presentLookScenes == status.expectedLookScenes
        && !status.missingScenes.contains(QStringLiteral("CoreVideo Sources"))
        && missingDesignLayers.isEmpty();

    m_obsSyncState = complete ? ObsSyncState::Synced : ObsSyncState::Dirty;
    const QString stateLabel = complete ? QStringLiteral("Synced") : QStringLiteral("Dirty");

    m_sceneSyncStatusLabel->setText(QStringLiteral("%1  %2/%3 src  %4/%5 slots  %6/%7 looks")
        .arg(stateLabel)
        .arg(status.presentParticipantSources)
        .arg(status.expectedParticipantSources)
        .arg(status.presentSlotScenes)
        .arg(status.expectedSlotScenes)
        .arg(status.presentLookScenes)
        .arg(status.expectedLookScenes));

    m_sceneSyncStatusLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px; background: transparent;")
            .arg(complete ? QStringLiteral("#20c460") : QStringLiteral("#e0a020")));

    QStringList detail;
    if (!status.inventoryReady)
        detail << QStringLiteral("OBS inventory is still loading.");
    if (!status.missingInputs.isEmpty())
        detail << QStringLiteral("Missing inputs: %1").arg(status.missingInputs.join(", "));
    if (!status.missingScenes.isEmpty())
        detail << QStringLiteral("Missing scenes: %1").arg(status.missingScenes.join(", "));
    if (!missingDesignLayers.isEmpty())
        detail << QStringLiteral("Missing design layers: %1").arg(missingDesignLayers.join(", "));
    if (detail.isEmpty())
        detail << QStringLiteral("CoreVideo OBS scene graph is synced.");
    if (!m_lastRenderedLookName.isEmpty())
        detail.prepend(QStringLiteral("Last rendered Look: %1 (%2)")
                           .arg(m_lastRenderedLookName, m_lastRenderedSceneName));
    m_sceneSyncStatusLabel->setToolTip(detail.join("\n"));
}

void MainWindow::onObsLog(const QString &msg)
{
    if (!m_logView) return;
    m_logView->appendPlainText(
        QString("[%1] %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"), msg));
}

void MainWindow::onScenesReceived(const QStringList &scenes)
{
    m_lastScenes = scenes;
    m_scenesPanel->setScenes(scenes);
    updateSceneSyncStatus();

    const QString target = m_settingsPage->targetScene();
    if (scenes.contains(target)) {
        m_obsClient->requestSceneItems(target);
    } else if (!scenes.isEmpty()) {
        onObsLog(QStringLiteral("Scene '%1' not found. Available: %2")
                     .arg(target, scenes.join(", ")));
    }
}

void MainWindow::onSceneActivated(const QString &name)
{
    stageLookFromObsScene(name);
    m_obsClient->setCurrentScene(name);
    onObsLog(QStringLiteral("Switched to scene: %1").arg(name));
}

void MainWindow::onMacroTriggered(const Macro &macro)
{
    if (!m_obsClient->isConnected()) {
        onObsLog(QStringLiteral("Macro '%1' — not connected to OBS.").arg(macro.label));
        return;
    }
    m_obsClient->executeMacro(macro);
    onObsLog(QStringLiteral("Macro '%1' triggered (%2 steps).")
                 .arg(macro.label).arg(macro.steps.size()));
}

void MainWindow::onVirtualCamToggle()
{
    if (m_obsClient->isVirtualCamActive())
        m_obsClient->stopVirtualCam();
    else
        m_obsClient->startVirtualCam();
}

void MainWindow::onVirtualCamState(bool active)
{
    m_vcamBtn->setText(active ? "⏺  V-Cam ON" : "⏺  V-Cam OFF");
    m_vcamBtn->setProperty("primary", active ? "true" : "false");
    m_vcamBtn->style()->unpolish(m_vcamBtn);
    m_vcamBtn->style()->polish(m_vcamBtn);
}

void MainWindow::onSettingsChanged()
{
    // Re-read the saved OBS config so the next connect picks it up
    m_obsConfig = m_settingsPage->obsConfig();
    onObsLog("Settings saved.");
}

void MainWindow::onPhaseSelected(ShowPhase phase)
{
    const QStringList labels = {"pre_show", "live", "post_show"};
    m_controlServer->notifyPhaseChanged(labels[int(phase)]);
    m_phase = phase;
    m_preShowBtn->setChecked(phase == ShowPhase::PreShow);
    m_liveBtn->setChecked(phase == ShowPhase::Live);
    m_postShowBtn->setChecked(phase == ShowPhase::PostShow);

    // Highlight top bar border red during LIVE
    const QString border = (phase == ShowPhase::Live) ? "#cc2020" : "#181828";
    m_topBar->setStyleSheet(
        QString("#topBar { background: #0d0d12; border-bottom: 2px solid %1; }").arg(border));

    const QStringList humanLabels = {"PRE-SHOW", "LIVE", "POST-SHOW"};
    onObsLog(QString("Show phase → %1").arg(humanLabels[int(phase)]));
}

void MainWindow::onSlotAssigned(int slotIndex, int participantId)
{
    const QVector<int> participantSlots = participantSlotIndexesForLook(m_working);
    if (!participantSlots.contains(slotIndex)) {
        onObsLog(QStringLiteral("Slot %1 is not participant-mappable for Look '%2'.")
                     .arg(slotIndex + 1).arg(m_working.name));
        return;
    }

    // Evict the old occupant of this slot, then assign the new one
    for (auto &p : m_participants) {
        if (p.slotAssign == slotIndex)
            p.slotAssign = -1;
    }
    for (auto &p : m_participants) {
        if (p.id == participantId)
            p.slotAssign = slotIndex;
    }

    // Update the staged Look's slot assignments and re-stage on PVW. PGM
    // is unchanged until the operator takes.
    m_working.slotAssignments.erase(
        std::remove_if(m_working.slotAssignments.begin(), m_working.slotAssignments.end(),
                       [slotIndex, participantId](const SlotAssignment &s) {
                           return s.slotIndex == slotIndex
                               || s.participantId == participantId;
                       }),
        m_working.slotAssignments.end());
    m_working.slotAssignments.append({slotIndex, participantId});
    m_bus->stageLook(m_working);

    m_lastSyncedSlotParticipants.remove(slotIndex);
    syncZoomOutputAssignments();
    if (m_obsClient && m_obsClient->isConnected())
        renderLookToOBS(m_working, false);

    // Refresh panel to show updated slot labels
    m_participantPanel->setParticipants(m_participants);

    onObsLog(QString("Slot %1 ← %2")
             .arg(slotIndex + 1)
             .arg([&]() -> QString {
                 for (const auto &p : m_participants)
                     if (p.id == participantId) return p.name;
                 return QString::number(participantId);
             }()));
}

void MainWindow::onSlotClicked(int slotIndex)
{
    if (!participantSlotIndexesForLook(m_working).contains(slotIndex)) return;

    // Context-sensitive right panel: clicking a slot focuses the Templates
    // page (which hosts the participants list) and arms click-to-assign so
    // the user can pick a participant without dragging.
    m_assignTargetSlot = slotIndex;
    m_sidebar->setActivePage(Sidebar::Page::Templates);
    m_rightStack->setCurrentIndex(m_pageTemplates);
    m_participantPanel->setAssignTarget(slotIndex);
    onObsLog(QString("Selected Slot %1 — click a participant to assign.")
                 .arg(slotIndex + 1));
}

void MainWindow::onSlotRoutingCycle(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= 8) return;
    const AudioRouting next =
        nextAudioRouting(m_slotRouting.value(slotIndex, AudioRouting::Mixed));
    if (next == AudioRouting::Mixed)
        m_slotRouting.remove(slotIndex);
    else
        m_slotRouting.insert(slotIndex, next);

    // Repaint both canvases so the badge updates.
    if (m_liveCanvas)  m_liveCanvas->setSlotRouting(m_slotRouting);
    if (m_sceneCanvas) m_sceneCanvas->setSlotRouting(m_slotRouting);

    // Push the new routing to the plugin. Drop the slot from the sync cache
    // so the next call re-emits assignOutput even when the participant id
    // hasn't changed — without this the cache would short-circuit the resend.
    m_lastSyncedSlotParticipants.remove(slotIndex);
    syncZoomOutputAssignments();

    onObsLog(QString("Slot %1 audio routing → %2")
                 .arg(slotIndex + 1).arg(audioRoutingLabel(next)));
}

void MainWindow::onParticipantAssignClicked(int participantId)
{
    // assignRequested already handles the assign-mode path. This slot is
    // a hook for any future "select participant first" flow.
    Q_UNUSED(participantId);
}

void MainWindow::openCommandPalette()
{
    populateCommandPalette();
    m_commandPalette->run();
}

void MainWindow::populateCommandPalette()
{
    auto *cp = m_commandPalette;
    cp->clearCommands();

    // Navigation
    cp->addCommand("Go to Templates", "Navigate", [this]() {
        m_sidebar->setActivePage(Sidebar::Page::Templates);
        m_rightStack->setCurrentIndex(m_pageTemplates);
    });
    cp->addCommand("Go to Themes", "Navigate", [this]() {
        m_sidebar->setActivePage(Sidebar::Page::Themes);
        m_rightStack->setCurrentIndex(m_pageThemes);
    });
    cp->addCommand("Go to Scenes", "Navigate", [this]() {
        m_sidebar->setActivePage(Sidebar::Page::Scenes);
        m_rightStack->setCurrentIndex(m_pageScenes);
        m_obsClient->refreshInventory();
    });
    cp->addCommand("Go to Macros", "Navigate", [this]() {
        m_sidebar->setActivePage(Sidebar::Page::Macros);
        m_rightStack->setCurrentIndex(m_pageMacros);
    });
    cp->addCommand("Go to Settings", "Navigate", [this]() {
        m_sidebar->setActivePage(Sidebar::Page::Settings);
        m_rightStack->setCurrentIndex(m_pageSettings);
    });

    // Connection / toggles
    cp->addCommand(m_obsClient->isConnected() ? "Disconnect from OBS"
                                              : "Connect to OBS",
                   "OBS", [this]() { onObsConnect(); });
    cp->addCommand(m_obsClient->isVirtualCamActive() ? "Stop Virtual Camera"
                                                    : "Start Virtual Camera",
                   "OBS", [this]() { onVirtualCamToggle(); });
    cp->addCommand("TAKE (commit PVW → PGM)", "Switcher",
                   [this]() { onTake(); });
    cp->addCommand("AUTO transition (current duration)", "Switcher",
                   [this]() { onAuto(); });
    cp->addCommand("AUTO 0.5s", "Switcher",
                   [this]() { m_bus->autoTake(500); });
    cp->addCommand("AUTO 1.0s", "Switcher",
                   [this]() { m_bus->autoTake(1000); });
    cp->addCommand("AUTO 2.0s", "Switcher",
                   [this]() { m_bus->autoTake(2000); });
    cp->addCommand("AUTO 5.0s", "Switcher",
                   [this]() { m_bus->autoTake(5000); });
    cp->addCommand(m_bus->isFTBActive() ? "FTB OFF — restore program"
                                        : "FTB ON — fade to black",
                   "Switcher", [this]() { onFTB(); });
    cp->addCommand("SWAP buses (PGM ⇄ PVW)", "Switcher",
                   [this]() { onSwapBuses(); });

    // Overlays — fire / clear on PVW
    for (const auto &ov : Overlay::builtInPresets()) {
        const Overlay snap = ov;
        cp->addCommand(QString("Stage overlay: %1").arg(Overlay::humanLabel(ov)),
                       "Overlay", [this, snap]() {
            m_working.overlays.append(snap);
            m_bus->stageLook(m_working);
            if (m_overlayPanel) m_overlayPanel->setActiveOverlays(m_working.overlays);
        });
    }
    cp->addCommand("Clear PVW overlays", "Overlay", [this]() {
        m_working.overlays.clear();
        m_bus->stageLook(m_working);
        if (m_overlayPanel) m_overlayPanel->setActiveOverlays(m_working.overlays);
    });
    cp->addCommand("Re-apply current PGM to OBS", "OBS",
                   [this]() { onApplyLayout(); });
    cp->addCommand("Repair CoreVideo duplicate OBS scenes", "OBS",
                   [this]() { repairCoreVideoDuplicates(); });

    // Phase
    cp->addCommand("Phase: Pre-show", "Phase",
                   [this]() { onPhaseSelected(ShowPhase::PreShow); });
    cp->addCommand("Phase: Live", "Phase",
                   [this]() { onPhaseSelected(ShowPhase::Live); });
    cp->addCommand("Phase: Post-show", "Phase",
                   [this]() { onPhaseSelected(ShowPhase::PostShow); });

    // Scenes (from last OBS sync)
    for (const QString &s : m_lastScenes) {
        const QString sc = s;
        cp->addCommand(QString("Switch to scene: %1").arg(sc), "Scene",
                       [this, sc]() { onSceneActivated(sc); });
    }

    // Looks (broadcast-ready presets) — stage or take in one keystroke
    for (const auto &lk : allLooks()) {
        const Look look = lk;
        const QString name = lk.name;
        cp->addCommand(QString("Stage Look on PVW: %1").arg(name), "Look",
                       [this, look]() {
            onLookSelected(look);
        });
    }
    for (const auto &lk : allLooks()) {
        const Look look = lk;
        const QString name = lk.name;
        cp->addCommand(QString("Take Look to PGM: %1").arg(name), "Look",
                       [this, look]() {
            onLookSelected(look);
            onTake();
        });
    }

    // Templates
    for (const auto &t : TemplateManager::instance().templates()) {
        const QString id = t.id;
        const QString name = t.name;
        cp->addCommand(QString("Stage on PVW: %1").arg(name), "Template",
                       [this, id]() {
            if (const auto *tt = TemplateManager::instance().findById(id))
                onTemplateSelected(*tt);
        });
    }
    // Stage + TAKE in one step
    for (const auto &t : TemplateManager::instance().templates()) {
        const QString id = t.id;
        const QString name = t.name;
        cp->addCommand(QString("Take to PGM: %1").arg(name), "Template",
                       [this, id]() {
            if (const auto *tt = TemplateManager::instance().findById(id)) {
                onTemplateSelected(*tt);
                onTake();
            }
        });
    }

    // Themes
    for (const auto &t : ShowTheme::builtIns()) {
        const ShowTheme theme = t;
        cp->addCommand(QString("Theme: %1").arg(theme.name), "Theme",
                       [this, theme]() { onThemeSelected(theme); });
    }

    // Participants — assign to currently selected slot (or slot 1 fallback)
    for (const auto &p : m_participants) {
        const int pid = p.id;
        const QString name = p.name;
        cp->addCommand(QString("Assign %1 to current slot").arg(name),
                       "Participant", [this, pid]() {
            int slot = m_assignTargetSlot;
            if (slot < 0) slot = 0;
            if (slot < m_working.tmpl.slotList.size())
                onSlotAssigned(slot, pid);
        });
    }
}
