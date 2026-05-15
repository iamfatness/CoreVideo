#pragma once
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class QTcpServer;
class QTcpSocket;

// Newline-delimited JSON control server for the Sidecar app.
// Companion connects here (default port 19880) to control show phases,
// apply templates, and receive push events.
class SidecarControlServer : public QObject {
    Q_OBJECT
public:
    explicit SidecarControlServer(QObject *parent = nullptr);

    bool start(quint16 port = 19880);
    void stop();

    // Push a JSON event to all subscribe_events subscribers.
    void pushEvent(const QJsonObject &event);

    // Called by MainWindow when its own state changes.
    // Each notifier caches the new value and pushes an event to subscribers.
    void notifyPhaseChanged(const QString &phase);
    void notifyTemplateChanged(const QString &id, const QString &name);
    void notifyOBSState(const QString &state);
    void notifySceneChanged(const QString &scene);
    void notifyScenesUpdated(const QStringList &scenes);

signals:
    void phaseChangeRequested(const QString &phase);
    void templateApplyRequested(const QString &templateId);
    void sceneChangeRequested(const QString &scene);

private:
    void handleLine(QTcpSocket *socket, const QByteArray &line);
    void writeResponse(QTcpSocket *socket, const QJsonObject &resp);
    void removeSubscriber(QTcpSocket *socket);

    QTcpServer         *m_server      = nullptr;
    QSet<QTcpSocket *>  m_eventSubs;

    // Cached for status replies
    QString     m_phase        = "pre_show";
    QString     m_templateId;
    QString     m_templateName;
    QString     m_obsState     = "disconnected";
    QStringList m_scenes;
    QString     m_currentScene;
};
