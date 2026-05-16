#include "obs-client.h"
#include "simple-websocket.h"
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <memory>

static const QString kCoreVideoSourcesScene = QStringLiteral("CoreVideo Sources");

static QString coreVideoBackgroundSourceName(const QString &sceneName)
{
    QString safe = sceneName;
    safe.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("-"));
    safe.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return QStringLiteral("CoreVideo Background - %1").arg(safe.left(72));
}

static QString coreVideoCanvasSourceName(const QString &sceneName)
{
    QString safe = sceneName;
    safe.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("-"));
    safe.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return QStringLiteral("CoreVideo Canvas - %1").arg(safe.left(72));
}

static QString coreVideoBorderSourceName(const QString &sceneName, int slotIndex)
{
    QString safe = sceneName;
    safe.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("-"));
    safe.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return QStringLiteral("CoreVideo Border - %1 - Slot %2")
        .arg(safe.left(58))
        .arg(slotIndex + 1);
}

static QString coreVideoNameTagSourceName(const QString &sceneName, int slotIndex)
{
    QString safe = sceneName;
    safe.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("-"));
    safe.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return QStringLiteral("CoreVideo Name - %1 - Slot %2")
        .arg(safe.left(60))
        .arg(slotIndex + 1);
}

static double obsColor(const QColor &color)
{
    const QColor c = color.isValid() ? color : QColor("#101018");
    return double((0xffu << 24)
                  | (uint(c.red()) << 16)
                  | (uint(c.green()) << 8)
                  | uint(c.blue()));
}

static QString coreVideoSlotSceneName(int index)
{
    return QStringLiteral("CoreVideo Slot %1").arg(index + 1);
}

static QString coreVideoSlotPlaceholderName(int index)
{
    return QStringLiteral("CoreVideo Placeholder Slot %1").arg(index + 1);
}

