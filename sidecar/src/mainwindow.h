#pragma once
#include "sidebar.h"
#include "layout-template.h"
#include "obs-client.h"
#include <QMainWindow>

class PreviewCanvas;
class TemplatePanel;
class ParticipantPanel;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QDockWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onPageSelected(Sidebar::Page p);
    void onTemplateSelected(const LayoutTemplate &tmpl);
    void onApplyLayout();
    void onEngineToggle();
    void onObsConnect();
    void onObsState(OBSClient::State s);
    void onObsLog(const QString &msg);
    void onScenesReceived(const QStringList &scenes);

private:
    void buildTopBar(QWidget *parent);
    void buildCenterArea(QWidget *parent);
    void buildRightPanel(QWidget *parent);
    void buildLogDock();
    void loadMockParticipants();
    void updateObsButton();

    // Top bar
    QWidget     *m_topBar         = nullptr;
    QWidget     *m_canvasArea     = nullptr;
    QLabel      *m_showNameLabel  = nullptr;
    QLabel      *m_obsStatusLabel = nullptr;
    QPushButton *m_engineBtn      = nullptr;
    QPushButton *m_obsBtn         = nullptr;
    bool         m_engineOn       = false;

    // Center
    PreviewCanvas *m_liveCanvas   = nullptr;
    PreviewCanvas *m_sceneCanvas  = nullptr;

    // Right panel
    TemplatePanel    *m_templatePanel    = nullptr;
    ParticipantPanel *m_participantPanel = nullptr;

    // Log
    QDockWidget    *m_logDock = nullptr;
    QPlainTextEdit *m_logView = nullptr;

    // State
    LayoutTemplate    m_currentTemplate;
    OBSClient        *m_obsClient = nullptr;
    OBSClient::Config m_obsConfig;
    QString           m_targetScene = "CoreVideo Main";
};
