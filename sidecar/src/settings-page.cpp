#include "settings-page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static QLabel *makeSectionHeader(const QString &text)
{
    auto *lbl = new QLabel(text);
    lbl->setStyleSheet(
        "color: #c0c0e0;"
        "font-size: 11px;"
        "font-weight: 700;"
        "letter-spacing: 0.08em;"
        "padding: 10px 12px 4px 12px;"
        "background: transparent;");
    return lbl;
}

static QLabel *makeHelpText(const QString &text)
{
    auto *lbl = new QLabel(text);
    lbl->setWordWrap(true);
    lbl->setStyleSheet(
        "color: #5a5a7a;"
        "font-size: 10px;"
        "background: transparent;"
        "padding: 0 0 4px 0;");
    return lbl;
}

static QFrame *makeHLine()
{
    auto *line = new QFrame();
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

SettingsPage::SettingsPage(QWidget *parent)
    : QWidget(parent)
{
    // ---- Top-level layout (no margins, sections separated by HLines) ------
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // ======================================================================
    // Section 1 — OBS Connection
    // ======================================================================
    rootLayout->addWidget(makeSectionHeader("OBS CONNECTION"));

    auto *obsSection = new QWidget();
    auto *obsForm    = new QFormLayout(obsSection);
    obsForm->setContentsMargins(12, 4, 12, 8);
    obsForm->setSpacing(8);

    m_obsHost = new QLineEdit();
    m_obsHost->setPlaceholderText("localhost");
    obsForm->addRow("Host:", m_obsHost);

    m_obsPort = new QSpinBox();
    m_obsPort->setRange(1, 65535);
    obsForm->addRow("Port:", m_obsPort);

    m_obsPassword = new QLineEdit();
    m_obsPassword->setEchoMode(QLineEdit::Password);
    m_obsPassword->setPlaceholderText("Leave empty if no auth");
    obsForm->addRow("Token:", m_obsPassword);

    m_autoReconnect = new QCheckBox("Reconnect automatically on disconnect");
    obsForm->addRow("", m_autoReconnect);

    rootLayout->addWidget(obsSection);
    rootLayout->addWidget(makeHLine());

    // ======================================================================
    // Section 2 — Scene Setup
    // ======================================================================
    rootLayout->addWidget(makeSectionHeader("SCENE SETUP"));

    auto *sceneSection = new QWidget();
    auto *sceneForm    = new QFormLayout(sceneSection);
    sceneForm->setContentsMargins(12, 4, 12, 8);
    sceneForm->setSpacing(8);

    m_scene = new QLineEdit();
    m_scene->setPlaceholderText("CoreVideo Main");
    sceneForm->addRow("Target Scene:", m_scene);
    sceneForm->addRow("", makeHelpText("The OBS scene where CoreVideo Zoom sources live."));

    m_sourcePattern = new QLineEdit();
    m_sourcePattern->setPlaceholderText("Zoom Participant %1");
    sceneForm->addRow("Source Pattern:", m_sourcePattern);
    sceneForm->addRow("", makeHelpText("Use %1 for slot number (1-based). e.g. 'Zoom Participant %1'"));

    m_canvasW = new QSpinBox();
    m_canvasW->setRange(640, 7680);
    m_canvasW->setValue(1920);
    sceneForm->addRow("Canvas Width:", m_canvasW);

    m_canvasH = new QSpinBox();
    m_canvasH->setRange(360, 4320);
    m_canvasH->setValue(1080);
    sceneForm->addRow("Canvas Height:", m_canvasH);

    rootLayout->addWidget(sceneSection);
    rootLayout->addWidget(makeHLine());

    // ======================================================================
    // Section 3 — About
    // ======================================================================
    rootLayout->addWidget(makeSectionHeader("ABOUT"));

    auto *aboutSection  = new QWidget();
    auto *aboutLayout   = new QVBoxLayout(aboutSection);
    aboutLayout->setContentsMargins(12, 4, 12, 8);
    aboutLayout->setSpacing(4);

    auto *appName = new QLabel("CoreVideo Sidecar");
    QFont boldFont = appName->font();
    boldFont.setBold(true);
    appName->setFont(boldFont);
    aboutLayout->addWidget(appName);

    aboutLayout->addWidget(new QLabel("Version 0.1.0 — obs-websocket v5"));

    rootLayout->addWidget(aboutSection);

    // ======================================================================
    // Spacer + Save button
    // ======================================================================
    rootLayout->addStretch(1);

    auto *saveBtn = new QPushButton("Save Settings");
    saveBtn->setObjectName("toolBtn");
    saveBtn->setProperty("primary", true);
    // Use a small margin wrapper so the button isn't edge-to-edge
    auto *btnWrapper  = new QWidget();
    auto *btnLayout   = new QVBoxLayout(btnWrapper);
    btnLayout->setContentsMargins(12, 8, 12, 12);
    btnLayout->addWidget(saveBtn);
    rootLayout->addWidget(btnWrapper);

    connect(saveBtn, &QPushButton::clicked, this, [this]() {
        saveSettings();
        emit settingsChanged();
    });

    // Populate fields from persisted settings
    loadSettings();
}

// ---------------------------------------------------------------------------
// loadSettings
// ---------------------------------------------------------------------------

void SettingsPage::loadSettings()
{
    QSettings s;

    m_obsHost->setText(s.value("obs/host", "localhost").toString());
    m_obsPort->setValue(s.value("obs/port", 4455).toInt());
    m_obsPassword->setText(s.value("obs/password", "").toString());
    m_autoReconnect->setChecked(s.value("obs/autoReconnect", true).toBool());

    m_scene->setText(s.value("scene/target", "CoreVideo Main").toString());
    m_sourcePattern->setText(s.value("scene/sourcePattern", "Zoom Participant %1").toString());
    m_canvasW->setValue(s.value("scene/canvasW", 1920).toInt());
    m_canvasH->setValue(s.value("scene/canvasH", 1080).toInt());
}

// ---------------------------------------------------------------------------
// saveSettings
// ---------------------------------------------------------------------------

void SettingsPage::saveSettings()
{
    QSettings s;

    s.setValue("obs/host",          m_obsHost->text());
    s.setValue("obs/port",          m_obsPort->value());
    s.setValue("obs/password",      m_obsPassword->text());
    s.setValue("obs/autoReconnect", m_autoReconnect->isChecked());

    s.setValue("scene/target",        m_scene->text());
    s.setValue("scene/sourcePattern", m_sourcePattern->text());
    s.setValue("scene/canvasW",       m_canvasW->value());
    s.setValue("scene/canvasH",       m_canvasH->value());
}

// ---------------------------------------------------------------------------
// Getters
// ---------------------------------------------------------------------------

QString SettingsPage::targetScene() const
{
    return m_scene->text();
}

QString SettingsPage::sourcePattern() const
{
    return m_sourcePattern->text();
}

int SettingsPage::canvasWidth() const
{
    return m_canvasW->value();
}

int SettingsPage::canvasHeight() const
{
    return m_canvasH->value();
}

OBSClient::Config SettingsPage::obsConfig() const
{
    OBSClient::Config cfg;
    cfg.host          = m_obsHost->text();
    cfg.port          = m_obsPort->value();
    cfg.password      = m_obsPassword->text();
    cfg.autoReconnect = m_autoReconnect->isChecked();
    return cfg;
}
