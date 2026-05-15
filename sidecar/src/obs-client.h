#pragma once
#include "layout-template.h"
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QStringList>
#include <QTimer>
#include <optional>

class QWebSocket;

// obs-websocket v5 client with auto-reconnect, sceneItemId resolution,
// transform application, and JSON-based template apply.
class OBSClient : public QObject {
    Q_OBJECT
public:
    enum class State {
        Disconnected,
        Connecting,
        Authenticating,
        Connected,
        Reconnecting,
        Failed,
    };
    Q_ENUM(State)

    struct Config {
        QString host     = "localhost";
        int     port     = 4455;
        QString password;             // empty = no auth
        bool    autoReconnect = true;
    };

    struct SceneItem {
        int     sceneItemId = 0;
        QString sourceName;
        bool    enabled = true;
    };

    // Full transform spec for SetSceneItemTransform.
    // All fields optional — only set what you want to change.
    struct Transform {
        std::optional<double> positionX, positionY;
        std::optional<double> scaleX,    scaleY;
        std::optional<double> rotation;
        std::optional<double> cropLeft, cropRight, cropTop, cropBottom;
        std::optional<double> boundsWidth, boundsHeight;
        std::optional<QString> boundsType;   // OBS_BOUNDS_*
        std::optional<int>     alignment;
        QJsonObject toJson() const;
    };

    explicit OBSClient(QObject *parent = nullptr);
    ~OBSClient() override;

    // ── Connection ───────────────────────────────────────────────────────────
    void  connectToOBS(const Config &cfg);
    void  disconnectFromOBS();
    bool  isConnected() const { return m_state == State::Connected; }
    State state()       const { return m_state; }
    QString stateLabel() const;

    // ── Scene queries ────────────────────────────────────────────────────────
    void requestSceneList();
    void requestSceneItems(const QString &sceneName);
    void setCurrentScene(const QString &name);

    // ── Virtual camera ───────────────────────────────────────────────────────
    void requestVirtualCamStatus();
    void startVirtualCam();
    void stopVirtualCam();
    bool isVirtualCamActive() const { return m_virtualCamActive; }

    // ── Transform application ────────────────────────────────────────────────
    void setSceneItemTransform(const QString  &sceneName,
                               int             sceneItemId,
                               const Transform &t);

    // Apply a normalized LayoutTemplate by mapping slot[i] → sourceNames[i].
    // Resolves source names to sceneItemIds via the cache (call
    // requestSceneItems() first, or pass cached items).
    void applyLayout(const QString          &sceneName,
                     const LayoutTemplate   &tmpl,
                     const QStringList      &sourceNames,
                     double canvasW, double canvasH);

    // Apply a flat applied-template JSON in the format:
    //   { "name": "...", "scene": "...", "items": [
    //       { "source": "...", "x": 0, "y": 0, "scale": 0.5,
    //         "scaleX": ..., "scaleY": ..., "rotation": ...,
    //         "cropLeft": ..., "cropRight": ..., "cropTop": ..., "cropBottom": ...,
    //         "boundsWidth": ..., "boundsHeight": ..., "boundsType": "..." }, ...
    //   ] }
    // Returns false if not connected or template malformed.
    bool applyTemplate(const QJsonObject &templateJson);

signals:
    void stateChanged(OBSClient::State s);
    void connected();
    void disconnected();
    void errorOccurred(const QString &msg);
    void scenesReceived(const QStringList &scenes);
    void sceneItemsReceived(const QString &scene, const QVector<SceneItem> &items);
    void templateApplied(const QString &name, int itemCount);
    void virtualCamStateChanged(bool active);
    void log(const QString &msg);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &msg);
    void tryReconnect();

private:
    void setState(State s);
    void sendOp(int op, const QJsonObject &data);
    void sendRequest(const QString &type, const QJsonObject &data = {},
                     const QString &id = {});
    QString nextId();
    void handleHello(const QJsonObject &d);
    void handleResponse(const QJsonObject &d);
    void handleEvent(const QJsonObject &d);
    int  resolveItemId(const QString &scene, const QString &source) const;
    static QString computeAuth(const QString &password,
                               const QString &salt,
                               const QString &challenge);

    QWebSocket *m_ws       = nullptr;
    QTimer     *m_reconnectTimer = nullptr;
    Config      m_cfg;
    State       m_state = State::Disconnected;
    int         m_idSeq = 1;
    int         m_reconnectAttempt = 0;
    QHash<QString, QString> m_pending;               // requestId → requestType
    QHash<QString, QHash<QString, int>> m_itemCache; // scene → (source → itemId)
    bool m_virtualCamActive = false;
};
