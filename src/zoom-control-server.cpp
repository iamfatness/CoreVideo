#include "zoom-control-server.h"
#include "zoom-auth.h"
#include "zoom-meeting.h"
#include "zoom-output-manager.h"
#include "zoom-participants.h"
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <obs-module.h>

ZoomControlServer &ZoomControlServer::instance()
{
    static ZoomControlServer inst;
    return inst;
}

ZoomControlServer::ZoomControlServer(QObject *parent)
    : QObject(parent)
{
}

bool ZoomControlServer::start(quint16 port)
{
    if (m_server && m_server->isListening()) return true;

    if (!m_server) {
        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this,
                [this]() { on_new_connection(); });
    }

    if (!m_server->listen(QHostAddress::LocalHost, port)) {
        blog(LOG_WARNING, "[obs-zoom-plugin] Control server failed on 127.0.0.1:%u: %s",
             static_cast<unsigned>(port),
             m_server->errorString().toUtf8().constData());
        return false;
    }

    blog(LOG_INFO, "[obs-zoom-plugin] Control server listening on 127.0.0.1:%u",
         static_cast<unsigned>(port));
    return true;
}

void ZoomControlServer::stop()
{
    if (!m_server) return;
    m_server->close();
}

void ZoomControlServer::on_new_connection()
{
    while (m_server->hasPendingConnections()) {
        auto *socket = m_server->nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            while (socket->canReadLine())
                handle_line(socket, socket->readLine().trimmed());
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    }
}

static QJsonObject participant_to_json(const ParticipantInfo &p)
{
    QJsonObject obj;
    obj["id"] = static_cast<int>(p.user_id);
    obj["name"] = QString::fromStdString(p.display_name);
    obj["has_video"] = p.has_video;
    obj["is_talking"] = p.is_talking;
    obj["is_muted"] = p.is_muted;
    return obj;
}

static QJsonObject output_to_json(const ZoomOutputInfo &o)
{
    QJsonObject obj;
    obj["source"] = QString::fromStdString(o.source_name);
    obj["participant_id"] = static_cast<int>(o.participant_id);
    obj["active_speaker"] = o.active_speaker;
    obj["isolate_audio"] = o.isolate_audio;
    obj["audio_channels"] = o.audio_mode == AudioChannelMode::Stereo
        ? "stereo" : "mono";
    obj["video_active"] = o.video_active;
    obj["width"] = static_cast<int>(o.width);
    obj["height"] = static_cast<int>(o.height);
    obj["frame_count"] = static_cast<double>(o.frame_count);
    obj["last_frame_ns"] = static_cast<double>(o.last_frame_ns);
    return obj;
}

static QJsonObject preset_to_json(const std::string &name)
{
    QJsonObject obj;
    obj["name"] = QString::fromStdString(name);
    return obj;
}

static QString meeting_state_to_string(MeetingState state)
{
    switch (state) {
    case MeetingState::Idle: return "idle";
    case MeetingState::Joining: return "joining";
    case MeetingState::InMeeting: return "in_meeting";
    case MeetingState::Leaving: return "leaving";
    case MeetingState::Failed: return "failed";
    }
    return "unknown";
}

void ZoomControlServer::handle_line(QTcpSocket *socket, const QByteArray &line)
{
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        write_response(socket, {
            {"ok", false},
            {"error", "invalid_json"}
        });
        return;
    }

    const QJsonObject req = doc.object();
    const QString cmd = req.value("cmd").toString();

    if (cmd == "help") {
        QJsonArray commands;
        commands.append("help");
        commands.append("status");
        commands.append("list_participants");
        commands.append("list_outputs");
        commands.append("list_presets");
        commands.append("save_preset");
        commands.append("apply_preset");
        commands.append("delete_preset");
        commands.append("assign_output");
        commands.append("join");
        commands.append("leave");
        write_response(socket, {
            {"ok", true},
            {"commands", commands}
        });
        return;
    }

    if (cmd == "status") {
        const auto participants = ZoomParticipants::instance().roster();
        const auto outputs = ZoomOutputManager::instance().outputs();
        write_response(socket, {
            {"ok", true},
            {"meeting_state", meeting_state_to_string(ZoomMeeting::instance().state())},
            {"active_speaker_id", static_cast<int>(
                ZoomParticipants::instance().active_speaker_id())},
            {"participant_count", static_cast<int>(participants.size())},
            {"output_count", static_cast<int>(outputs.size())}
        });
        return;
    }

    if (cmd == "list_participants") {
        QJsonArray participants;
        for (const auto &p : ZoomParticipants::instance().roster())
            participants.append(participant_to_json(p));
        write_response(socket, {{"ok", true}, {"participants", participants}});
        return;
    }

    if (cmd == "list_outputs") {
        QJsonArray outputs;
        for (const auto &o : ZoomOutputManager::instance().outputs())
            outputs.append(output_to_json(o));
        write_response(socket, {{"ok", true}, {"outputs", outputs}});
        return;
    }

    if (cmd == "list_presets") {
        QJsonArray presets;
        for (const auto &name : ZoomOutputManager::instance().preset_names())
            presets.append(preset_to_json(name));
        write_response(socket, {{"ok", true}, {"presets", presets}});
        return;
    }

    if (cmd == "save_preset") {
        const QString name = req.value("name").toString();
        write_response(socket, {
            {"ok", ZoomOutputManager::instance().save_preset(name.toStdString())}
        });
        return;
    }

    if (cmd == "apply_preset") {
        const QString name = req.value("name").toString();
        write_response(socket, {
            {"ok", ZoomOutputManager::instance().apply_preset(name.toStdString())}
        });
        return;
    }

    if (cmd == "delete_preset") {
        const QString name = req.value("name").toString();
        write_response(socket, {
            {"ok", ZoomOutputManager::instance().delete_preset(name.toStdString())}
        });
        return;
    }

    if (cmd == "assign_output") {
        const QString source = req.value("source").toString();
        const uint32_t participant_id =
            static_cast<uint32_t>(req.value("participant_id").toInt(0));
        const bool active_speaker = req.value("active_speaker").toBool(false);
        const bool isolate_audio = req.value("isolate_audio").toBool(false);
        const QString audio_channels = req.value("audio_channels").toString("mono");
        const AudioChannelMode audio_mode = audio_channels == "stereo"
            ? AudioChannelMode::Stereo : AudioChannelMode::Mono;

        const bool ok = ZoomOutputManager::instance().configure_output(
            source.toStdString(), participant_id, active_speaker,
            isolate_audio, audio_mode);
        QJsonObject response;
        response["ok"] = ok;
        if (!ok) response["error"] = "unknown_output";
        write_response(socket, response);
        return;
    }

    if (cmd == "join") {
        const QString meeting_id = req.value("meeting_id").toString();
        const QString passcode = req.value("passcode").toString();
        const QString display_name = req.value("display_name").toString("OBS");
        const bool ok = ZoomMeeting::instance().join(
            meeting_id.toStdString(), passcode.toStdString(),
            display_name.toStdString());
        write_response(socket, {{"ok", ok}});
        return;
    }

    if (cmd == "leave") {
        ZoomMeeting::instance().leave();
        write_response(socket, {{"ok", true}});
        return;
    }

    write_response(socket, {
        {"ok", false},
        {"error", "unknown_command"}
    });
}

void ZoomControlServer::write_response(QTcpSocket *socket,
                                       const QJsonObject &response)
{
    QJsonDocument doc(response);
    socket->write(doc.toJson(QJsonDocument::Compact));
    socket->write("\n");
    socket->flush();
}
