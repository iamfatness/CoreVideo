#include "mainwindow.h"
#include "preview-canvas.h"
#include "template-panel.h"
#include "participant-panel.h"
#include "template-manager.h"
#include "obs-connect-dialog.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QPlainTextEdit>
#include <QDockWidget>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QDateTime>
#include <QStyle>

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

    // ── Sidebar ───────────────────────────────────────────────────────────────
    auto *sidebar = new Sidebar(this);
    sidebar->setFixedWidth(200);
    rootH->addWidget(sidebar);

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

    auto makeToolBtn = [&](const QString &label, bool primary = false) -> QPushButton * {
        auto *btn = new QPushButton(label, toolbar);
        btn->setObjectName("toolBtn");
        btn->setFixedHeight(34);
        if (primary) btn->setProperty("primary", "true");
        return btn;
    };

    auto *applyBtn  = makeToolBtn("⊞  Apply Template", true);
    auto *spotBtn   = makeToolBtn("◉  Spotlight Speaker");
    auto *lowerBtn  = makeToolBtn("▼  Lower Thirds");
    auto *recBtn    = makeToolBtn("⏺  Record");
    auto *streamBtn = makeToolBtn("⏩  Stream");

    tbRow->addWidget(applyBtn);
    tbRow->addWidget(spotBtn);
    tbRow->addWidget(lowerBtn);
    tbRow->addStretch(1);
    tbRow->addWidget(recBtn);
    tbRow->addWidget(streamBtn);

    centerV->addWidget(toolbar);
    rootH->addWidget(centerCol, 1);

    // ── Right panel ───────────────────────────────────────────────────────────
    auto *rightPanel = new QWidget(central);
    rightPanel->setObjectName("rightPanel");
    rightPanel->setFixedWidth(272);
    buildRightPanel(rightPanel);
    rootH->addWidget(rightPanel);

    // ── Log dock ──────────────────────────────────────────────────────────────
    buildLogDock();

    // ── Restore saved OBS config ──────────────────────────────────────────────
    QSettings s;
    m_obsConfig.host          = s.value("obs/host", "localhost").toString();
    m_obsConfig.port          = s.value("obs/port", 4455).toInt();
    m_obsConfig.password      = s.value("obs/password", "").toString();
    m_obsConfig.autoReconnect = s.value("obs/autoReconnect", true).toBool();

    // ── Connections ───────────────────────────────────────────────────────────
    connect(sidebar,     &Sidebar::pageSelected, this, &MainWindow::onPageSelected);
    connect(applyBtn,    &QPushButton::clicked,  this, &MainWindow::onApplyLayout);
    connect(m_engineBtn, &QPushButton::clicked,  this, &MainWindow::onEngineToggle);
    connect(m_obsBtn,    &QPushButton::clicked,  this, &MainWindow::onObsConnect);

    // OBS client
    m_obsClient = new OBSClient(this);
    connect(m_obsClient, &OBSClient::stateChanged,    this, &MainWindow::onObsState);
    connect(m_obsClient, &OBSClient::log,             this, &MainWindow::onObsLog);
    connect(m_obsClient, &OBSClient::scenesReceived,  this, &MainWindow::onScenesReceived);
    connect(m_obsClient, &OBSClient::templateApplied, this,
            [this](const QString &name, int n) {
                onObsLog(QStringLiteral("✓ Applied '%1' (%2 items).").arg(name).arg(n));
            });

    // Templates
    auto &tm = TemplateManager::instance();
    tm.loadBuiltIn();
    m_templatePanel->loadTemplates(tm.templates());

    if (const auto *grid4 = tm.findById("4-up-grid")) onTemplateSelected(*grid4);

    loadMockParticipants();
    onObsState(OBSClient::State::Disconnected);
    onObsLog("Sidecar ready. Click 'OBS' in the top bar to connect.");
}

// ── Top bar ───────────────────────────────────────────────────────────────────
void MainWindow::buildTopBar(QWidget *parent)
{
    m_topBar = new QWidget(parent);
    m_topBar->setObjectName("topBar");
    m_topBar->setFixedHeight(52);

    auto *row = new QHBoxLayout(m_topBar);
    row->setContentsMargins(16, 0, 12, 0);
    row->setSpacing(12);

    m_showNameLabel = new QLabel("Morning Show — Episode 142", m_topBar);
    m_showNameLabel->setStyleSheet("color: #e0e0f0; font-size: 15px; font-weight: 700;");

    m_obsStatusLabel = new QLabel("Disconnected", m_topBar);
    m_obsStatusLabel->setStyleSheet("color: #8080a0; font-size: 11px; background: transparent;");

    m_engineBtn = new QPushButton("⏻  Engine OFF", m_topBar);
    m_engineBtn->setObjectName("engineOffBtn");
    m_engineBtn->setFixedHeight(34);

    m_obsBtn = new QPushButton("OBS  ○", m_topBar);
    m_obsBtn->setObjectName("obsBtn");
    m_obsBtn->setFixedHeight(34);

    row->addWidget(m_showNameLabel, 1);
    row->addWidget(m_obsStatusLabel);
    row->addSpacing(6);
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

    m_templatePanel = new TemplatePanel(parent);
    connect(m_templatePanel, &TemplatePanel::templateSelected,
            this, &MainWindow::onTemplateSelected);
    vl->addWidget(m_templatePanel);

    auto *div = new QFrame(parent);
    div->setFrameShape(QFrame::HLine);
    div->setStyleSheet("color: #1e1e2e;");
    vl->addWidget(div);

    m_participantPanel = new ParticipantPanel(parent);
    vl->addWidget(m_participantPanel, 1);
}

