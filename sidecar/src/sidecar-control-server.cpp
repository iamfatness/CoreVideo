#include "sidecar-control-server.h"
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QDebug>

SidecarControlServer::SidecarControlServer(QObject *parent)
    : QObject(parent)
{
}

bool SidecarControlServer::start(quint16 port)
{
    if (m_server && m_server->isListening()) return true;

    if (!m_server) {
        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this, [this]() {
            while (m_server->hasPendingConnections()) {
                auto *socket = m_server->nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                    while (socket->canReadLine())
                        handleLine(socket, socket->readLine(4096).trimmed());
                });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                    removeSubscriber(socket);
                    socket->deleteLater();
                });
            }
        });
    }

    if (!m_server->listen(QHostAddress::LocalHost, port)) {
        qWarning() << "[sidecar] Control server failed to bind on 127.0.0.1:"
                   << port << "—" << m_server->errorString();
        return false;
    }

    qInfo() << "[sidecar] Control server listening on 127.0.0.1:" << port;
    return true;
}

void SidecarControlServer::stop()
{
    if (!m_server) return;
    m_server->close();
    m_eventSubs.clear();
}

void SidecarControlServer::pushEvent(const QJsonObject &event)
{
    const QByteArray line =
        QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
    for (auto *sock : QList<QTcpSocket *>(m_eventSubs.begin(), m_eventSubs.end())) {
        if (sock->state() == QAbstractSocket::ConnectedState) {
            sock->write(line);
            sock->flush();
        }
    }
}

void SidecarControlServer::removeSubscriber(QTcpSocket *socket)
{
    m_eventSubs.remove(socket);
}

void SidecarControlServer::notifyPhaseChanged(const QString &phase)
{
    m_phase = phase;
    pushEvent({{"event", "phase_changed"}, {"phase", phase}});
}

void SidecarControlServer::notifyTemplateChanged(const QString &id, const QString &name)
{
    m_templateId   = id;
    m_templateName = name;
    pushEvent({{"event", "template_changed"}, {"template_id", id}, {"template_name", name}});
}

void SidecarControlServer::notifyOBSState(const QString &state)
{
    m_obsState = state;
    pushEvent({{"event", "obs_state"}, {"state", state}});
}

void SidecarControlServer::notifySceneChanged(const QString &scene)
{
    m_currentScene = scene;
    pushEvent({{"event", "scene_changed"}, {"scene", scene}});
}

void SidecarControlServer::notifyScenesUpdated(const QStringList &scenes)
{
    m_scenes = scenes;
    QJsonArray arr;
    for (const auto &s : scenes) arr.append(s);
    pushEvent({{"event", "scenes_updated"}, {"scenes", arr}});
}

void SidecarControlServer::handleLine(QTcpSocket *socket, const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        writeResponse(socket, {{"ok", false}, {"error", "invalid_json"}});
        return;
    }

    const QJsonObject req = doc.object();
    const QString cmd = req.value("cmd").toString();

    if (cmd == "status") {
        QJsonArray scenesArr;
        for (const auto &s : m_scenes) scenesArr.append(s);
        writeResponse(socket, {
            {"ok",            true},
            {"phase",         m_phase},
            {"template_id",   m_templateId},
            {"template_name", m_templateName},
            {"obs_state",     m_obsState},
            {"scenes",        scenesArr},
            {"current_scene", m_currentScene},
        });
        return;
    }

    if (cmd == "list_templates") {
        struct BuiltIn { const char *id; const char *name; };
        static const BuiltIn builtIns[] = {
            {"1-up",        "1-Up Full"},
            {"2-up-sbs",    "2-Up Side by Side"},
            {"2-up-pip",    "2-Up Picture-in-Picture"},
            {"4-up-grid",   "4-Up Grid"},
            {"talk-show",   "Talk Show"},
        };
        QJsonArray templates;
        for (const auto &t : builtIns) {
            QJsonObject o;
            o["id"]   = t.id;
            o["name"] = t.name;
            templates.append(o);
        }
        writeResponse(socket, {{"ok", true}, {"templates", templates}});
        return;
    }

    if (cmd == "set_phase") {
        const QString phase = req.value("phase").toString();
        emit phaseChangeRequested(phase);
        writeResponse(socket, {{"ok", true}});
        return;
    }

    if (cmd == "apply_template") {
        const QString id = req.value("template_id").toString();
        emit templateApplyRequested(id);
        writeResponse(socket, {{"ok", true}});
        return;
    }

    if (cmd == "set_scene") {
        const QString scene = req.value("scene").toString();
        emit sceneChangeRequested(scene);
        writeResponse(socket, {{"ok", true}});
        return;
    }

    if (cmd == "subscribe_events") {
        m_eventSubs.insert(socket);
        connect(socket, &QTcpSocket::disconnected, this,
                [this, socket]() { removeSubscriber(socket); },
                Qt::UniqueConnection);
        writeResponse(socket, {{"ok", true}, {"subscribed", true}});
        // Push current status so the subscriber starts with fresh data.
        QJsonArray scenesArr;
        for (const auto &s : m_scenes) scenesArr.append(s);
        pushEvent({
            {"event",         "status"},
            {"phase",         m_phase},
            {"template_id",   m_templateId},
            {"template_name", m_templateName},
            {"obs_state",     m_obsState},
            {"scenes",        scenesArr},
            {"current_scene", m_currentScene},
        });
        return;
    }

    writeResponse(socket, {{"ok", false}, {"error", "unknown_command"}});
}

void SidecarControlServer::writeResponse(QTcpSocket *socket, const QJsonObject &resp)
{
    socket->write(QJsonDocument(resp).toJson(QJsonDocument::Compact));
    socket->write("\n");
    socket->flush();
}
