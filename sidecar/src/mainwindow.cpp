#include "mainwindow.h"
#include "preview-canvas.h"
#include "template-panel.h"
#include "participant-panel.h"
#include "template-manager.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QSplitter>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle("CoreVideo Sidecar");
    setMinimumSize(1200, 720);
    resize(1440, 860);

    // Central widget holds 3 columns
    auto *central = new QWidget(this);
    setCentralWidget(central);

    auto *rootH = new QHBoxLayout(central);
    rootH->setContentsMargins(0, 0, 0, 0);
    rootH->setSpacing(0);

    // ── Left: Sidebar ─────────────────────────────────────────────────────────
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

    auto *applyBtn = makeToolBtn("⊞  Apply Layout", true);
    auto *spotBtn  = makeToolBtn("◉  Spotlight Speaker");
    auto *lowerBtn = makeToolBtn("▼  Lower Thirds");
    auto *recBtn   = makeToolBtn("⏺  Record");
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

    // ── Connections ───────────────────────────────────────────────────────────
    connect(sidebar, &Sidebar::pageSelected, this, &MainWindow::onPageSelected);
    connect(applyBtn, &QPushButton::clicked, this, &MainWindow::onApplyLayout);
    connect(m_engineBtn, &QPushButton::clicked, this, &MainWindow::onEngineToggle);
    connect(m_obsBtn, &QPushButton::clicked, this, &MainWindow::onObsConnect);

    // OBS client
    m_obsClient = new OBSClient(this);
    connect(m_obsClient, &OBSClient::connected,    this, [this]() {
        m_obsConnected = true; updateObsButton();
    });
    connect(m_obsClient, &OBSClient::disconnected, this, [this]() {
        m_obsConnected = false; updateObsButton();
    });

    // Load templates
    auto &tm = TemplateManager::instance();
    tm.loadBuiltIn();
    m_templatePanel->loadTemplates(tm.templates());

    // Pre-select 4-up grid as example
    const LayoutTemplate *grid4 = tm.findById("4-up-grid");
    if (grid4) onTemplateSelected(*grid4);

    // Mock participants
    loadMockParticipants();
}

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

    m_engineBtn = new QPushButton("⏻  Engine OFF", m_topBar);
    m_engineBtn->setObjectName("engineOffBtn");
    m_engineBtn->setFixedHeight(34);

    m_obsBtn = new QPushButton("OBS  ●", m_topBar);
    m_obsBtn->setObjectName("obsBtn");
    m_obsBtn->setFixedHeight(34);
    updateObsButton();

    row->addWidget(m_showNameLabel, 1);
    row->addWidget(m_obsBtn);
    row->addWidget(m_engineBtn);
}

void MainWindow::buildCenterArea(QWidget *parent)
{
    m_canvasArea = new QWidget(parent);
    auto *row = new QHBoxLayout(m_canvasArea);
    row->setContentsMargins(12, 12, 12, 8);
    row->setSpacing(12);

    // Live preview
    auto *liveWrap = new QWidget(m_canvasArea);
    auto *liveV    = new QVBoxLayout(liveWrap);
    liveV->setContentsMargins(0, 0, 0, 0);
    liveV->setSpacing(4);
    auto *liveLbl  = new QLabel("LIVE", liveWrap);
    liveLbl->setStyleSheet("color: #ff4040; font-size: 10px; font-weight: 800; "
                           "letter-spacing: 0.1em; background: transparent;");
    m_liveCanvas = new PreviewCanvas(liveWrap);
    liveV->addWidget(liveLbl);
    liveV->addWidget(m_liveCanvas, 1);
    row->addWidget(liveWrap, 1);

    // Scene preview
    auto *sceneWrap = new QWidget(m_canvasArea);
    auto *sceneV    = new QVBoxLayout(sceneWrap);
    sceneV->setContentsMargins(0, 0, 0, 0);
    sceneV->setSpacing(4);
    auto *sceneLbl  = new QLabel("SCENE PREVIEW", sceneWrap);
    sceneLbl->setStyleSheet("color: #5080ff; font-size: 10px; font-weight: 800; "
                            "letter-spacing: 0.1em; background: transparent;");
    m_sceneCanvas = new PreviewCanvas(sceneWrap);
    sceneV->addWidget(sceneLbl);
    sceneV->addWidget(m_sceneCanvas, 1);
    row->addWidget(sceneWrap, 1);
}

void MainWindow::buildRightPanel(QWidget *parent)
{
    auto *vl = new QVBoxLayout(parent);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    m_templatePanel = new TemplatePanel(parent);
    connect(m_templatePanel, &TemplatePanel::templateSelected,
            this, &MainWindow::onTemplateSelected);
    vl->addWidget(m_templatePanel);

    // Divider
    auto *div = new QFrame(parent);
    div->setFrameShape(QFrame::HLine);
    div->setStyleSheet("color: #1e1e2e;");
    vl->addWidget(div);

    m_participantPanel = new ParticipantPanel(parent);
    vl->addWidget(m_participantPanel, 1);
}

void MainWindow::loadMockParticipants()
{
    QVector<ParticipantInfo> parts = {
        {1, "Alex Rivera",   "AR", QColor(0x1e, 0x6a, 0xe0), false, true,  0},
        {2, "Sam Chen",      "SC", QColor(0x20, 0xa0, 0x60), false, false, 1},
        {3, "Jordan Kim",    "JK", QColor(0x9b, 0x40, 0xd0), false, true,  2},
        {4, "Taylor Brooks", "TB", QColor(0xe0, 0x60, 0x20), false, false, 3},
    };
    m_participantPanel->setParticipants(parts);

    // Set on scene canvas
    QVector<PreviewCanvas::Participant> canvasParts;
    for (const auto &p : parts) {
        canvasParts.append({p.name, p.initials, p.color, p.isTalking, p.hasVideo});
    }
    m_liveCanvas->setParticipants(canvasParts);
    m_sceneCanvas->setParticipants(canvasParts);
}

void MainWindow::onPageSelected(Sidebar::Page /*p*/)
{
    // Future: switch stacked widget pages
}

void MainWindow::onTemplateSelected(const LayoutTemplate &tmpl)
{
    m_currentTemplate = tmpl;
    m_liveCanvas->setTemplate(tmpl);
    m_sceneCanvas->setTemplate(tmpl);
}

void MainWindow::onApplyLayout()
{
    if (!m_currentTemplate.isValid()) return;
    if (m_obsConnected) {
        QStringList sources;
        for (int i = 0; i < m_currentTemplate.slots.size(); ++i)
            sources << QString("CoreVideo Participant %1").arg(i + 1);
        m_obsClient->applyLayout("Scene", m_currentTemplate, sources, 1920, 1080);
    }
    // Mock: previews already up to date via onTemplateSelected
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
    if (m_obsConnected) {
        m_obsClient->disconnectFromOBS();
    } else {
        m_obsClient->connectToOBS({"localhost", 4455, ""});
    }
}

void MainWindow::updateObsButton()
{
    m_obsBtn->setProperty("connected", m_obsConnected ? "true" : "false");
    m_obsBtn->setText(m_obsConnected ? "OBS  ●" : "OBS  ○");
    m_obsBtn->style()->unpolish(m_obsBtn);
    m_obsBtn->style()->polish(m_obsBtn);
}