// ── Log dock ──────────────────────────────────────────────────────────────────
void MainWindow::buildLogDock()
{
    m_logDock = new QDockWidget("OBS Log", this);
    m_logDock->setFeatures(QDockWidget::DockWidgetClosable |
                           QDockWidget::DockWidgetMovable);
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
    QVector<ParticipantInfo> parts = {
        {1, "Alex Rivera",   "AR", QColor(0x1e, 0x6a, 0xe0), false, true,  0},
        {2, "Sam Chen",      "SC", QColor(0x20, 0xa0, 0x60), false, false, 1},
        {3, "Jordan Kim",    "JK", QColor(0x9b, 0x40, 0xd0), false, true,  2},
        {4, "Taylor Brooks", "TB", QColor(0xe0, 0x60, 0x20), false, false, 3},
    };
    m_participantPanel->setParticipants(parts);

    QVector<PreviewCanvas::Participant> canvasParts;
    for (const auto &p : parts)
        canvasParts.append({p.name, p.initials, p.color, p.isTalking, p.hasVideo});
    m_liveCanvas->setParticipants(canvasParts);
    m_sceneCanvas->setParticipants(canvasParts);
}

// ── Slots ─────────────────────────────────────────────────────────────────────
void MainWindow::onPageSelected(Sidebar::Page) { /* future */ }

void MainWindow::onTemplateSelected(const LayoutTemplate &tmpl)
{
    m_currentTemplate = tmpl;
    m_liveCanvas->setTemplate(tmpl);
    m_sceneCanvas->setTemplate(tmpl);
}

void MainWindow::onApplyLayout()
{
    if (!m_currentTemplate.isValid()) {
        onObsLog("No template selected.");
        return;
    }
    if (!m_obsClient->isConnected()) {
        onObsLog("Not connected to OBS — preview-only apply.");
        return;
    }

    // Try the flat applied-template JSON first if one ships with the template
    const QString applied = QString(":/applied/data/applied/%1.applied.json")
                                .arg(m_currentTemplate.id);
    QFile f(applied);
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
        // Override scene with our active target so the user can retarget without
        // editing JSON.
        QJsonObject withScene = obj;
        if (!m_targetScene.isEmpty()) withScene["scene"] = m_targetScene;
        m_obsClient->applyTemplate(withScene);
        return;
    }

    // Fall back to normalized layout
    QStringList sources;
    for (int i = 0; i < m_currentTemplate.slots.size(); ++i)
        sources << QString("Zoom Participant %1").arg(i + 1);
    m_obsClient->applyLayout(m_targetScene, m_currentTemplate, sources, 1920, 1080);
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

    OBSConnectDialog dlg(m_obsConfig, this);
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
    case OBSClient::State::Connected:    color = "#20c460"; break;
    case OBSClient::State::Connecting:
    case OBSClient::State::Authenticating:
    case OBSClient::State::Reconnecting: color = "#e0a020"; break;
    case OBSClient::State::Failed:       color = "#e04040"; break;
    case OBSClient::State::Disconnected: color = "#8080a0"; break;
    }
    m_obsStatusLabel->setStyleSheet(
        QString("color: %1; font-size: 11px; background: transparent;").arg(color));
}

void MainWindow::onObsLog(const QString &msg)
{
    if (!m_logView) return;
    const QString line = QString("[%1] %2")
        .arg(QDateTime::currentDateTime().toString("HH:mm:ss"), msg);
    m_logView->appendPlainText(line);
    if (!m_logDock->isVisible()) m_logDock->show();
}

void MainWindow::onScenesReceived(const QStringList &scenes)
{
    if (scenes.contains(m_targetScene)) {
        m_obsClient->requestSceneItems(m_targetScene);
    } else if (!scenes.isEmpty()) {
        onObsLog(QStringLiteral("Target scene '%1' not found. Available: %2")
                     .arg(m_targetScene, scenes.join(", ")));
    }
}
