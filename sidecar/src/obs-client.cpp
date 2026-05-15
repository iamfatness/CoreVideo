#include "obs-client.h"
#include <QWebSocket>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QUrl>

OBSClient::OBSClient(QObject *parent) : QObject(parent)
{
    m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_ws, &QWebSocket::connected,    this, &OBSClient::onConnected);
    connect(m_ws, &QWebSocket::disconnected, this, &OBSClient::onDisconnected);
    connect(m_ws, &QWebSocket::textMessageReceived,
            this, &OBSClient::onTextMessageReceived);
    connect(m_ws, &QWebSocket::errorOccurred, this,
            [this](auto err) {
                emit errorOccurred(m_ws->errorString());
            });
}

OBSClient::~OBSClient() { disconnectFromOBS(); }

void OBSClient::connectToOBS(const Config &cfg)
{
    m_cfg        = cfg;
    m_identified = false;
    m_ws->open(QUrl(QString("ws://%1:%2").arg(cfg.host).arg(cfg.port)));
}

void OBSClient::disconnectFromOBS()
{
    m_identified = false;
    if (m_ws->isValid()) m_ws->close();
}

// ── WebSocket slots ───────────────────────────────────────────────────────────
void OBSClient::onConnected()  { /* wait for Hello */ }
void OBSClient::onDisconnected()
{
    m_identified = false;
    emit disconnected();
}

void OBSClient::onTextMessageReceived(const QString &msg)
{
    const QJsonObject root = QJsonDocument::fromJson(msg.toUtf8()).object();
    const int op = root["op"].toInt(-1);
    const QJsonObject d = root["d"].toObject();

    switch (op) {
    case 0: handleHello(d);    break;
    case 2:                                // Identified
        m_identified = true;
        emit connected();
        break;
    case 5: handleEvent(d);    break;
    case 7: handleResponse(d); break;
    default: break;
    }
}

// ── Protocol helpers ──────────────────────────────────────────────────────────
void OBSClient::sendOp(int op, const QJsonObject &data)
{
    m_ws->sendTextMessage(
        QJsonDocument(QJsonObject{{"op", op}, {"d", data}}).toJson(QJsonDocument::Compact));
}

void OBSClient::sendRequest(const QString &type, const QJsonObject &data)
{
    const QString id = nextId();
    m_pending[id] = type;
    QJsonObject req{{"requestType", type}, {"requestId", id}};
    if (!data.isEmpty()) req["requestData"] = data;
    sendOp(6, req);
}

QString OBSClient::nextId()
{
    return QString("cv-%1").arg(m_idSeq++);
}

// ── obs-websocket v5 auth flow ────────────────────────────────────────────────
void OBSClient::handleHello(const QJsonObject &d)
{
    QJsonObject identify{{"rpcVersion", 1}};

    if (d.contains("authentication") && !m_cfg.password.isEmpty()) {
        const QJsonObject auth = d["authentication"].toObject();
        identify["authentication"] = computeAuth(
            m_cfg.password,
            auth["salt"].toString(),
            auth["challenge"].toString());
    }
    sendOp(1, identify);
}

QString OBSClient::computeAuth(const QString &password,
                               const QString &salt,
                               const QString &challenge)
{
    // base64( sha256( base64(sha256(password+salt)) + challenge ) )
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
    if (!d["requestStatus"].toObject()["result"].toBool()) return;

    const QJsonObject rd = d["responseData"].toObject();

    if (type == "GetSceneList") {
        QStringList names;
        for (const auto &v : rd["scenes"].toArray())
            names.prepend(v.toObject()["sceneName"].toString());
        emit scenesReceived(names);
    }
    else if (type == "GetSceneItemList") {
        const QString scene = rd["sceneName"].toString();
        QVector<SceneItem> items;
        for (const auto &v : rd["sceneItems"].toArray()) {
            const QJsonObject o = v.toObject();
            SceneItem si;
            si.sceneItemId = o["sceneItemId"].toInt();
            si.sourceName  = o["sourceName"].toString();
            si.enabled     = o["sceneItemEnabled"].toBool(true);
            items.append(si);
        }
        emit sceneItemsReceived(scene, items);
    }
}

void OBSClient::handleEvent(const QJsonObject &d)
{
    // Handle scene-changed events in the future
    Q_UNUSED(d)
}

// ── Public API ────────────────────────────────────────────────────────────────
void OBSClient::requestSceneList()
{
    if (m_identified) sendRequest("GetSceneList");
}

void OBSClient::requestSceneItems(const QString &sceneName)
{
    if (m_identified)
        sendRequest("GetSceneItemList", {{"sceneName", sceneName}});
}

void OBSClient::setCurrentScene(const QString &name)
{
    if (m_identified)
        sendRequest("SetCurrentProgramScene", {{"sceneName", name}});
}

void OBSClient::applyLayout(const QString        &sceneName,
                            const LayoutTemplate &tmpl,
                            const QStringList    &sourceNames,
                            double canvasW, double canvasH)
{
    if (!m_identified) return;

    // For each slot we need the sceneItemId of the matching source.
    // This is a two-step flow: caller must ensure scene items are fetched.
    // Here we construct the transforms directly — caller is responsible for
    // mapping sourceNames[i] → sceneItemId via sceneItemsReceived signal.
    //
    // We send a RequestBatch (op 8) so all transforms arrive atomically.
    QJsonArray requests;
    for (int i = 0; i < tmpl.slots.size() && i < sourceNames.size(); ++i) {
        const TemplateSlot &slot = tmpl.slots[i];

        const QJsonObject transform{
            {"positionX",    slot.x      * canvasW},
            {"positionY",    slot.y      * canvasH},
            {"boundsWidth",  slot.width  * canvasW},
            {"boundsHeight", slot.height * canvasH},
            {"boundsType",   "OBS_BOUNDS_SCALE_INNER"},
            {"scaleX",       1.0},
            {"scaleY",       1.0},
        };

        // Note: sceneItemId must be resolved by the caller; we use
        // sourceNames[i] as a placeholder here. Real integration uses the
        // sceneItemsReceived signal to build a name→id map first.
        const QString reqId = nextId();
        requests.append(QJsonObject{
            {"requestType", "SetSceneItemTransform"},
            {"requestId",   reqId},
            {"requestData", QJsonObject{
                {"sceneName",        sceneName},
                {"sourceName",       sourceNames[i]},  // resolved by OBS
                {"sceneItemTransform", transform},
            }},
        });
    }

    sendOp(8, QJsonObject{
        {"requestId",    nextId()},
        {"haltOnFailure", false},
        {"requests",     requests},
    });
}
