#pragma once
#include "look.h"
#include "layout-template.h"
#include "macro.h"
#include "overlay.h"
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <optional>

class SimpleWebSocket;

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

    struct CoreVideoSyncStatus {
        bool inventoryReady = false;
        int expectedParticipantSources = 0;
        int presentParticipantSources = 0;
        int expectedSlotScenes = 0;
        int presentSlotScenes = 0;
        int expectedLookScenes = 0;
        int presentLookScenes = 0;
        QStringList missingInputs;
        QStringList missingScenes;
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
    void requestInputList();
    void refreshInventory();
    void setCurrentScene(const QString &name);
    void setCurrentPreviewScene(const QString &name);
    QStringList sceneItemSourceNames(const QString &sceneName) const;
    CoreVideoSyncStatus coreVideoSyncStatus(const QStringList &participantSources,
                                            const QStringList &lookScenes) const;

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

    // Create the target scene and CoreVideo source placeholders when needed,
    // then apply the normalized template once OBS has refreshed scene items.
    void loadSceneTemplate(const QString        &sceneName,
                           const LayoutTemplate &tmpl,
                           const QStringList    &sourceNames,
                           double canvasW, double canvasH,
                           const QVector<Overlay> &overlays = {},
                           const QString &backgroundImagePath = {},
                           const TileStyle &tileStyle = {},
                           const QStringList &slotLabels = {},
                           bool makeProgram = false);
    void ensureCoreVideoSources(const QString &sceneName,
                                const QStringList &sourceNames);
    void removeStaleCoreVideoDuplicates(const QStringList &participantSources,
                                        const QStringList &lookScenes);
    void applyOverlays(const QString &sceneName,
                       const QVector<Overlay> &overlays,
                       double canvasW, double canvasH);
    void applyBackgroundImage(const QString &sceneName,
                              const QString &imagePath,
                              double canvasW, double canvasH);
    void applyCanvasColor(const QString &sceneName,
                          const TileStyle &tileStyle,
                          double canvasW, double canvasH,
                          bool retryAfterCreate = true);
    void applyTileDecorations(const QString &sceneName,
                              const LayoutTemplate &tmpl,
                              const TileStyle &tileStyle,
                              const QStringList &slotLabels,
                              double canvasW, double canvasH,
                              bool retryAfterCreate = true);

    // Apply a flat applied-template JSON in the format:
    //   { "name": "...", "scene": "...", "items": [
    //       { "source": "...", "x": 0, "y": 0, "scale": 0.5,
    //         "scaleX": ..., "scaleY": ..., "rotation": ...,
    //         "cropLeft": ..., "cropRight": ..., "cropTop": ..., "cropBottom": ...,
    //         "boundsWidth": ..., "boundsHeight": ..., "boundsType": "..." }, ...
    //   ] }
    // Returns false if not connected or template malformed.
    bool applyTemplate(const QJsonObject &templateJson);

    // Execute a Macro: sends all steps as a RequestBatch.
    void executeMacro(const Macro &macro);

signals:
    void stateChanged(OBSClient::State s);
    void connected();
    void disconnected();
    void errorOccurred(const QString &msg);
    void scenesReceived(const QStringList &scenes);
    void sceneItemsReceived(const QString &scene, const QVector<SceneItem> &items);
    void inventoryReady();
    void sceneChanged(const QString &name);
    void templateApplied(const QString &name, int itemCount);
    void virtualCamStateChanged(bool active);
    void log(const QString &msg);
    void requestFailed(const QString &summary);

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
    void enqueueCreateSceneIfMissing(QJsonArray &requests, const QString &sceneName);
    void enqueueCreateInputIfMissing(QJsonArray &requests,
                                     const QString &sceneName,
                                     const QString &inputName,
                                     const QString &inputKind,
                                     const QJsonObject &inputSettings = {});
    static QString overlaySourceName(const QString &sceneName, int index);
    static QString computeAuth(const QString &password,
                               const QString &salt,
                               const QString &challenge);

    SimpleWebSocket *m_ws       = nullptr;
    QTimer     *m_reconnectTimer = nullptr;
    Config      m_cfg;
    State       m_state = State::Disconnected;
    int         m_idSeq = 1;
    int         m_reconnectAttempt = 0;
    QHash<QString, QString> m_pending;               // requestId → requestType
    QHash<QString, QHash<QString, int>> m_itemCache; // scene → (source → itemId)
    QHash<QString, QString> m_pendingSceneItemLists;
    QHash<QString, QVector<SceneItem>> m_sceneItems;
    QSet<QString> m_knownScenes;
    QSet<QString> m_knownInputs;
    bool m_receivedSceneList = false;
    bool m_receivedInputList = false;
    bool m_inventoryReadyEmitted = false;
    bool m_virtualCamActive = false;
};
