#include "obs-client.h"
#include "simple-websocket.h"
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QDebug>

static const QString kCoreVideoSourcesScene = QStringLiteral("CoreVideo Sources");

static QString coreVideoSlotSceneName(int index)
{
    return QStringLiteral("CoreVideo Slot %1").arg(index + 1);
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
    for (int i = 0; i < tmpl.slotList.size() && i < sourceNames.size(); ++i) {
        const TemplateSlot &slot = tmpl.slotList[i];
        int itemId = resolveItemId(sceneName, sourceNames[i]);
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
                         .arg(sourceNames[i], sceneName));
            continue;
        }
        ++matched;
        usedItemIds.append(itemId);

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
    requests.append(QJsonObject{
        {"requestType", "CreateScene"},
        {"requestId",   nextId()},
        {"requestData", QJsonObject{{"sceneName", sceneName}}},
    });

    sendOp(8, QJsonObject{
        {"requestId",     nextId()},
        {"haltOnFailure", false},
        {"requests",      requests},
    });

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
    QTimer::singleShot(1800, this, [this, sceneName, tmpl, slotSceneNames, canvasW, canvasH, overlays, makeProgram]() {
        applyLayout(sceneName, tmpl, slotSceneNames, canvasW, canvasH);
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
    requests.append(QJsonObject{
        {"requestType", "CreateScene"},
        {"requestId",   nextId()},
        {"requestData", QJsonObject{{"sceneName", sceneName}}},
    });

    for (int i = 0; i < sourceNames.size(); ++i) {
        requests.append(QJsonObject{
            {"requestType", "CreateScene"},
            {"requestId",   nextId()},
            {"requestData", QJsonObject{{"sceneName", coreVideoSlotSceneName(i)}}},
        });
    }

    for (const QString &source : sourceNames) {
        const QString sourceName = source.trimmed();
        if (sourceName.isEmpty()) continue;
        requests.append(QJsonObject{
            {"requestType", "CreateInput"},
            {"requestId",   nextId()},
            {"requestData", QJsonObject{
                {"sceneName",        sceneName},
                {"inputName",        sourceName},
                {"inputKind",        "zoom_participant_source"},
                {"inputSettings",    QJsonObject{}},
                {"sceneItemEnabled", true},
            }},
        });
    }

    sendOp(8, QJsonObject{
        {"requestId",     nextId()},
        {"haltOnFailure", false},
        {"requests",      requests},
    });
    m_itemCache.remove(sceneName);
    m_sceneItems.remove(sceneName);
    emit log(QStringLiteral("Provisioning %1 CoreVideo placeholder sources in '%2'.")
                 .arg(sourceNames.size()).arg(sceneName));

    QTimer::singleShot(600, this, [this, sceneName, sourceNames]() {
        requestSceneItems(sceneName);
        for (int i = 0; i < sourceNames.size(); ++i)
            requestSceneItems(coreVideoSlotSceneName(i));
        requestSceneList();
    });
    auto linkSources = [this, sceneName, sourceNames]() {
        const bool sourceSceneReady = m_itemCache.contains(sceneName);
        if (!sourceSceneReady)
            requestSceneItems(sceneName);

        QJsonArray requests;
        if (sourceSceneReady) {
            for (const QString &source : sourceNames) {
                const QString sourceName = source.trimmed();
                if (sourceName.isEmpty()) continue;
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
        }

        for (int i = 0; i < sourceNames.size(); ++i) {
            const QString sourceName = sourceNames[i].trimmed();
            if (sourceName.isEmpty()) continue;
            const QString slotScene = coreVideoSlotSceneName(i);
            if (!m_itemCache.contains(slotScene)) {
                requestSceneItems(slotScene);
                continue;
            }
            if (resolveItemId(slotScene, sourceName) >= 0)
                continue;

            requests.append(QJsonObject{
                {"requestType", "CreateSceneItem"},
                {"requestId",   nextId()},
                {"requestData", QJsonObject{
                    {"sceneName",        slotScene},
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
    });
    QTimer::singleShot(3200, this, [this, sceneName, sourceNames]() {
        requestSceneItems(sceneName);
        for (int i = 0; i < sourceNames.size(); ++i)
            requestSceneItems(coreVideoSlotSceneName(i));
        requestSceneList();
    });
}

// ── applyTemplate (flat applied JSON) ─────────────────────────────────────────
void OBSClient::applyOverlays(const QString &sceneName,
                              const QVector<Overlay> &overlays,
                              double canvasW, double canvasH)
{
    if (m_state != State::Connected || sceneName.isEmpty())
        return;

    QJsonArray requests;
    for (int i = 0; i < overlays.size(); ++i) {
        const Overlay &ov = overlays[i];
        const QString sourceName = QStringLiteral("CoreVideo Overlay %1").arg(i + 1);
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

        requests.append(QJsonObject{
            {"requestType", "CreateInput"},
            {"requestId",   nextId()},
            {"requestData", QJsonObject{
                {"sceneName",        sceneName},
                {"inputName",        sourceName},
                {"inputKind",        "text_gdiplus_v3"},
                {"inputSettings",    inputSettings},
                {"sceneItemEnabled", true},
            }},
        });
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
            const QString sourceName = QStringLiteral("CoreVideo Overlay %1").arg(i + 1);
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