static QString coreVideoInputKindForSource(const QString &sourceName)
{
    return sourceName.compare(QStringLiteral("Zoom Screen Share"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("zoom_share_source")
        : QStringLiteral("zoom_participant_source");
}

static QStringList coreVideoSlotSceneNames(int count)
{
    QStringList names;
    names.reserve(count);
    for (int i = 0; i < count; ++i)
        names << coreVideoSlotSceneName(i);
    return names;
}

OBSClient::OBSClient(QObject *parent) : QObject(parent)
{
    m_ws = new SimpleWebSocket(this);
    connect(m_ws, &SimpleWebSocket::connected,    this, &OBSClient::onConnected);
    connect(m_ws, &SimpleWebSocket::disconnected, this, &OBSClient::onDisconnected);
    connect(m_ws, &SimpleWebSocket::textMessageReceived,
            this, &OBSClient::onTextMessageReceived);
    connect(m_ws, &SimpleWebSocket::errorOccurred, this, [this](const QString &err) {
        qWarning() << "[obs-ws]" << err;
        emit errorOccurred(err);
        emit log(QStringLiteral("Error: %1").arg(err));
    });

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &OBSClient::tryReconnect);
}

OBSClient::~OBSClient()
{
    m_cfg.autoReconnect = false;
    disconnectFromOBS();
}

// ── State ─────────────────────────────────────────────────────────────────────
void OBSClient::setState(State s)
{
    if (m_state == s) return;
    m_state = s;
    emit stateChanged(s);
}

QString OBSClient::stateLabel() const
{
    switch (m_state) {
    case State::Disconnected:   return "Disconnected";
    case State::Connecting:     return "Connecting…";
    case State::Authenticating: return "Authenticating…";
    case State::Connected:      return "Connected";
    case State::Reconnecting:   return "Reconnecting…";
    case State::Failed:         return "Failed";
    }
    return {};
}

// ── Connection ────────────────────────────────────────────────────────────────
void OBSClient::connectToOBS(const Config &cfg)
{
    m_cfg = cfg;
    m_reconnectAttempt = 0;
    m_itemCache.clear();
    m_sceneItems.clear();
    m_pendingSceneItemLists.clear();
    m_knownScenes.clear();
    m_knownInputs.clear();
    m_receivedSceneList = false;
    m_receivedInputList = false;
    m_inventoryReadyEmitted = false;
    setState(State::Connecting);
    emit log(QStringLiteral("Connecting to ws://%1:%2…").arg(cfg.host).arg(cfg.port));
    m_ws->open(cfg.host, static_cast<quint16>(cfg.port));
}

void OBSClient::disconnectFromOBS()
{
    m_reconnectTimer->stop();
    m_cfg.autoReconnect = false;
    m_ws->close();
    setState(State::Disconnected);
}

void OBSClient::onConnected()
{
    setState(State::Authenticating);
    emit log("Socket open — awaiting Hello…");
}

void OBSClient::onDisconnected()
{
    const bool wasConnected = (m_state == State::Connected);
    m_itemCache.clear();
    m_sceneItems.clear();
    m_pendingSceneItemLists.clear();
    m_knownScenes.clear();
    m_knownInputs.clear();
    m_receivedSceneList = false;
    m_receivedInputList = false;
    m_inventoryReadyEmitted = false;
    setState(State::Disconnected);
    emit disconnected();

    if (m_cfg.autoReconnect) {
        // Exponential backoff capped at 30s
        const int delay = std::min(30000, 1000 * (1 << std::min(5, m_reconnectAttempt)));
        ++m_reconnectAttempt;
        setState(State::Reconnecting);
        emit log(QStringLiteral("%1 — reconnecting in %2s (attempt %3)…")
                     .arg(wasConnected ? "Connection lost" : "Connect failed")
                     .arg(delay / 1000)
                     .arg(m_reconnectAttempt));
        m_reconnectTimer->start(delay);
    }
}

void OBSClient::tryReconnect()
{
    if (m_state == State::Connected) return;
    setState(State::Connecting);
    m_ws->open(m_cfg.host, static_cast<quint16>(m_cfg.port));
}

void OBSClient::onTextMessageReceived(const QString &msg)
{
    const QJsonObject root = QJsonDocument::fromJson(msg.toUtf8()).object();
    const int op = root["op"].toInt(-1);
    const QJsonObject d = root["d"].toObject();

    switch (op) {
    case 0: handleHello(d); break;
    case 2:                                   // Identified
        m_reconnectAttempt = 0;
        setState(State::Connected);
        emit connected();
        emit log("Identified — connection ready.");
        requestSceneList();
        requestInputList();
        requestVirtualCamStatus();
        break;
    case 5: handleEvent(d);    break;
    case 7: handleResponse(d); break;
    case 9: {                                 // RequestBatchResponse
        const QJsonArray results = d["results"].toArray();
        int failed = 0;
        QStringList failures;
        for (const auto &v : results) {
            const QJsonObject r = v.toObject();
            const QJsonObject status = r["requestStatus"].toObject();
            if (status["result"].toBool()) continue;
            ++failed;
            failures << QStringLiteral("%1: %2")
                            .arg(r["requestType"].toString(),
                                 status["comment"].toString());
        }
        if (failed > 0) {
            emit log(QStringLiteral("Batch response: %1/%2 failed (%3).")
                         .arg(failed).arg(results.size()).arg(failures.join("; ")));
        } else {
            emit log(QStringLiteral("Batch response received (%1 results).")
                         .arg(results.size()));
        }
        break;
    }
    default: break;
    }
}

// ── Protocol helpers ──────────────────────────────────────────────────────────
void OBSClient::sendOp(int op, const QJsonObject &data)
{
    m_ws->sendTextMessage(
        QJsonDocument(QJsonObject{{"op", op}, {"d", data}}).toJson(QJsonDocument::Compact));
}

void OBSClient::sendRequest(const QString &type, const QJsonObject &data,
                            const QString &id)
{
    const QString reqId = id.isEmpty() ? nextId() : id;
    m_pending[reqId] = type;
    QJsonObject req{{"requestType", type}, {"requestId", reqId}};
    if (!data.isEmpty()) req["requestData"] = data;
    sendOp(6, req);
}

QString OBSClient::nextId() { return QString("cv-%1").arg(m_idSeq++); }

// ── obs-websocket v5 auth flow ────────────────────────────────────────────────
void OBSClient::handleHello(const QJsonObject &d)
{
    QJsonObject identify{{"rpcVersion", 1}};

    if (d.contains("authentication") && !m_cfg.password.isEmpty()) {
        const QJsonObject auth = d["authentication"].toObject();
        identify["authentication"] = computeAuth(
            m_cfg.password, auth["salt"].toString(), auth["challenge"].toString());
    } else if (d.contains("authentication")) {
        emit log("Server requires auth but no password configured.");
        emit errorOccurred("OBS requires a password but none was set.");
    }
    sendOp(1, identify);
}

QString OBSClient::computeAuth(const QString &password,
                               const QString &salt,
                               const QString &challenge)
{
    QCryptographicHash h1(QCryptographicHash::Sha256);
    h1.addData((password + salt).toUtf8());
    const QByteArray step1 = h1.result().toBase64();

    QCryptographicHash h2(QCryptographicHash::Sha256);
    h2.addData(step1 + challenge.toUtf8());
    return QString::fromLatin1(h2.result().toBase64());
}

// ── Response dispatch ─────────────────────────────────────────────────────────
void OBSClient::handleResponse(const QJsonObject &d)
{
    const QString id   = d["requestId"].toString();
    const QString type = m_pending.take(id);
    const QJsonObject status = d["requestStatus"].toObject();
    if (!status["result"].toBool()) {
        if (type == "GetSceneItemList")
            m_pendingSceneItemLists.remove(id);
        const QString comment = status["comment"].toString();
        emit log(QStringLiteral("%1 failed: %2").arg(type, comment));
        return;
    }

    const QJsonObject rd = d["responseData"].toObject();

    if (type == "GetSceneList") {
        QStringList names;
        for (const auto &v : rd["scenes"].toArray())
            names.prepend(v.toObject()["sceneName"].toString());
        m_knownScenes.clear();
        for (const QString &name : names) {
            if (!name.isEmpty())
                m_knownScenes.insert(name);
        }
        m_receivedSceneList = true;
        if (m_receivedInputList && !m_inventoryReadyEmitted) {
            m_inventoryReadyEmitted = true;
            emit inventoryReady();
        }
        emit scenesReceived(names);
        emit log(QStringLiteral("Scenes: %1").arg(names.join(", ")));
    }
    else if (type == "GetInputList") {
        QSet<QString> names;
        for (const auto &v : rd["inputs"].toArray()) {
            const QString name = v.toObject()["inputName"].toString();
            if (!name.isEmpty())
                names.insert(name);
        }
        m_knownInputs = names;
        m_receivedInputList = true;
        if (m_receivedSceneList && !m_inventoryReadyEmitted) {
            m_inventoryReadyEmitted = true;
            emit inventoryReady();
        }
        emit log(QStringLiteral("Inputs cached: %1.").arg(m_knownInputs.size()));
    }
    else if (type == "GetVirtualCamStatus") {
        const bool active = rd["outputActive"].toBool();
        if (m_virtualCamActive != active) {
            m_virtualCamActive = active;
            emit virtualCamStateChanged(active);
        }
        emit log(QStringLiteral("Virtual camera: %1.").arg(active ? "active" : "inactive"));
    }
    else if (type == "StartVirtualCam") {
        m_virtualCamActive = true;
        emit virtualCamStateChanged(true);
        emit log("Virtual camera started.");
    }
    else if (type == "StopVirtualCam") {
        m_virtualCamActive = false;
        emit virtualCamStateChanged(false);
        emit log("Virtual camera stopped.");
    }
    else if (type == "GetSceneItemList") {
        const QString scene = m_pendingSceneItemLists.take(id);
        QVector<SceneItem> items;
        QHash<QString, int> nameToId;
        for (const auto &v : rd["sceneItems"].toArray()) {
            const QJsonObject o = v.toObject();
            SceneItem si;
            si.sceneItemId = o["sceneItemId"].toInt();
            si.sourceName  = o["sourceName"].toString();
            si.enabled     = o["sceneItemEnabled"].toBool(true);
            items.append(si);
            nameToId.insert(si.sourceName, si.sceneItemId);
        }
        m_itemCache.insert(scene, nameToId);
        m_sceneItems.insert(scene, items);
        emit sceneItemsReceived(scene, items);
        emit log(QStringLiteral("Scene '%1': %2 items cached.").arg(scene).arg(items.size()));
    }
}

void OBSClient::handleEvent(const QJsonObject &d)
{
    const QString type = d["eventType"].toString();
    if (type == "CurrentProgramSceneChanged") {
        emit sceneChanged(d["eventData"].toObject()["sceneName"].toString());
    }
    else if (type == "SceneItemCreated" || type == "SceneItemRemoved") {
        const QString scene = d["eventData"].toObject()["sceneName"].toString();
        m_itemCache.remove(scene);
        m_sceneItems.remove(scene);
    }
    else if (type == "SceneCreated") {
        const QString scene = d["eventData"].toObject()["sceneName"].toString();
        if (!scene.isEmpty())
            m_knownScenes.insert(scene);
    }
    else if (type == "SceneRemoved") {
        const QString scene = d["eventData"].toObject()["sceneName"].toString();
        if (!scene.isEmpty())
            m_knownScenes.remove(scene);
    }
    else if (type == "InputCreated") {
        const QString input = d["eventData"].toObject()["inputName"].toString();
        if (!input.isEmpty())
            m_knownInputs.insert(input);
    }
    else if (type == "InputRemoved") {
        const QString input = d["eventData"].toObject()["inputName"].toString();
        if (!input.isEmpty())
            m_knownInputs.remove(input);
    }
    else if (type == "VirtualcamStateChanged") {
        const bool active = d["eventData"].toObject()["outputActive"].toBool();
        if (m_virtualCamActive != active) {
            m_virtualCamActive = active;
            emit virtualCamStateChanged(active);
            emit log(QStringLiteral("Virtual camera %1.").arg(active ? "started" : "stopped"));
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
void OBSClient::requestSceneList()
{
    if (m_state == State::Connected) sendRequest("GetSceneList");
}

void OBSClient::requestSceneItems(const QString &sceneName)
{
    if (m_state == State::Connected) {
        const QString id = nextId();
        m_pendingSceneItemLists.insert(id, sceneName);
        sendRequest("GetSceneItemList", {{"sceneName", sceneName}}, id);
    }
}

void OBSClient::requestInputList()
{
    if (m_state == State::Connected)
        sendRequest("GetInputList");
}

void OBSClient::refreshInventory()
{
    requestSceneList();
    requestInputList();
}

void OBSClient::setCurrentScene(const QString &name)
{
    if (m_state == State::Connected)
        sendRequest("SetCurrentProgramScene", {{"sceneName", name}});
}

void OBSClient::setCurrentPreviewScene(const QString &name)
{
    if (m_state == State::Connected)
        sendRequest("SetCurrentPreviewScene", {{"sceneName", name}});
}

int OBSClient::resolveItemId(const QString &scene, const QString &source) const
{
    auto it = m_itemCache.find(scene);
    if (it == m_itemCache.end()) return -1;
    return it->value(source, -1);
}

OBSClient::CoreVideoSyncStatus
OBSClient::coreVideoSyncStatus(const QStringList &participantSources,
                               const QStringList &lookScenes) const
{
    CoreVideoSyncStatus status;
    status.inventoryReady = m_receivedSceneList && m_receivedInputList;
    status.expectedParticipantSources = participantSources.size();
    status.expectedSlotScenes = participantSources.size();
    status.expectedLookScenes = lookScenes.size();

    for (const QString &source : participantSources) {
        const QString name = source.trimmed();
        if (name.isEmpty())
            continue;
        if (m_knownInputs.contains(name))
            ++status.presentParticipantSources;
        else
            status.missingInputs << name;
    }

    if (!m_knownScenes.contains(kCoreVideoSourcesScene))
        status.missingScenes << kCoreVideoSourcesScene;

    for (int i = 0; i < participantSources.size(); ++i) {
        const QString slotScene = coreVideoSlotSceneName(i);
        if (m_knownScenes.contains(slotScene))
            ++status.presentSlotScenes;
        else
            status.missingScenes << slotScene;
    }

    for (const QString &scene : lookScenes) {
        const QString name = scene.trimmed();
        if (name.isEmpty())
            continue;
        if (m_knownScenes.contains(name))
            ++status.presentLookScenes;
        else
            status.missingScenes << name;
    }

    return status;
}

void OBSClient::enqueueCreateSceneIfMissing(QJsonArray &requests, const QString &sceneName)
{
    const QString name = sceneName.trimmed();
    if (name.isEmpty() || m_knownScenes.contains(name))
        return;

    requests.append(QJsonObject{
        {"requestType", "CreateScene"},
        {"requestId",   nextId()},
        {"requestData", QJsonObject{{"sceneName", name}}},
    });
    m_knownScenes.insert(name);
}

void OBSClient::enqueueCreateInputIfMissing(QJsonArray &requests,
                                            const QString &sceneName,
                                            const QString &inputName,
                                            const QString &inputKind,
                                            const QJsonObject &inputSettings)
{
    const QString name = inputName.trimmed();
    if (sceneName.trimmed().isEmpty() || name.isEmpty() || m_knownInputs.contains(name))
        return;

    requests.append(QJsonObject{
        {"requestType", "CreateInput"},
        {"requestId",   nextId()},
        {"requestData", QJsonObject{
            {"sceneName",        sceneName},
            {"inputName",        name},
            {"inputKind",        inputKind},
            {"inputSettings",    inputSettings},
            {"sceneItemEnabled", true},
        }},
    });
    m_knownInputs.insert(name);
}

QString OBSClient::overlaySourceName(const QString &sceneName, int index)
{
    QString safe = sceneName;
    safe.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("-"));
    safe.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return QStringLiteral("CoreVideo Overlay - %1 - %2").arg(safe.left(48)).arg(index + 1);
}

// ── Transform ─────────────────────────────────────────────────────────────────
QJsonObject OBSClient::Transform::toJson() const
{
    QJsonObject t;
    if (positionX)    t["positionX"]    = *positionX;
    if (positionY)    t["positionY"]    = *positionY;
    if (scaleX)       t["scaleX"]       = *scaleX;
    if (scaleY)       t["scaleY"]       = *scaleY;
    if (rotation)     t["rotation"]     = *rotation;
    if (cropLeft)     t["cropLeft"]     = *cropLeft;
    if (cropRight)    t["cropRight"]    = *cropRight;
    if (cropTop)      t["cropTop"]      = *cropTop;
    if (cropBottom)   t["cropBottom"]   = *cropBottom;
    if (boundsWidth)  t["boundsWidth"]  = *boundsWidth;
    if (boundsHeight) t["boundsHeight"] = *boundsHeight;
    if (boundsType)   t["boundsType"]   = *boundsType;
    if (alignment)    t["alignment"]    = *alignment;
    return t;
}

void OBSClient::setSceneItemTransform(const QString  &sceneName,
                                      int             sceneItemId,
                                      const Transform &t)
{
    if (m_state != State::Connected) return;
    sendRequest("SetSceneItemTransform", {
        {"sceneName",          sceneName},
        {"sceneItemId",        sceneItemId},
        {"sceneItemTransform", t.toJson()},
    });
}

// ── Macro execution ───────────────────────────────────────────────────────────
void OBSClient::executeMacro(const Macro &macro)
{
    if (m_state != State::Connected) return;
    QJsonArray requests;
    for (const auto &step : macro.steps) {
        QJsonObject req{
            {"requestType", step.requestType},
            {"requestId",   nextId()},
        };
        if (!step.data.isEmpty()) req["requestData"] = step.data;
        requests.append(req);
    }
    if (requests.isEmpty()) return;
    sendOp(8, QJsonObject{
        {"requestId",     nextId()},
        {"haltOnFailure", false},
        {"requests",      requests},
    });
    emit log(QStringLiteral("Macro '%1': dispatched %2 steps.")
                 .arg(macro.label).arg(requests.size()));
}

// ── Virtual camera ────────────────────────────────────────────────────────────
void OBSClient::requestVirtualCamStatus()
{
    if (m_state == State::Connected) sendRequest("GetVirtualCamStatus");
}

void OBSClient::startVirtualCam()
{
    if (m_state == State::Connected) sendRequest("StartVirtualCam");
}

void OBSClient::stopVirtualCam()
{
    if (m_state == State::Connected) sendRequest("StopVirtualCam");
}

// ── applyLayout (normalized LayoutTemplate) ───────────────────────────────────
void OBSClient::applyLayout(const QString        &sceneName,
                            const LayoutTemplate &tmpl,
                            const QStringList    &sourceNames,
                            double canvasW, double canvasH)
{
    if (m_state != State::Connected) return;

    QJsonArray requests;
    int matched = 0;
    QList<int> usedItemIds;
    QSet<QString> desiredSources;
    for (const QString &source : sourceNames) {
        if (!source.trimmed().isEmpty())
            desiredSources.insert(source.trimmed());
    }

    const auto existingItems = m_sceneItems.value(sceneName);
    for (const SceneItem &item : existingItems) {
        if (!item.sourceName.startsWith(QStringLiteral("CoreVideo Slot ")))
            continue;
        const bool shouldEnable = desiredSources.contains(item.sourceName);
        if (item.enabled == shouldEnable)
            continue;
        requests.append(QJsonObject{
            {"requestType", "SetSceneItemEnabled"},
            {"requestId",   nextId()},
            {"requestData", QJsonObject{
                {"sceneName",        sceneName},
                {"sceneItemId",      item.sceneItemId},
                {"sceneItemEnabled", shouldEnable},
            }},
        });
    }

    for (int i = 0; i < tmpl.slotList.size(); ++i) {
        const TemplateSlot &slot = tmpl.slotList[i];
        const int sourceIndex = (slot.index >= 0 && slot.index < sourceNames.size())
                                    ? slot.index
                                    : i;
        if (sourceIndex < 0 || sourceIndex >= sourceNames.size()) continue;

        const QString &sourceName = sourceNames[sourceIndex];
        int itemId = resolveItemId(sceneName, sourceName);
        if (itemId < 0) {
            const auto ordered = m_sceneItems.value(sceneName);
            for (const SceneItem &item : ordered) {
                if (usedItemIds.contains(item.sceneItemId)) continue;
                itemId = item.sceneItemId;
                emit log(QStringLiteral("Using scene item '%1' for slot %2.")
                             .arg(item.sourceName).arg(i + 1));
                break;
            }
        }
        if (itemId < 0) {
            emit log(QStringLiteral("Skip '%1' — not in scene '%2'.")
                         .arg(sourceName, sceneName));
            continue;
        }
        ++matched;
        usedItemIds.append(itemId);

        requests.append(QJsonObject{
            {"requestType", "SetSceneItemEnabled"},
            {"requestId",   nextId()},
            {"requestData", QJsonObject{
                {"sceneName",        sceneName},
                {"sceneItemId",      itemId},
                {"sceneItemEnabled", true},
            }},
        });

        const QJsonObject transform{
            {"positionX",    slot.x      * canvasW},
            {"positionY",    slot.y      * canvasH},
            {"boundsWidth",  slot.width  * canvasW},
            {"boundsHeight", slot.height * canvasH},
            {"boundsType",   "OBS_BOUNDS_SCALE_INNER"},
        };

        requests.append(QJsonObject{
            {"requestType", "SetSceneItemTransform"},
            {"requestId",   nextId()},
            {"requestData", QJsonObject{
                {"sceneName",          sceneName},
                {"sceneItemId",        itemId},
                {"sceneItemTransform", transform},
            }},
        });
    }

    if (requests.isEmpty()) {
        emit log("applyLayout: no matching scene items.");
        return;
    }

    sendOp(8, QJsonObject{
        {"requestId",     nextId()},
        {"haltOnFailure", false},
        {"requests",      requests},
    });
    emit log(QStringLiteral("applyLayout: dispatched %1 transforms to '%2'.")
                 .arg(matched).arg(sceneName));
    emit templateApplied(tmpl.name, matched);
}

void OBSClient::loadSceneTemplate(const QString        &sceneName,
                                  const LayoutTemplate &tmpl,
                                  const QStringList    &sourceNames,
                                  double canvasW, double canvasH,
                                  const QVector<Overlay> &overlays,
                                  const QString &backgroundImagePath,
                                  const TileStyle &tileStyle,
                                  const QStringList &slotLabels,
                                  bool makeProgram)
{
    if (m_state != State::Connected) return;
    if (!tmpl.isValid() || sceneName.isEmpty()) {
        emit log("loadSceneTemplate: missing scene or template.");
        return;
    }

    ensureCoreVideoSources(kCoreVideoSourcesScene, sourceNames);
    const QStringList slotSceneNames = coreVideoSlotSceneNames(sourceNames.size());

    QJsonArray requests;
    enqueueCreateSceneIfMissing(requests, sceneName);
    if (!requests.isEmpty()) {
        sendOp(8, QJsonObject{
            {"requestId",     nextId()},
            {"haltOnFailure", false},
            {"requests",      requests},
        });
    }

    m_itemCache.remove(sceneName);
    m_sceneItems.remove(sceneName);
    emit log(QStringLiteral("Loading scene template '%1' into '%2'.")
                 .arg(tmpl.name, sceneName));

    QTimer::singleShot(300, this, [this, sceneName]() {
        requestSceneItems(sceneName);
    });
    QTimer::singleShot(900, this, [this, sceneName, slotSceneNames]() {
        if (!m_itemCache.contains(sceneName)) {
            requestSceneItems(sceneName);
            return;
        }

        QJsonArray requests;
        for (const QString &sourceName : slotSceneNames) {
            if (sourceName.trimmed().isEmpty()) continue;
            if (resolveItemId(sceneName, sourceName) >= 0) continue;

            requests.append(QJsonObject{
                {"requestType", "CreateSceneItem"},
                {"requestId",   nextId()},
                {"requestData", QJsonObject{
                    {"sceneName",        sceneName},
                    {"sourceName",       sourceName},
                    {"sceneItemEnabled", true},
                }},
            });
        }

        if (requests.isEmpty()) return;

        sendOp(8, QJsonObject{
            {"requestId",     nextId()},
            {"haltOnFailure", false},
            {"requests",      requests},
        });
        m_itemCache.remove(sceneName);
        m_sceneItems.remove(sceneName);
        emit log(QStringLiteral("Added %1 nested slot scenes to '%2'.")
                     .arg(requests.size()).arg(sceneName));
    });
    QTimer::singleShot(1300, this, [this, sceneName]() {
        requestSceneItems(sceneName);
    });
    QTimer::singleShot(1750, this, [this, sceneName, tileStyle, canvasW, canvasH]() {
        applyCanvasColor(sceneName, tileStyle, canvasW, canvasH);
    });
    QTimer::singleShot(1800, this, [this, sceneName, tmpl, slotSceneNames, canvasW, canvasH, overlays, makeProgram]() {
        Q_UNUSED(makeProgram);
        applyLayout(sceneName, tmpl, slotSceneNames, canvasW, canvasH);
    });
    QTimer::singleShot(1900, this, [this, sceneName, backgroundImagePath, canvasW, canvasH]() {
        applyBackgroundImage(sceneName, backgroundImagePath, canvasW, canvasH);
    });
    QTimer::singleShot(2000, this, [this, sceneName, tmpl, tileStyle, slotLabels, canvasW, canvasH]() {
        applyTileDecorations(sceneName, tmpl, tileStyle, slotLabels, canvasW, canvasH);
    });
    QTimer::singleShot(2100, this, [this, sceneName, overlays, canvasW, canvasH, makeProgram]() {
        applyOverlays(sceneName, overlays, canvasW, canvasH);
        if (makeProgram)
            setCurrentScene(sceneName);
        requestSceneList();
    });
}

void OBSClient::ensureCoreVideoSources(const QString &sceneName,
                                       const QStringList &sourceNames)
{
    if (m_state != State::Connected || sceneName.isEmpty() || sourceNames.isEmpty())
        return;

    QJsonArray requests;
    enqueueCreateSceneIfMissing(requests, sceneName);

    for (int i = 0; i < sourceNames.size(); ++i) {
        enqueueCreateSceneIfMissing(requests, coreVideoSlotSceneName(i));
    }

    const QVector<double> placeholderColors = {
        0xff1e6ae0u, 0xff20a060u, 0xff9b40d0u, 0xffe06020u,
        0xff2979ffu, 0xffd04090u, 0xffb09020u, 0xff40a0c0u,
    };
    for (int i = 0; i < sourceNames.size(); ++i) {
        enqueueCreateInputIfMissing(requests,
                                    coreVideoSlotSceneName(i),
                                    coreVideoSlotPlaceholderName(i),
                                    QStringLiteral("color_source_v3"),
                                    QJsonObject{
                                        {"color", placeholderColors.value(i % placeholderColors.size())},
                                        {"width", 1920},
                                        {"height", 1080},
                                    });
    }

    for (const QString &source : sourceNames) {
        const QString sourceName = source.trimmed();
        if (sourceName.isEmpty()) continue;
        enqueueCreateInputIfMissing(requests,
                                    sceneName,
                                    sourceName,
                                    coreVideoInputKindForSource(sourceName));
    }

    if (!requests.isEmpty()) {
        sendOp(8, QJsonObject{
            {"requestId",     nextId()},
            {"haltOnFailure", false},
            {"requests",      requests},
        });
    }
    m_itemCache.remove(sceneName);
    m_sceneItems.remove(sceneName);
    emit log(QStringLiteral("Provisioning %1 CoreVideo placeholder sources in '%2'.")
                 .arg(sourceNames.size()).arg(sceneName));

    QTimer::singleShot(600, this, [this, sceneName, sourceNames]() {
        requestSceneItems(sceneName);
        for (int i = 0; i < sourceNames.size(); ++i)
            requestSceneItems(coreVideoSlotSceneName(i));
        requestSceneList();
        requestInputList();
    });
    auto requestedLinks = std::make_shared<QSet<QString>>();
    auto linkSources = [this, sceneName, sourceNames, requestedLinks]() {
        const bool sourceSceneReady = m_itemCache.contains(sceneName);
        if (!sourceSceneReady)
            requestSceneItems(sceneName);

        QJsonArray requests;
        if (sourceSceneReady) {
            for (const QString &source : sourceNames) {
                const QString sourceName = source.trimmed();
                if (sourceName.isEmpty()) continue;
                if (resolveItemId(sceneName, sourceName) >= 0) continue;
                const QString linkKey = sceneName + QStringLiteral("\n") + sourceName;
                if (requestedLinks->contains(linkKey)) continue;

                requests.append(QJsonObject{
                    {"requestType", "CreateSceneItem"},
                    {"requestId",   nextId()},
                    {"requestData", QJsonObject{
                        {"sceneName",        sceneName},
                        {"sourceName",       sourceName},
                        {"sceneItemEnabled", true},
                    }},
                });
                requestedLinks->insert(linkKey);
            }
        }

        for (int i = 0; i < sourceNames.size(); ++i) {
            const QString sourceName = sourceNames[i].trimmed();
            if (sourceName.isEmpty()) continue;
            const QString slotScene = coreVideoSlotSceneName(i);
            if (!m_itemCache.contains(slotScene)) {
                requestSceneItems(slotScene);
                continue;
            }
            const QString placeholderName = coreVideoSlotPlaceholderName(i);
            if (resolveItemId(slotScene, placeholderName) < 0) {
                const QString linkKey = slotScene + QStringLiteral("\n") + placeholderName;
                if (!requestedLinks->contains(linkKey)) {
                    requests.append(QJsonObject{
                        {"requestType", "CreateSceneItem"},
                        {"requestId",   nextId()},
                        {"requestData", QJsonObject{
                            {"sceneName",        slotScene},
                            {"sourceName",       placeholderName},
                            {"sceneItemEnabled", true},
                        }},
                    });
                    requestedLinks->insert(linkKey);
                }
            }
            if (resolveItemId(slotScene, sourceName) >= 0)
                continue;
            const QString linkKey = slotScene + QStringLiteral("\n") + sourceName;
            if (requestedLinks->contains(linkKey)) continue;

            requests.append(QJsonObject{
                {"requestType", "CreateSceneItem"},
                {"requestId",   nextId()},
                {"requestData", QJsonObject{
                    {"sceneName",        slotScene},
                    {"sourceName",       sourceName},
                    {"sceneItemEnabled", true},
                }},
            });
            requestedLinks->insert(linkKey);
        }

        if (requests.isEmpty()) return;
        sendOp(8, QJsonObject{
            {"requestId",     nextId()},
            {"haltOnFailure", false},
            {"requests",      requests},
        });
        m_itemCache.remove(sceneName);
        m_sceneItems.remove(sceneName);
        for (int i = 0; i < sourceNames.size(); ++i) {
            m_itemCache.remove(coreVideoSlotSceneName(i));
            m_sceneItems.remove(coreVideoSlotSceneName(i));
        }
        emit log(QStringLiteral("Linked %1 CoreVideo source items into shared and slot scenes.")
                     .arg(requests.size()));
    };
    QTimer::singleShot(1000, this, linkSources);
    QTimer::singleShot(1800, this, linkSources);
    QTimer::singleShot(2800, this, linkSources);
    QTimer::singleShot(1400, this, [this, sceneName, sourceNames]() {
        requestSceneItems(sceneName);
        for (int i = 0; i < sourceNames.size(); ++i)
            requestSceneItems(coreVideoSlotSceneName(i));
        requestSceneList();
        requestInputList();
    });
    QTimer::singleShot(3200, this, [this, sceneName, sourceNames]() {
        requestSceneItems(sceneName);
        for (int i = 0; i < sourceNames.size(); ++i)
            requestSceneItems(coreVideoSlotSceneName(i));
        requestSceneList();
        requestInputList();
    });
}

// ── applyTemplate (flat applied JSON) ─────────────────────────────────────────
void OBSClient::removeStaleCoreVideoDuplicates(const QStringList &participantSources,
                                               const QStringList &lookScenes)
{
    if (m_state != State::Connected)
        return;

    QSet<QString> canonicalScenes;
    canonicalScenes.insert(kCoreVideoSourcesScene);
    for (int i = 0; i < participantSources.size(); ++i)
        canonicalScenes.insert(coreVideoSlotSceneName(i));
    for (const QString &scene : lookScenes) {
        if (!scene.trimmed().isEmpty())
            canonicalScenes.insert(scene.trimmed());
    }

    QSet<QString> canonicalInputs;
    for (const QString &source : participantSources) {
        if (!source.trimmed().isEmpty())
            canonicalInputs.insert(source.trimmed());
    }
    for (const QString &scene : lookScenes) {
        for (int i = 0; i < 8; ++i)
            canonicalInputs.insert(overlaySourceName(scene, i));
    }

    QJsonArray requests;
    const QRegularExpression staleScenePattern(
        QStringLiteral("^(CoreVideo Sources|CoreVideo Slot \\d+|CoreVideo - .+) \\d+$"));
    const QRegularExpression staleInputPattern(
        QStringLiteral("^(Zoom Participant \\d+|CoreVideo Overlay(?: - .+)?) \\d+$"));

    for (const QString &scene : m_knownScenes) {
        if (canonicalScenes.contains(scene))
            continue;
        if (!staleScenePattern.match(scene).hasMatch())
            continue;
        requests.append(QJsonObject{
            {"requestType", "RemoveScene"},
            {"requestId",   nextId()},
            {"requestData", QJsonObject{{"sceneName", scene}}},
        });
    }

    for (const QString &input : m_knownInputs) {
        if (canonicalInputs.contains(input))
            continue;
        if (input == "CoreVideo Overlay 1" || input == "CoreVideo Overlay 2"
            || staleInputPattern.match(input).hasMatch()) {
            requests.append(QJsonObject{
                {"requestType", "RemoveInput"},
                {"requestId",   nextId()},
                {"requestData", QJsonObject{{"inputName", input}}},
            });
        }
    }

    for (auto it = m_sceneItems.constBegin(); it != m_sceneItems.constEnd(); ++it) {
        const QString scene = it.key();
        if (!(scene == kCoreVideoSourcesScene
              || scene.startsWith(QStringLiteral("CoreVideo Slot "))
              || scene.startsWith(QStringLiteral("CoreVideo - ")))) {
            continue;
        }

        QSet<QString> seenSources;
        for (const SceneItem &item : it.value()) {
            if (item.sourceName.trimmed().isEmpty())
                continue;
            if (!seenSources.contains(item.sourceName)) {
                seenSources.insert(item.sourceName);
                continue;
            }
            requests.append(QJsonObject{
                {"requestType", "RemoveSceneItem"},
                {"requestId",   nextId()},
                {"requestData", QJsonObject{
                    {"sceneName",   scene},
                    {"sceneItemId", item.sceneItemId},
                }},
            });
        }
    }

    if (requests.isEmpty()) {
        emit log("Repair: no stale CoreVideo duplicates found.");
        return;
    }

    sendOp(8, QJsonObject{
        {"requestId",     nextId()},
        {"haltOnFailure", false},
        {"requests",      requests},
    });
    emit log(QStringLiteral("Repair: removing %1 stale CoreVideo duplicate(s).")
                 .arg(requests.size()));
    QTimer::singleShot(800, this, [this]() { refreshInventory(); });
}

void OBSClient::applyBackgroundImage(const QString &sceneName,
                                     const QString &imagePath,
                                     double canvasW, double canvasH)
{
    if (m_state != State::Connected || sceneName.isEmpty() || imagePath.trimmed().isEmpty())
        return;

    const QFileInfo info(imagePath);
    if (!info.exists() || !info.isFile()) {
        emit log(QStringLiteral("Background skipped: file not found '%1'.").arg(imagePath));
        return;
    }

    const QString sourceName = coreVideoBackgroundSourceName(sceneName);
    const QString nativePath = QDir::toNativeSeparators(info.absoluteFilePath());
    const QJsonObject inputSettings{{"file", nativePath}};

    QJsonArray requests;
    enqueueCreateInputIfMissing(requests,
                                sceneName,
                                sourceName,
                                QStringLiteral("image_source"),
                                inputSettings);
    requests.append(QJsonObject{
        {"requestType", "SetInputSettings"},
        {"requestId",   nextId()},
        {"requestData", QJsonObject{
            {"inputName",     sourceName},
            {"inputSettings", inputSettings},
            {"overlay",       true},
        }},
    });

    if (!requests.isEmpty()) {
        sendOp(8, QJsonObject{
            {"requestId",     nextId()},
            {"haltOnFailure", false},
            {"requests",      requests},
        });
        m_itemCache.remove(sceneName);
        m_sceneItems.remove(sceneName);
    }

    QTimer::singleShot(500, this, [this, sceneName]() {
        requestSceneItems(sceneName);
    });
    QTimer::singleShot(1000, this, [this, sceneName, sourceName, canvasW, canvasH]() {
        if (!m_itemCache.contains(sceneName)) {
            requestSceneItems(sceneName);
            return;
        }

        QJsonArray bgRequests;
        int itemId = resolveItemId(sceneName, sourceName);
        if (itemId < 0) {
            bgRequests.append(QJsonObject{
                {"requestType", "CreateSceneItem"},
                {"requestId",   nextId()},
                {"requestData", QJsonObject{
                    {"sceneName",        sceneName},
                    {"sourceName",       sourceName},
                    {"sceneItemEnabled", true},
                }},
            });
            QTimer::singleShot(650, this, [this, sceneName, sourceName, canvasW, canvasH]() {
                requestSceneItems(sceneName);
                const int refreshedId = resolveItemId(sceneName, sourceName);
                if (refreshedId < 0)
                    return;
                QJsonArray followup;
                followup.append(QJsonObject{
                    {"requestType", "SetSceneItemIndex"},
                    {"requestId",   nextId()},
                    {"requestData", QJsonObject{
                        {"sceneName",      sceneName},
                        {"sceneItemId",    refreshedId},
                        {"sceneItemIndex", 0},
                    }},
                });
                followup.append(QJsonObject{
                    {"requestType", "SetSceneItemTransform"},
                    {"requestId",   nextId()},
                    {"requestData", QJsonObject{
                        {"sceneName",   sceneName},
                        {"sceneItemId", refreshedId},
                        {"sceneItemTransform", QJsonObject{
                            {"positionX",    0},
                            {"positionY",    0},
                            {"boundsWidth",  canvasW},
                            {"boundsHeight", canvasH},
                            {"boundsType",   "OBS_BOUNDS_SCALE_OUTER"},
                            {"alignment",    5},
                        }},
                    }},
                });
                sendOp(8, QJsonObject{
                    {"requestId",     nextId()},
                    {"haltOnFailure", false},
                    {"requests",      followup},
                });
            });
        } else {
            bgRequests.append(QJsonObject{
                {"requestType", "SetSceneItemEnabled"},
                {"requestId",   nextId()},
                {"requestData", QJsonObject{
                    {"sceneName",        sceneName},
                    {"sceneItemId",      itemId},
                    {"sceneItemEnabled", true},
                }},
            });
            bgRequests.append(QJsonObject{
                {"requestType", "SetSceneItemIndex"},
                {"requestId",   nextId()},
                {"requestData", QJsonObject{
                    {"sceneName",      sceneName},
                    {"sceneItemId",    itemId},
                    {"sceneItemIndex", 0},
                }},
            });
            bgRequests.append(QJsonObject{
                {"requestType", "SetSceneItemTransform"},
                {"requestId",   nextId()},
                {"requestData", QJsonObject{
                    {"sceneName",   sceneName},
                    {"sceneItemId", itemId},
                    {"sceneItemTransform", QJsonObject{
                        {"positionX",    0},
                        {"positionY",    0},
                        {"boundsWidth",  canvasW},
                        {"boundsHeight", canvasH},
                        {"boundsType",   "OBS_BOUNDS_SCALE_OUTER"},
                        {"alignment",    5},
                    }},
                }},
            });
        }

        if (bgRequests.isEmpty()) return;
        sendOp(8, QJsonObject{
            {"requestId",     nextId()},
            {"haltOnFailure", false},
            {"requests",      bgRequests},
        });
        emit log(QStringLiteral("Background applied to '%1'.").arg(sceneName));
    });
}

void OBSClient::applyCanvasColor(const QString &sceneName,
                                 const TileStyle &tileStyle,
                                 double canvasW, double canvasH)
{
    if (m_state != State::Connected || sceneName.isEmpty())
        return;

    const QString sourceName = coreVideoCanvasSourceName(sceneName);
    const QJsonObject inputSettings{
        {"color", obsColor(tileStyle.canvasColor)},
        {"width", int(canvasW)},
        {"height", int(canvasH)},
    };

    QJsonArray requests;
    enqueueCreateInputIfMissing(requests,
                                sceneName,
                                sourceName,
                                QStringLiteral("color_source_v3"),
                                inputSettings);
    requests.append(QJsonObject{
        {"requestType", "SetInputSettings"},
        {"requestId",   nextId()},
        {"requestData", QJsonObject{
            {"inputName",     sourceName},
            {"inputSettings", inputSettings},
            {"overlay",       true},
        }},
    });

    if (!requests.isEmpty()) {
        sendOp(8, QJsonObject{
            {"requestId",     nextId()},
            {"haltOnFailure", false},
            {"requests",      requests},
        });
        m_itemCache.remove(sceneName);
        m_sceneItems.remove(sceneName);
    }

    QTimer::singleShot(700, this, [this, sceneName, sourceName, canvasW, canvasH]() {
        if (!m_itemCache.contains(sceneName)) {
            requestSceneItems(sceneName);
            return;
        }

        QJsonArray requests;
        int itemId = resolveItemId(sceneName, sourceName);
        if (itemId < 0) {
            requests.append(QJsonObject{
                {"requestType", "CreateSceneItem"},
                {"requestId",   nextId()},
                {"requestData", QJsonObject{
                    {"sceneName",        sceneName},
                    {"sourceName",       sourceName},
                    {"sceneItemEnabled", true},
                }},
            });
        } else {
            requests.append(QJsonObject{
                {"requestType", "SetSceneItemIndex"},
                {"requestId",   nextId()},
                {"requestData", QJsonObject{
                    {"sceneName",      sceneName},
                    {"sceneItemId",    itemId},
                    {"sceneItemIndex", 0},
                }},
            });
            requests.append(QJsonObject{
                {"requestType", "SetSceneItemTransform"},
                {"requestId",   nextId()},
                {"requestData", QJsonObject{
                    {"sceneName",   sceneName},
                    {"sceneItemId", itemId},
                    {"sceneItemTransform", QJsonObject{
                        {"positionX",    0},
                        {"positionY",    0},
                        {"boundsWidth",  canvasW},
                        {"boundsHeight", canvasH},
                        {"boundsType",   "OBS_BOUNDS_STRETCH"},
                        {"alignment",    5},
                    }},
                }},
            });
        }

        if (requests.isEmpty()) return;
        sendOp(8, QJsonObject{
            {"requestId",     nextId()},
            {"haltOnFailure", false},
            {"requests",      requests},
        });
    });
}

void OBSClient::applyTileDecorations(const QString &sceneName,
                                     const LayoutTemplate &tmpl,
                                     const TileStyle &tileStyle,
                                     const QStringList &slotLabels,
                                     double canvasW, double canvasH)
{
    if (m_state != State::Connected || sceneName.isEmpty() || !tmpl.isValid())
        return;

    QJsonArray createRequests;
    const bool showBorder = tileStyle.borderWidth > 0.0;
    for (const auto &slot : tmpl.slotList) {
        if (showBorder) {
            enqueueCreateInputIfMissing(createRequests,
                                        sceneName,
                                        coreVideoBorderSourceName(sceneName, slot.index),
                                        QStringLiteral("color_source_v3"),
                                        QJsonObject{
                                            {"color", obsColor(tileStyle.borderColor)},
                                            {"width", int(canvasW)},
                                            {"height", int(canvasH)},
                                        });
        }
        if (tileStyle.showNameTag) {
            const QString label = slot.index >= 0 && slot.index < slotLabels.size()
                ? slotLabels.value(slot.index)
                : QStringLiteral("Slot %1").arg(slot.index + 1);
            enqueueCreateInputIfMissing(createRequests,
                                        sceneName,
                                        coreVideoNameTagSourceName(sceneName, slot.index),
                                        QStringLiteral("text_gdiplus_v3"),
                                        QJsonObject{
                                            {"text", label},
                                            {"color", -1},
                                            {"bk_color", 0},
                                            {"bk_opacity", 180},
                                            {"font", QJsonObject{
                                                {"face", "Arial"},
                                                {"size", 28},
                                                {"style", "Bold"},
                                            }},
                                        });
        }
    }

    if (!createRequests.isEmpty()) {
        sendOp(8, QJsonObject{
            {"requestId",     nextId()},
            {"haltOnFailure", false},
            {"requests",      createRequests},
        });
        m_itemCache.remove(sceneName);
        m_sceneItems.remove(sceneName);
    }

    QTimer::singleShot(850, this, [this, sceneName, tmpl, tileStyle, slotLabels, canvasW, canvasH, showBorder]() {
        if (!m_itemCache.contains(sceneName)) {
            requestSceneItems(sceneName);
            return;
        }

        QJsonArray requests;
        for (const auto &slot : tmpl.slotList) {
            if (showBorder) {
                const QString borderName = coreVideoBorderSourceName(sceneName, slot.index);
                int borderId = resolveItemId(sceneName, borderName);
                if (borderId < 0) {
                    requests.append(QJsonObject{
                        {"requestType", "CreateSceneItem"},
                        {"requestId",   nextId()},
                        {"requestData", QJsonObject{
                            {"sceneName",        sceneName},
                            {"sourceName",       borderName},
                            {"sceneItemEnabled", true},
                        }},
                    });
                } else {
                    const double bw = tileStyle.borderWidth;
                    requests.append(QJsonObject{
                        {"requestType", "SetSceneItemTransform"},
                        {"requestId",   nextId()},
                        {"requestData", QJsonObject{
                            {"sceneName",   sceneName},
                            {"sceneItemId", borderId},
                            {"sceneItemTransform", QJsonObject{
                                {"positionX",    slot.x * canvasW - bw},
                                {"positionY",    slot.y * canvasH - bw},
                                {"boundsWidth",  slot.width * canvasW + bw * 2.0},
                                {"boundsHeight", slot.height * canvasH + bw * 2.0},
                                {"boundsType",   "OBS_BOUNDS_STRETCH"},
                            }},
                        }},
                    });
                }
            }

            if (!tileStyle.showNameTag)
                continue;

            const QString nameTag = coreVideoNameTagSourceName(sceneName, slot.index);
            const QString label = slot.index >= 0 && slot.index < slotLabels.size()
                ? slotLabels.value(slot.index)
                : QStringLiteral("Slot %1").arg(slot.index + 1);
            int nameId = resolveItemId(sceneName, nameTag);
            requests.append(QJsonObject{
                {"requestType", "SetInputSettings"},
                {"requestId",   nextId()},
                {"requestData", QJsonObject{
                    {"inputName", nameTag},
                    {"inputSettings", QJsonObject{{"text", label}}},
                    {"overlay", true},
                }},
            });
            if (nameId < 0) {
                requests.append(QJsonObject{
                    {"requestType", "CreateSceneItem"},
                    {"requestId",   nextId()},
                    {"requestData", QJsonObject{
                        {"sceneName",        sceneName},
                        {"sourceName",       nameTag},
                        {"sceneItemEnabled", true},
                    }},
                });
            } else {
                const double tagH = qMax(36.0, slot.height * canvasH * 0.12);
                requests.append(QJsonObject{
                    {"requestType", "SetSceneItemTransform"},
                    {"requestId",   nextId()},
                    {"requestData", QJsonObject{
                        {"sceneName",   sceneName},
                        {"sceneItemId", nameId},
                        {"sceneItemTransform", QJsonObject{
                            {"positionX",    slot.x * canvasW + 12.0},
                            {"positionY",    (slot.y + slot.height) * canvasH - tagH - 10.0},
                            {"boundsWidth",  qMax(80.0, slot.width * canvasW - 24.0)},
                            {"boundsHeight", tagH},
                            {"boundsType",   "OBS_BOUNDS_SCALE_INNER"},
                        }},
                    }},
                });
            }
        }

        if (requests.isEmpty()) return;
        sendOp(8, QJsonObject{
            {"requestId",     nextId()},
            {"haltOnFailure", false},
            {"requests",      requests},
        });
        emit log(QStringLiteral("Applied tile design layers to '%1'.").arg(sceneName));
    });
}

void OBSClient::applyOverlays(const QString &sceneName,
                              const QVector<Overlay> &overlays,
                              double canvasW, double canvasH)
{
    if (m_state != State::Connected || sceneName.isEmpty())
        return;

    QJsonArray requests;
    for (int i = 0; i < overlays.size(); ++i) {
        const Overlay &ov = overlays[i];
        const QString sourceName = overlaySourceName(sceneName, i);
        const QString label = ov.text2.isEmpty()
            ? ov.text1
            : QStringLiteral("%1\n%2").arg(ov.text1, ov.text2);
        if (label.trimmed().isEmpty())
            continue;

        QJsonObject inputSettings{
            {"text", label},
            {"color", -1},
            {"bk_color", 0},
            {"bk_opacity", 0},
            {"outline", false},
            {"drop_shadow", true},
            {"font", QJsonObject{
                {"face", "Arial"},
                {"size", ov.type == Overlay::Bug ? 26 : 42},
                {"style", "Bold"},
            }},
        };

        enqueueCreateInputIfMissing(requests,
                                    sceneName,
                                    sourceName,
                                    QStringLiteral("text_gdiplus_v3"),
                                    inputSettings);
        requests.append(QJsonObject{
            {"requestType", "SetInputSettings"},
            {"requestId",   nextId()},
            {"requestData", QJsonObject{
                {"inputName",     sourceName},
                {"inputSettings", inputSettings},
                {"overlay",       true},
            }},
        });
    }

    if (!requests.isEmpty()) {
        sendOp(8, QJsonObject{
            {"requestId",     nextId()},
            {"haltOnFailure", false},
            {"requests",      requests},
        });
        m_itemCache.remove(sceneName);
        m_sceneItems.remove(sceneName);
        emit log(QStringLiteral("Applying %1 overlay source(s) to '%2'.")
                     .arg(overlays.size()).arg(sceneName));
    }

    QTimer::singleShot(600, this, [this, sceneName]() {
        requestSceneItems(sceneName);
    });
    QTimer::singleShot(1100, this, [this, sceneName, overlays, canvasW, canvasH]() {
        if (!m_itemCache.contains(sceneName)) {
            requestSceneItems(sceneName);
            return;
        }

        QJsonArray transformRequests;
        for (int i = 0; i < overlays.size(); ++i) {
            const Overlay &ov = overlays[i];
            const QString sourceName = overlaySourceName(sceneName, i);
            const int itemId = resolveItemId(sceneName, sourceName);
            if (itemId < 0) {
                transformRequests.append(QJsonObject{
                    {"requestType", "CreateSceneItem"},
                    {"requestId",   nextId()},
                    {"requestData", QJsonObject{
                        {"sceneName",        sceneName},
                        {"sourceName",       sourceName},
                        {"sceneItemEnabled", true},
                    }},
                });
                continue;
            }

            transformRequests.append(QJsonObject{
                {"requestType", "SetSceneItemTransform"},
                {"requestId",   nextId()},
                {"requestData", QJsonObject{
                    {"sceneName",   sceneName},
                    {"sceneItemId", itemId},
                    {"sceneItemTransform", QJsonObject{
                        {"positionX",    ov.x * canvasW},
                        {"positionY",    ov.y * canvasH},
                        {"boundsWidth",  ov.w * canvasW},
                        {"boundsHeight", ov.h * canvasH},
                        {"boundsType",   "OBS_BOUNDS_SCALE_INNER"},
                        {"alignment",    5},
                    }},
                }},
            });
        }

        if (transformRequests.isEmpty()) return;
        sendOp(8, QJsonObject{
            {"requestId",     nextId()},
            {"haltOnFailure", false},
            {"requests",      transformRequests},
        });
    });
}

bool OBSClient::applyTemplate(const QJsonObject &templateJson)
{
    if (m_state != State::Connected) {
        emit log("applyTemplate: not connected.");
        return false;
    }
    const QString name  = templateJson.value("name").toString();
    const QString scene = templateJson.value("scene").toString();
    const QJsonArray items = templateJson.value("items").toArray();
    if (scene.isEmpty() || items.isEmpty()) {
        emit log("applyTemplate: missing 'scene' or 'items'.");
        return false;
    }

    if (!m_itemCache.contains(scene)) {
        // Auto-fetch and re-attempt is left to the caller; for now we just
        // request and bail so the user can press Apply again once ready.
        requestSceneItems(scene);
        emit log(QStringLiteral("applyTemplate: fetching scene items for '%1' — try again.").arg(scene));
        return false;
    }

    QJsonArray requests;
    int matched = 0, skipped = 0;
    for (const auto &v : items) {
        const QJsonObject item = v.toObject();
        const QString src = item.value("source").toString();
        const int itemId = resolveItemId(scene, src);
        if (itemId < 0) { ++skipped; continue; }
        ++matched;

        Transform t;
        if (item.contains("x"))            t.positionX    = item["x"].toDouble();
        if (item.contains("y"))            t.positionY    = item["y"].toDouble();
        if (item.contains("scale")) {
            const double s = item["scale"].toDouble();
            t.scaleX = s; t.scaleY = s;
        }
        if (item.contains("scaleX"))       t.scaleX       = item["scaleX"].toDouble();
        if (item.contains("scaleY"))       t.scaleY       = item["scaleY"].toDouble();
        if (item.contains("rotation"))     t.rotation     = item["rotation"].toDouble();
        if (item.contains("cropLeft"))     t.cropLeft     = item["cropLeft"].toDouble();
        if (item.contains("cropRight"))    t.cropRight    = item["cropRight"].toDouble();
        if (item.contains("cropTop"))      t.cropTop      = item["cropTop"].toDouble();
        if (item.contains("cropBottom"))   t.cropBottom   = item["cropBottom"].toDouble();
        if (item.contains("boundsWidth"))  t.boundsWidth  = item["boundsWidth"].toDouble();
        if (item.contains("boundsHeight")) t.boundsHeight = item["boundsHeight"].toDouble();
        if (item.contains("boundsType"))   t.boundsType   = item["boundsType"].toString();
        if (item.contains("alignment"))    t.alignment    = item["alignment"].toInt();

        requests.append(QJsonObject{
            {"requestType", "SetSceneItemTransform"},
            {"requestId",   nextId()},
            {"requestData", QJsonObject{
                {"sceneName",          scene},
                {"sceneItemId",        itemId},
                {"sceneItemTransform", t.toJson()},
            }},
        });
    }

    if (requests.isEmpty()) {
        emit log(QStringLiteral("applyTemplate '%1': no items matched in scene '%2'.")
                     .arg(name, scene));
        return false;
    }

    sendOp(8, QJsonObject{
        {"requestId",     nextId()},
        {"haltOnFailure", false},
        {"requests",      requests},
    });
    emit log(QStringLiteral("applyTemplate '%1' → '%2': %3 applied, %4 skipped.")
                 .arg(name, scene).arg(matched).arg(skipped));
    emit templateApplied(name, matched);
    return true;
}
