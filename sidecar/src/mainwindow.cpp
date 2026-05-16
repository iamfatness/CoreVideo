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
#include "command-palette.h"
#include "sidecar-style.h"
#include <QShortcut>
#include <QKeySequence>
#include <QTimer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QPlainTextEdit>
#include <QDockWidget>
#include <QStackedWidget>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QDateTime>
#include <QApplication>
#include <QStyle>

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
    toolbar->setObjectName("toolbar");
    toolbar->setFixedHeight(52);
    toolbar->setStyleSheet("#toolbar { background: #101016; border-top: 1px solid #1e1e2e; }");
    auto *tbRow = new QHBoxLayout(toolbar);
    tbRow->setContentsMargins(12, 0, 12, 0);
    tbRow->setSpacing(8);

    auto makeBtn = [&](const QString &label, bool primary = false) -> QPushButton * {
        auto *btn = new QPushButton(label, toolbar);
        btn->setObjectName("toolBtn");
        btn->setFixedHeight(34);
        if (primary) btn->setProperty("primary", "true");
        return btn;
    };

    m_swapBtn       = makeBtn("⇄  SWAP");
    m_takeBtn       = makeBtn("⏵  TAKE", true);
    auto *spotBtn   = makeBtn("◉  Spotlight");
    auto *lowerBtn  = makeBtn("▼  Lower Thirds");
    m_vcamBtn       = makeBtn("⏺  V-Cam OFF");
    auto *streamBtn = makeBtn("⏩  Stream");

    m_takeBtn->setFixedWidth(110);
    m_swapBtn->setFixedWidth(90);

    tbRow->addWidget(m_swapBtn);
    tbRow->addWidget(m_takeBtn);
    tbRow->addSpacing(12);
    tbRow->addWidget(spotBtn);
    tbRow->addWidget(lowerBtn);
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
        if (m_overlayPanel) m_overlayPanel->setActiveOverlays(l.overlays);
    });
    connect(m_bus, &MEBus::programChanged, this, [this](const Look &l) {
        m_liveCanvas->setTemplate(l.tmpl);
        m_liveCanvas->setParticipants(participantsForLook(l));
        m_liveCanvas->setOverlays(l.overlays);
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
    connect(m_swapBtn, &QPushButton::clicked,  this, &MainWindow::onSwapBuses);
    connect(m_engineBtn, &QPushButton::clicked, this, &MainWindow::onEngineToggle);
    connect(m_obsBtn,  &QPushButton::clicked,  this, &MainWindow::onObsConnect);
    connect(m_vcamBtn, &QPushButton::clicked,  this, &MainWindow::onVirtualCamToggle);

    // Spacebar = TAKE
    auto *takeShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
    takeShortcut->setContext(Qt::ApplicationShortcut);
    connect(takeShortcut, &QShortcut::activated, this, &MainWindow::onTake);

    m_obsClient = new OBSClient(this);
    connect(m_obsClient, &OBSClient::stateChanged,       this, &MainWindow::onObsState);
    connect(m_obsClient, &OBSClient::log,                this, &MainWindow::onObsLog);
    connect(m_obsClient, &OBSClient::scenesReceived,     this, &MainWindow::onScenesReceived);
    connect(m_obsClient, &OBSClient::sceneChanged,       this, [this](const QString &name) {
        m_scenesPanel->setCurrentScene(name);
    });
    connect(m_obsClient, &OBSClient::virtualCamStateChanged, this, &MainWindow::onVirtualCamState);
    connect(m_obsClient, &OBSClient::templateApplied,    this,
            [this](const QString &name, int n) {
                onObsLog(QStringLiteral("✓ Applied '%1' (%2 items).").arg(name).arg(n));
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

    // Canvas slot assignment from drag-and-drop
    connect(m_liveCanvas,  &PreviewCanvas::slotAssigned, this, &MainWindow::onSlotAssigned);
    connect(m_sceneCanvas, &PreviewCanvas::slotAssigned, this, &MainWindow::onSlotAssigned);

    // Click-to-assign — selecting a slot focuses the Templates page and arms
    // the participant panel so the next card click fills that slot.
    connect(m_liveCanvas,  &PreviewCanvas::slotClicked, this, &MainWindow::onSlotClicked);
    connect(m_sceneCanvas, &PreviewCanvas::slotClicked, this, &MainWindow::onSlotClicked);

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
    m_lookPanel->loadLooks(ll.looks());

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

    m_engineBtn = new QPushButton("⏻  Engine OFF", m_topBar);
    m_engineBtn->setObjectName("engineOffBtn");
    m_engineBtn->setFixedHeight(34);

    m_obsBtn = new QPushButton("OBS  ○", m_topBar);
    m_obsBtn->setObjectName("obsBtn");
    m_obsBtn->setFixedHeight(34);

    row->addWidget(m_showNameLabel);
    row->addStretch(1);
    row->addWidget(phaseBar);
    row->addWidget(m_obsStatusLabel);
    row->addSpacing(4);
    row->addWidget(m_obsBtn);
    row->addWidget(m_engineBtn);
}

// ── Center canvas area ────────────────────────────────────────────────────────
void MainWindow::buildCenterArea(QWidget *parent)
{
    m_canvasArea = new QWidget(parent);
    auto *row = new QHBoxLayout(m_canvasArea);
    row->setContentsMargins(12, 12, 12, 8);
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
        v->addWidget(outLbl);
        v->addWidget(out, 1);
        return wrap;
    };

    // PVW on the left (where you build), PGM on the right (what's on air) —
    // matches the visual flow "stage → take → on air."
    row->addWidget(buildPane("PVW  ◉  PREVIEW", "#20c460", m_sceneCanvas, m_pvwLabel), 1);
    row->addWidget(buildPane("PGM  ●  ON AIR",  "#ff4040", m_liveCanvas,  m_pgmLabel), 1);
}

QVector<PreviewCanvas::Participant>
MainWindow::participantsForLook(const Look &look) const
{
    const int nSlots = look.tmpl.slotList.size();
    QVector<PreviewCanvas::Participant> cp(nSlots);
    for (const auto &s : look.slots) {
        if (s.slotIndex < 0 || s.slotIndex >= nSlots) continue;
        for (const auto &p : m_participants) {
            if (p.id == s.participantId) {
                cp[s.slotIndex] = {p.name, p.initials, p.color, p.isTalking, p.hasVideo};
                break;
            }
        }
    }
    return cp;
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
    tmplV->addWidget(m_lookPanel, 1);
    auto *div = new QFrame(tmplPage);
    div->setFrameShape(QFrame::HLine);
    div->setStyleSheet("color: #1e1e2e;");
    tmplV->addWidget(div);
    m_participantPanel = new ParticipantPanel(tmplPage);
    tmplV->addWidget(m_participantPanel, 1);
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
        m_obsClient->requestSceneList();
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
    m_logDock->setWidget(m_logView);
    addDockWidget(Qt::BottomDockWidgetArea, m_logDock);
    m_logDock->hide();
}

// ── Mock data ─────────────────────────────────────────────────────────────────
void MainWindow::loadMockParticipants()
{
    m_participants = {
        {1, "Alex Rivera",   "AR", QColor(0x1e, 0x6a, 0xe0), false, true,  0},
        {2, "Sam Chen",      "SC", QColor(0x20, 0xa0, 0x60), false, false, 1},
        {3, "Jordan Kim",    "JK", QColor(0x9b, 0x40, 0xd0), false, true,  2},
        {4, "Taylor Brooks", "TB", QColor(0xe0, 0x60, 0x20), false, false, 3},
    };
    m_participantPanel->setParticipants(m_participants);

    // Seed the working Look's slot assignments from each participant's
    // default slotAssign so first-paint shows people in slots.
    m_working.slots.clear();
    for (const auto &p : m_participants) {
        if (p.slotAssign >= 0)
            m_working.slots.append({p.slotAssign, p.id});
    }
    if (m_bus) m_bus->stageLook(m_working);
}

// ── Slots ─────────────────────────────────────────────────────────────────────
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
        m_obsClient->requestSceneList();
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
    Look staged          = look;
    staged.slots         = m_working.slots;
    m_working            = staged;
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

void MainWindow::onSwapBuses()
{
    m_bus->swap();
    onObsLog("Swapped PGM ⇄ PVW (off-air only).");
}

void MainWindow::onApplyLayout()
{
    if (!m_currentTemplate.isValid()) { onObsLog("No template on PGM."); return; }
    if (!m_obsClient->isConnected())  { onObsLog("Not connected — preview only."); return; }

    const QString scene   = m_settingsPage->targetScene();
    const double  canvasW = m_settingsPage->canvasWidth();
    const double  canvasH = m_settingsPage->canvasHeight();
    const QString pattern = m_settingsPage->sourcePattern();

    // Try bundled applied-JSON first
    QFile f(QString(":/applied/data/applied/%1.applied.json").arg(m_currentTemplate.id));
    if (f.open(QIODevice::ReadOnly)) {
        QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
        obj["scene"] = scene;
        m_obsClient->applyTemplate(obj);
        return;
    }

    // Fall back to normalized layout
    QStringList sources;
    for (int i = 0; i < m_currentTemplate.slotList.size(); ++i)
        sources << pattern.arg(i + 1);
    m_obsClient->applyLayout(scene, m_currentTemplate, sources, canvasW, canvasH);
}

void MainWindow::onEngineToggle()
{
    m_engineOn = !m_engineOn;
    m_engineBtn->setObjectName(m_engineOn ? "engineOnBtn" : "engineOffBtn");
    m_engineBtn->setText(m_engineOn ? "⏻  Engine ON" : "⏻  Engine OFF");
    m_engineBtn->style()->unpolish(m_engineBtn);
    m_engineBtn->style()->polish(m_engineBtn);
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
}

void MainWindow::onObsLog(const QString &msg)
{
    if (!m_logView) return;
    m_logView->appendPlainText(
        QString("[%1] %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"), msg));
    if (!m_logDock->isVisible()) m_logDock->show();
}

void MainWindow::onScenesReceived(const QStringList &scenes)
{
    m_lastScenes = scenes;
    m_scenesPanel->setScenes(scenes);

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
    m_working.slots.erase(
        std::remove_if(m_working.slots.begin(), m_working.slots.end(),
                       [slotIndex, participantId](const SlotAssignment &s) {
                           return s.slotIndex == slotIndex
                               || s.participantId == participantId;
                       }),
        m_working.slots.end());
    m_working.slots.append({slotIndex, participantId});
    m_bus->stageLook(m_working);

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
    if (slotIndex < 0 || slotIndex >= m_working.tmpl.slotList.size()) return;

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
        m_obsClient->requestSceneList();
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
    for (const auto &lk : LookLibrary::instance().looks()) {
        const QString id   = lk.id;
        const QString name = lk.name;
        cp->addCommand(QString("Stage Look on PVW: %1").arg(name), "Look",
                       [this, id]() {
            if (const auto *l = LookLibrary::instance().findById(id))
                onLookSelected(*l);
        });
    }
    for (const auto &lk : LookLibrary::instance().looks()) {
        const QString id   = lk.id;
        const QString name = lk.name;
        cp->addCommand(QString("Take Look to PGM: %1").arg(name), "Look",
                       [this, id]() {
            if (const auto *l = LookLibrary::instance().findById(id)) {
                onLookSelected(*l);
                onTake();
            }
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
