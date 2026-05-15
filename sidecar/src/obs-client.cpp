#include "obs-client.h"
#include <QWebSocket>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QUrl>
#include <QDebug>

OBSClient::OBSClient(QObject *parent) : QObject(parent)
{
    m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_ws, &QWebSocket::connected,    this, &OBSClient::onConnected);
    connect(m_ws, &QWebSocket::disconnected, this, &OBSClient::onDisconnected);
    connect(m_ws, &QWebSocket::textMessageReceived,
            this, &OBSClient::onTextMessageReceived);
    connect(m_ws, &QWebSocket::errorOccurred, this, [this](auto) {
        const QString err = m_ws->errorString();
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
    setState(State::Connecting);
    emit log(QStringLiteral("Connecting to ws://%1:%2…").arg(cfg.host).arg(cfg.port));
    m_ws->open(QUrl(QString("ws://%1:%2").arg(cfg.host).arg(cfg.port)));
}

void OBSClient::disconnectFromOBS()
{
    m_reconnectTimer->stop();
    m_cfg.autoReconnect = false;
    if (m_ws->isValid() || m_ws->state() != QAbstractSocket::UnconnectedState)
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
    m_ws->open(QUrl(QString("ws://%1:%2").arg(m_cfg.host).arg(m_cfg.port)));
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
        requestVirtualCamStatus();
        break;
    case 5: handleEvent(d);    break;
    case 7: handleResponse(d); break;
    case 9:                                   // RequestBatchResponse
        emit log(QStringLiteral("Batch response received (%1 results).")
                     .arg(d["results"].toArray().size()));
        break;
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
        const QString comment = status["comment"].toString();
        emit log(QStringLiteral("%1 failed: %2").arg(type, comment));
        return;
    }

    const QJsonObject rd = d["responseData"].toObject();

    if (type == "GetSceneList") {
        QStringList names;
        for (const auto &v : rd["scenes"].toArray())
            names.prepend(v.toObject()["sceneName"].toString());
        emit scenesReceived(names);
        emit log(QStringLiteral("Scenes: %1").arg(names.join(", ")));
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
        const QString scene = rd["sceneName"].toString();
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
        emit sceneItemsReceived(scene, items);
        emit log(QStringLiteral("Scene '%1': %2 items cached.").arg(scene).arg(items.size()));
    }
}

void OBSClient::handleEvent(const QJsonObject &d)
{
    const QString type = d["eventType"].toString();
    if (type == "SceneItemCreated" || type == "SceneItemRemoved") {
        const QString scene = d["eventData"].toObject()["sceneName"].toString();
        m_itemCache.remove(scene);
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
    if (m_state == State::Connected)
        sendRequest("GetSceneItemList", {{"sceneName", sceneName}});
}

void OBSClient::setCurrentScene(const QString &name)
{
    if (m_state == State::Connected)
        sendRequest("SetCurrentProgramScene", {{"sceneName", name}});
}

int OBSClient::resolveItemId(const QString &scene, const QString &source) const
{
    auto it = m_itemCache.find(scene);
    if (it == m_itemCache.end()) return -1;
    return it->value(source, -1);
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
    for (int i = 0; i < tmpl.slots.size() && i < sourceNames.size(); ++i) {
        const TemplateSlot &slot = tmpl.slots[i];
        const int itemId = resolveItemId(sceneName, sourceNames[i]);
        if (itemId < 0) {
            emit log(QStringLiteral("Skip '%1' — not in scene '%2'.")
                         .arg(sourceNames[i], sceneName));
            continue;
        }
        ++matched;

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

// ── applyTemplate (flat applied JSON) ─────────────────────────────────────────
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
