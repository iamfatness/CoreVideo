#pragma once
#include <QDialog>

class QStackedWidget;
class QLabel;
class QLineEdit;
class QPushButton;
class CvStatusDot;

// Multi-step first-run wizard shown when no SDK credentials are configured.
// Guides the user through: Welcome → SDK Credentials → Test → Done.
class CvOnboardingWizard : public QDialog {
    Q_OBJECT
public:
    explicit CvOnboardingWizard(QWidget *parent = nullptr);

    // Returns true if the wizard has been completed before (flag stored in
    // QSettings). Call this before showing the wizard to skip repeat display.
    static bool isCompleted();
    static void markCompleted();

private slots:
    void next();
    void back();
    void testCredentials();

private:
    void buildWelcomePage(QWidget *page);
    void buildCredentialsPage(QWidget *page);
    void buildTestPage(QWidget *page);
    void buildDonePage(QWidget *page);
    void updateButtons();

    QStackedWidget *m_stack    = nullptr;
    QPushButton    *m_backBtn  = nullptr;
    QPushButton    *m_nextBtn  = nullptr;
    QPushButton    *m_testBtn  = nullptr;

    // Credentials page
    QLineEdit *m_sdkKey    = nullptr;
    QLineEdit *m_sdkSecret = nullptr;
    QLineEdit *m_jwtToken  = nullptr;

    // Test page
    CvStatusDot *m_testDot    = nullptr;
    QLabel      *m_testStatus = nullptr;

    static constexpr int kPageWelcome     = 0;
    static constexpr int kPageCredentials = 1;
    static constexpr int kPageTest        = 2;
    static constexpr int kPageDone        = 3;
};
