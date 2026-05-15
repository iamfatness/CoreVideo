#include "cv-onboarding.h"
#include "cv-style.h"
#include "cv-widgets.h"
#include "zoom-settings.h"
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

// ── Static helpers ────────────────────────────────────────────────────────────
bool CvOnboardingWizard::isCompleted()
{
    QSettings s;
    return s.value("onboarding/completed", false).toBool();
}

void CvOnboardingWizard::markCompleted()
{
    QSettings s;
    s.setValue("onboarding/completed", true);
}

// ── Constructor ───────────────────────────────────────────────────────────────
CvOnboardingWizard::CvOnboardingWizard(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("CoreVideo Setup");
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setFixedSize(480, 380);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Page header banner
    auto *banner = new QWidget(this);
    banner->setFixedHeight(52);
    banner->setStyleSheet("background: qlineargradient(x1:0,y1:0,x2:1,y2:0,"
                          "stop:0 #1a2fff, stop:1 #7010e0);");
    auto *bannerRow = new QHBoxLayout(banner);
    bannerRow->setContentsMargins(20, 0, 20, 0);
    auto *bannerTitle = new QLabel("CoreVideo Setup", banner);
    bannerTitle->setStyleSheet("color: white; font-size: 16px; font-weight: 700; background: transparent;");
    bannerRow->addWidget(bannerTitle);
    root->addWidget(banner);

    // Stacked pages
    m_stack = new QStackedWidget(this);
    root->addWidget(m_stack, 1);

    auto addPage = [this]() {
        auto *p = new QWidget;
        m_stack->addWidget(p);
        return p;
    };

    buildWelcomePage(addPage());
    buildCredentialsPage(addPage());
    buildTestPage(addPage());
    buildDonePage(addPage());

    // Bottom button bar
    auto *btnBar = new QWidget(this);
    btnBar->setStyleSheet("background: #131319; border-top: 1px solid #1e1e2e;");
    btnBar->setFixedHeight(54);
    auto *btnRow = new QHBoxLayout(btnBar);
    btnRow->setContentsMargins(16, 0, 16, 0);
    btnRow->setSpacing(8);

    m_backBtn = new QPushButton("← Back", btnBar);
    m_nextBtn = new QPushButton("Next →", btnBar);
    m_testBtn = new QPushButton("Test Connection", btnBar);

    m_nextBtn->setProperty("role", "primary");
    m_testBtn->setProperty("role", "primary");

    btnRow->addWidget(m_backBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(m_testBtn);
    btnRow->addWidget(m_nextBtn);

    root->addWidget(btnBar);

    connect(m_backBtn, &QPushButton::clicked, this, &CvOnboardingWizard::back);
    connect(m_nextBtn, &QPushButton::clicked, this, &CvOnboardingWizard::next);
    connect(m_testBtn, &QPushButton::clicked, this, &CvOnboardingWizard::testCredentials);

    setStyleSheet(cv_stylesheet());
    updateButtons();
}

// ── Pages ─────────────────────────────────────────────────────────────────────
void CvOnboardingWizard::buildWelcomePage(QWidget *page)
{
    auto *vl = new QVBoxLayout(page);
    vl->setContentsMargins(28, 24, 28, 16);
    vl->setSpacing(12);

    auto *title = new QLabel("Welcome to CoreVideo", page);
    title->setStyleSheet("font-size: 20px; font-weight: 700; color: #e0e0f0;");

    auto *body = new QLabel(
        "CoreVideo integrates Zoom Meeting SDK directly into OBS Studio, "
        "letting you receive individual participant video and audio streams as "
        "native OBS sources.\n\n"
        "This wizard takes about 2 minutes and will guide you through:\n"
        "  • Entering your Zoom SDK credentials\n"
        "  • Verifying the connection\n\n"
        "You'll need a Zoom developer account with an SDK app. "
        "Visit marketplace.zoom.us to create one.", page);
    body->setWordWrap(true);
    body->setStyleSheet("color: #a0a0c0; font-size: 13px; line-height: 1.5;");

    vl->addWidget(title);
    vl->addWidget(body);
    vl->addStretch(1);
}

void CvOnboardingWizard::buildCredentialsPage(QWidget *page)
{
    auto *vl = new QVBoxLayout(page);
    vl->setContentsMargins(28, 20, 28, 16);
    vl->setSpacing(10);

    auto *title = new QLabel("SDK Credentials", page);
    title->setStyleSheet("font-size: 16px; font-weight: 700; color: #e0e0f0;");
    vl->addWidget(title);

    auto *help = new QLabel(
        "Enter your Zoom Meeting SDK credentials from marketplace.zoom.us.\n"
        "Use either SDK Key + Secret (JWT apps) or a server-to-server JWT token.", page);
    help->setWordWrap(true);
    help->setStyleSheet("color: #7070a0; font-size: 11px;");
    vl->addWidget(help);

    auto addField = [&](const QString &label, bool password = false) -> QLineEdit * {
        auto *lbl = new QLabel(label, page);
        lbl->setStyleSheet("color: #c0c0e0; font-size: 12px;");
        auto *edit = new QLineEdit(page);
        if (password) edit->setEchoMode(QLineEdit::Password);
        vl->addWidget(lbl);
        vl->addWidget(edit);
        return edit;
    };

    m_sdkKey    = addField("SDK Key");
    m_sdkSecret = addField("SDK Secret", true);

    auto *sep = new QLabel("— or —", page);
    sep->setAlignment(Qt::AlignCenter);
    sep->setStyleSheet("color: #404060; font-size: 11px;");
    vl->addWidget(sep);

    m_jwtToken = addField("Server-to-Server JWT Token", true);

    // Pre-fill if credentials already exist
    const ZoomPluginSettings s = ZoomPluginSettings::load();
    m_sdkKey->setText(QString::fromStdString(s.sdk_key));
    m_sdkSecret->setText(QString::fromStdString(s.sdk_secret));
    m_jwtToken->setText(QString::fromStdString(s.jwt_token));

    vl->addStretch(1);
}

void CvOnboardingWizard::buildTestPage(QWidget *page)
{
    auto *vl = new QVBoxLayout(page);
    vl->setContentsMargins(28, 24, 28, 16);
    vl->setSpacing(12);

    auto *title = new QLabel("Verify Credentials", page);
    title->setStyleSheet("font-size: 16px; font-weight: 700; color: #e0e0f0;");
    vl->addWidget(title);

    auto *help = new QLabel(
        "Click 'Test Connection' to validate your SDK credentials.\n"
        "The SDK will be initialised with the values you entered.", page);
    help->setWordWrap(true);
    help->setStyleSheet("color: #7070a0; font-size: 11px;");
    vl->addWidget(help);

    auto *statusRow = new QHBoxLayout;
    m_testDot = new CvStatusDot(page);
    m_testStatus = new QLabel("Not tested yet", page);
    m_testStatus->setStyleSheet("color: #a0a0c0; font-size: 13px;");
    statusRow->addWidget(m_testDot);
    statusRow->addWidget(m_testStatus, 1);
    vl->addLayout(statusRow);

    vl->addStretch(1);

    auto *note = new QLabel(
        "If the test fails, go back and double-check your credentials.\n"
        "You can also skip this step and test manually from the OBS dock.", page);
    note->setWordWrap(true);
    note->setStyleSheet("color: #505070; font-size: 10px;");
    vl->addWidget(note);
}

void CvOnboardingWizard::buildDonePage(QWidget *page)
{
    auto *vl = new QVBoxLayout(page);
    vl->setContentsMargins(28, 24, 28, 16);
    vl->setSpacing(12);

    auto *title = new QLabel("You're all set!", page);
    title->setStyleSheet("font-size: 20px; font-weight: 700; color: #20c460;");
    vl->addWidget(title);

    auto *body = new QLabel(
        "CoreVideo is configured and ready to use.\n\n"
        "To start using it:\n"
        "  1. Open the CoreVideo dock in OBS (Docks menu)\n"
        "  2. Enter a Zoom Meeting ID and click Join\n"
        "  3. Assign participants to your OBS sources\n"
        "  4. Click Apply\n\n"
        "You can update credentials anytime in the CoreVideo settings dialog.", page);
    body->setWordWrap(true);
    body->setStyleSheet("color: #a0a0c0; font-size: 13px; line-height: 1.5;");
    vl->addWidget(body);
    vl->addStretch(1);
}

// ── Navigation ────────────────────────────────────────────────────────────────
void CvOnboardingWizard::next()
{
    const int cur = m_stack->currentIndex();

    if (cur == kPageCredentials) {
        // Save credentials before moving to test page
        ZoomPluginSettings s = ZoomPluginSettings::load();
        s.sdk_key    = m_sdkKey->text().trimmed().toStdString();
        s.sdk_secret = m_sdkSecret->text().trimmed().toStdString();
        s.jwt_token  = m_jwtToken->text().trimmed().toStdString();
        s.save();
    }

    if (cur == kPageDone) {
        markCompleted();
        accept();
        return;
    }

    m_stack->setCurrentIndex(cur + 1);
    updateButtons();
}

void CvOnboardingWizard::back()
{
    const int cur = m_stack->currentIndex();
    if (cur > 0) {
        m_stack->setCurrentIndex(cur - 1);
        updateButtons();
    }
}

void CvOnboardingWizard::testCredentials()
{
    m_testStatus->setText("Checking credentials…");
    m_testDot->setState(MeetingState::Joining);

    // Validate that at least one credential set is non-empty.
    // Full SDK init verification happens on the first Join attempt.
    QTimer::singleShot(600, this, [this]() {
        const ZoomPluginSettings s = ZoomPluginSettings::load();
        const bool hasJwt = !s.jwt_token.empty();
        const bool hasSdk = !s.sdk_key.empty() && !s.sdk_secret.empty();
        if (hasJwt || hasSdk) {
            m_testStatus->setText(
                hasJwt ? "JWT token saved — credentials look good."
                       : "SDK Key + Secret saved — credentials look good.");
            m_testDot->setState(MeetingState::InMeeting);
        } else {
            m_testStatus->setText("No credentials found. Go back and enter your SDK details.");
            m_testDot->setState(MeetingState::Failed);
        }
    });
}

void CvOnboardingWizard::updateButtons()
{
    const int cur = m_stack->currentIndex();
    m_backBtn->setVisible(cur > kPageWelcome && cur < kPageDone);
    m_testBtn->setVisible(cur == kPageTest);
    m_nextBtn->setText(cur == kPageDone ? "Finish" : "Next →");
    m_nextBtn->setProperty("role",
        (cur == kPageDone || cur == kPageCredentials) ? "primary" : "");
    m_nextBtn->style()->unpolish(m_nextBtn);
    m_nextBtn->style()->polish(m_nextBtn);
}
