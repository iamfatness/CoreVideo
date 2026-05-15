#pragma once
#include "sidebar.h"
#include "layout-template.h"
#include "obs-client.h"
#include "show-theme.h"
#include <QMainWindow>

class PreviewCanvas;
class TemplatePanel;
class ParticipantPanel;
class ThemePanel;
class SettingsPage;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QDockWidget;
class QStackedWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onPageSelected(Sidebar::Page p);
    void onTemplateSelected(const LayoutTemplate &tmpl);
    void onThemeSelected(const ShowTheme &theme);
    void onApplyLayout();
    void onEngineToggle();
    void onObsConnect();
    void onObsState(OBSClient::State s);
    void onObsLog(const QString &msg);
    void onScenesReceived(const QStringList &scenes);
    void onVirtualCamToggle();
    void onVirtualCamState(bool active);
    void onSettingsChanged();

private:
    void buildTopBar(QWidget *parent);
    void buildCenterArea(QWidget *parent);
    void buildRightPanel(QWidget *parent);
    void buildLogDock();
    void loadMockParticipants();

    // Top bar
    QWidget     *m_topBar         = nullptr;
    QWidget     *m_canvasArea     = nullptr;
    QLabel      *m_showNameLabel  = nullptr;
    QLabel      *m_obsStatusLabel = nullptr;
    QPushButton *m_engineBtn      = nullptr;
    QPushButton *m_obsBtn         = nullptr;
    bool         m_engineOn       = false;

    // Toolbar buttons (kept for state updates)
    QPushButton *m_vcamBtn = nullptr;

    // Center
    PreviewCanvas *m_liveCanvas   = nullptr;
    PreviewCanvas *m_sceneCanvas  = nullptr;

    // Right panel — stacked pages
    QStackedWidget   *m_rightStack       = nullptr;
    TemplatePanel    *m_templatePanel    = nullptr;
    ParticipantPanel *m_participantPanel = nullptr;
    ThemePanel       *m_themePanel       = nullptr;
    SettingsPage     *m_settingsPage     = nullptr;

    // Right panel page indices
    int m_pageTemplates    = 0;
    int m_pageThemes       = 0;
    int m_pageParticipants = 0;
    int m_pageSettings     = 0;

    // Log
    QDockWidget    *m_logDock = nullptr;
    QPlainTextEdit *m_logView = nullptr;

    // State
    LayoutTemplate    m_currentTemplate;
    OBSClient        *m_obsClient = nullptr;
    OBSClient::Config m_obsConfig;
    Sidebar          *m_sidebar = nullptr;
};
