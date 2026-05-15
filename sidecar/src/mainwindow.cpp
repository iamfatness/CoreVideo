#include "mainwindow.h"
#include "preview-canvas.h"
#include "template-panel.h"
#include "theme-panel.h"
#include "participant-panel.h"
#include "scenes-panel.h"
#include "macros-panel.h"
#include "template-manager.h"
#include "settings-page.h"
#include "obs-connect-dialog.h"
#include "sidecar-style.h"
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

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
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

    auto *applyBtn  = makeBtn("⊞  Apply Template", true);
    auto *spotBtn   = makeBtn("◉  Spotlight");
    auto *lowerBtn  = makeBtn("▼  Lower Thirds");
    m_vcamBtn       = makeBtn("⏺  V-Cam OFF");
    auto *streamBtn = makeBtn("⏩  Stream");

    tbRow->addWidget(applyBtn);
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

    // ── Connections ───────────────────────────────────────────────────────────
    connect(m_sidebar, &Sidebar::pageSelected, this, &MainWindow::onPageSelected);
    connect(applyBtn,  &QPushButton::clicked,  this, &MainWindow::onApplyLayout);
    connect(m_engineBtn, &QPushButton::clicked, this, &MainWindow::onEngineToggle);
    connect(m_obsBtn,  &QPushButton::clicked,  this, &MainWindow::onObsConnect);
    connect(m_vcamBtn, &QPushButton::clicked,  this, &MainWindow::onVirtualCamToggle);

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
    connect(m_controlServer, &SidecarControlServer::templateApplyRequested,
            this, [this](const QString &id) {
        auto &tm = TemplateManager::instance();
        if (const auto *t = tm.findById(id)) {
            onTemplateSelected(*t);
            onApplyLayout();
        }
    });
    connect(m_controlServer, &SidecarControlServer::sceneChangeRequested,
            this, [this](const QString &scene) {
        onSceneActivated(scene);
    });

    // Canvas slot assignment from drag-and-drop
    connect(m_liveCanvas,  &PreviewCanvas::slotAssigned, this, &MainWindow::onSlotAssigned);
    connect(m_sceneCanvas, &PreviewCanvas::slotAssigned, this, &MainWindow::onSlotAssigned);

    // Templates
    auto &tm = TemplateManager::instance();
    tm.loadBuiltIn();
    m_templatePanel->loadTemplates(tm.templates());
    if (const auto *grid4 = tm.findById("4-up-grid")) onTemplateSelected(*grid4);

    // Themes
    m_themePanel->loadThemes(ShowTheme::builtIns());

    // Mock data
    loadMockParticipants();

    // Initial UI state
    onObsState(OBSClient::State::Disconnected);
    onObsLog("Sidecar ready. Click 'OBS' to connect.");
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
                         PreviewCanvas *&out) {
        auto *wrap = new QWidget(m_canvasArea);
        auto *v = new QVBoxLayout(wrap);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);
        auto *lbl = new QLabel(label, wrap);
        lbl->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: 800; "
                                   "letter-spacing: 0.1em; background: transparent;")
                               .arg(color));
        out = new PreviewCanvas(wrap);
        v->addWidget(lbl);
        v->addWidget(out, 1);
        return wrap;
    };

    row->addWidget(buildPane("LIVE",          "#ff4040", m_liveCanvas),  1);
    row->addWidget(buildPane("SCENE PREVIEW", "#5080ff", m_sceneCanvas), 1);
}

// ── Right panel ───────────────────────────────────────────────────────────────
void MainWindow::buildRightPanel(QWidget *parent)
{
    auto *vl = new QVBoxLayout(parent);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    m_rightStack = new QStackedWidget(parent);

    // Page 0: Templates + Participants (combined scroll)
    auto *tmplPage = new QWidget;
    auto *tmplV = new QVBoxLayout(tmplPage);
    tmplV->setContentsMargins(0, 0, 0, 0);
    tmplV->setSpacing(0);
    m_templatePanel = new TemplatePanel(tmplPage);
    connect(m_templatePanel, &TemplatePanel::templateSelected,
            this, &MainWindow::onTemplateSelected);
    tmplV->addWidget(m_templatePanel);
    auto *div = new QFrame(tmplPage);
    div->setFrameShape(QFrame::HLine);
    div->setStyleSheet("color: #1e1e2e;");
    tmplV->addWidget(div);
    m_participantPanel = new ParticipantPanel(tmplPage);
    tmplV->addWidget(m_participantPanel, 1);
    m_pageTemplates = m_rightStack->addWidget(tmplPage);

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

    QVector<PreviewCanvas::Participant> cp;
    for (const auto &p : m_participants)
        cp.append({p.name, p.initials, p.color, p.isTalking, p.hasVideo});
    m_liveCanvas->setParticipants(cp);
    m_sceneCanvas->setParticipants(cp);
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
    case P::Macros:
        m_rightStack->setCurrentIndex(m_pageMacros);     break;
    case P::Settings:
        m_rightStack->setCurrentIndex(m_pageSettings);   break;
    }
}

void MainWindow::onTemplateSelected(const LayoutTemplate &tmpl)
{
    m_currentTemplate = tmpl;
    m_controlServer->notifyTemplateChanged(tmpl.id, tmpl.name);
    m_liveCanvas->setTemplate(tmpl);
    m_sceneCanvas->setTemplate(tmpl);
}

void MainWindow::onThemeSelected(const ShowTheme &theme)
{
    qApp->setStyleSheet(sidecar_stylesheet(&theme));
    onObsLog(QStringLiteral("Theme applied: %1.").arg(theme.name));
}

void MainWindow::onApplyLayout()
{
    if (!m_currentTemplate.isValid()) { onObsLog("No template selected."); return; }
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
    for (int i = 0; i < m_currentTemplate.slots.size(); ++i)
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

    const QStringList labels = {"PRE-SHOW", "LIVE", "POST-SHOW"};
    onObsLog(QString("Show phase → %1").arg(labels[int(phase)]));
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

    // Rebuild ordered participant list for canvas (slot index → position)
    const int nSlots = m_currentTemplate.slots.size();
    QVector<PreviewCanvas::Participant> cp(nSlots);
    for (const auto &p : m_participants) {
        if (p.slotAssign >= 0 && p.slotAssign < nSlots)
            cp[p.slotAssign] = {p.name, p.initials, p.color, p.isTalking, p.hasVideo};
    }
    m_liveCanvas->setParticipants(cp);
    m_sceneCanvas->setParticipants(cp);

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
