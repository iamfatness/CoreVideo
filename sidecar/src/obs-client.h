#pragma once
#include "layout-template.h"
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>

class QWebSocket;

// obs-websocket v5 client.
// Handles authentication, scene queries, and layout application.
class OBSClient : public QObject {
    Q_OBJECT
public:
    struct Config {
        QString host     = "localhost";
        int     port     = 4455;
        QString password;
    };

    // SceneItem mirrors the minimal fields we need from GetSceneItemList
    struct SceneItem {
        int     sceneItemId = 0;
        QString sourceName;
        bool    enabled = true;
    };

    explicit OBSClient(QObject *parent = nullptr);
    ~OBSClient() override;

    void connectToOBS(const Config &cfg);
    void disconnectFromOBS();
    bool isConnected() const { return m_identified; }

    // Scene control
    void requestSceneList();
    void requestSceneItems(const QString &sceneName);
    void setCurrentScene(const QString &name);

    // Apply a LayoutTemplate: repositions sceneItems to match template slots.
    // sourceNames[i] maps to template slot i.
    // canvasW/H is the OBS canvas resolution (e.g. 1920 x 1080).
    void applyLayout(const QString          &sceneName,
                     const LayoutTemplate   &tmpl,
                     const QStringList      &sourceNames,
                     double canvasW, double canvasH);

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &msg);
    void scenesReceived(const QStringList &scenes);
    void sceneItemsReceived(const QString &scene, const QVector<SceneItem> &items);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &msg);

private:
    void sendOp(int op, const QJsonObject &data);
    void sendRequest(const QString &type, const QJsonObject &data = {});
    QString nextId();
    void handleHello(const QJsonObject &d);
    void handleResponse(const QJsonObject &d);
    void handleEvent(const QJsonObject &d);
    static QString computeAuth(const QString &password,
                               const QString &salt,
                               const QString &challenge);

    QWebSocket *m_ws       = nullptr;
    Config      m_cfg;
    bool        m_identified = false;
    int         m_idSeq      = 1;
    QMap<QString, QString> m_pending; // requestId → requestType
};
