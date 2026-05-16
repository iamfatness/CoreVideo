#include "zoom-control-server.h"
#include "obs-utils.h"
#include "zoom-engine-client.h"
#include "zoom-iso-recorder.h"
#include "zoom-oauth.h"
#include "zoom-output-manager.h"
#include "zoom-reconnect.h"
#include "zoom-settings.h"
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>
#include <obs-module.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// Timing-safe string equality — prevents token leakage via timing side-channel.
// noinline prevents the compiler from inlining this and then optimising away
// the constant-time loop when the result is observable from context.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static bool ct_equal(const std::string &a, const std::string &b)
{
    volatile size_t diff = a.size() ^ b.size();
    const size_t max_len = std::max(a.size(), b.size());
    for (size_t i = 0; i < max_len; ++i) {
        const uint8_t ca = i < a.size() ? static_cast<uint8_t>(a[i]) : 0;
        const uint8_t cb = i < b.size() ? static_cast<uint8_t>(b[i]) : 0;
        diff |= static_cast<size_t>(ca ^ cb);
    }
    return diff == 0;
}

static bool json_to_uint32(const QJsonObject &obj, const char *key, uint32_t &out)
{
    const QJsonValue value = obj.value(key);
    if (!value.isDouble()) return false;

    const double raw = value.toDouble(-1);
    if (!std::isfinite(raw) || raw < 0 ||
        raw > static_cast<double>(std::numeric_limits<uint32_t>::max()) ||
        std::floor(raw) != raw) {
        return false;
    }

    out = static_cast<uint32_t>(raw);
    return true;
}

static QString meeting_state_to_string(MeetingState state);

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

    m_server->setMaxPendingConnections(10);

    if (!m_server->listen(QHostAddress::LocalHost, port)) {
        blog(LOG_ERROR,
             "[obs-zoom-plugin] Control server failed to bind on 127.0.0.1:%u: %s "
             "— TCP control API unavailable. Check that no other process owns this port.",
             static_cast<unsigned>(port),
             m_server->errorString().toUtf8().constData());
        return false;
    }

    if (m_token.empty())
        blog(LOG_WARNING, "[obs-zoom-plugin] Control server started without an auth token "
             "— any local process can issue commands. Set a token in Zoom Plugin Settings.");

    blog(LOG_INFO, "[obs-zoom-plugin] Control server listening on 127.0.0.1:%u",
         static_cast<unsigned>(port));

    // Roster changes: marshal to Qt main thread, then push event to subscribers.
    ZoomEngineClient::instance().add_roster_callback(this, [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            push_event({{"event", "roster_changed"}});
        }, Qt::QueuedConnection);
    });

    if (!m_poll_timer) {
        m_poll_timer = new QTimer(this);
        connect(m_poll_timer, &QTimer::timeout, this, [this]() { poll_and_push(); });
    }
    m_poll_timer->start(250);

    return true;
}

void ZoomControlServer::stop()
{
    if (!m_server) return;
    m_server->close();
    if (m_poll_timer) m_poll_timer->stop();
    ZoomEngineClient::instance().remove_roster_callback(this);
    m_event_subs.clear();
}

void ZoomControlServer::set_token(const std::string &token)
{
    m_token = token;
}

void ZoomControlServer::push_event(const QJsonObject &event)
{
    const QByteArray line =
        QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
    for (auto *sock : QList<QTcpSocket *>(m_event_subs.begin(), m_event_subs.end())) {
        if (sock->state() == QAbstractSocket::ConnectedState) {
            sock->write(line);
            sock->flush();
        }
    }
}

void ZoomControlServer::remove_subscriber(QTcpSocket *socket)
{
    m_event_subs.remove(socket);
}

void ZoomControlServer::poll_and_push()
{
    const auto newState = ZoomEngineClient::instance().state();
    if (newState != m_last_state) {
        m_last_state = newState;
        push_event({{"event", "meeting_state"}, {"state", meeting_state_to_string(newState)}});
    }

    const uint32_t spk = ZoomEngineClient::instance().active_speaker_id();
    if (spk != m_last_speaker) {
        m_last_speaker = spk;
        QString name;
        for (const auto &p : ZoomEngineClient::instance().roster()) {
            if (p.user_id == spk) { name = QString::fromStdString(p.display_name); break; }
        }
        push_event({
            {"event",   "active_speaker"},
            {"user_id", static_cast<double>(spk)},
            {"name",    name},
        });
    }
}

void ZoomControlServer::on_new_connection()
{
    while (m_server->hasPendingConnections()) {
        auto *socket = m_server->nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            while (socket->canReadLine())
                handle_line(socket, socket->readLine(4096).trimmed());
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    }
}

static QJsonObject output_to_json(const ZoomOutputInfo &o)
{
    QJsonObject obj;
    obj["source_uuid"]    = QString::fromStdString(o.source_uuid);
    obj["source"]         = QString::fromStdString(o.source_name);
    obj["display_name"]   = o.display_name.empty()
                            ? QString::fromStdString(o.source_name)
                            : QString::fromStdString(o.display_name);
    obj["participant_id"] = static_cast<double>(o.participant_id);
    obj["active_speaker"] = o.active_speaker;
    switch (o.assignment) {
    case AssignmentMode::ActiveSpeaker:
        obj["assignment_mode"] = "active_speaker";
        break;
    case AssignmentMode::SpotlightIndex:
        obj["assignment_mode"] = "spotlight";
        break;
    case AssignmentMode::ScreenShare:
        obj["assignment_mode"] = "screen_share";
        break;
    case AssignmentMode::Participant:
    default:
        obj["assignment_mode"] = "participant";
        break;
    }
    obj["spotlight_slot"] = static_cast<double>(o.spotlight_slot);
    obj["failover_participant_id"] =
        static_cast<double>(o.failover_participant_id);
    obj["isolate_audio"]  = o.isolate_audio;
    obj["audience_audio"] = o.audience_audio;
    obj["audio_channels"] = o.audio_mode == AudioChannelMode::Stereo
        ? "stereo" : "mono";
    switch (o.video_resolution) {
    case VideoResolution::P360:
        obj["video_resolution"] = "360p";
        break;
    case VideoResolution::P1080:
        obj["video_resolution"] = "1080p";
        break;
    case VideoResolution::P720:
    default:
        obj["video_resolution"] = "720p";
        break;
    }
    obj["observed_width"] = static_cast<double>(o.observed_width);
    obj["observed_height"] = static_cast<double>(o.observed_height);
    obj["observed_fps"] = o.observed_fps;
    obj["signal_below_requested"] = output_signal_below_requested(o);
    return obj;
}

static QJsonObject participant_to_json(const ParticipantInfo &p)
{
    QJsonObject obj;
    obj["id"] = static_cast<double>(p.user_id);
    obj["name"] = QString::fromStdString(p.display_name);
    obj["has_video"] = p.has_video;
    obj["is_talking"] = p.is_talking;
    obj["is_muted"] = p.is_muted;
    return obj;
}

static VideoResolution video_resolution_from_json(const QJsonObject &req)
{
    const QString value = req.value("video_resolution").toString("720p");
    if (value == "360p" || value == "360") return VideoResolution::P360;
    if (value == "1080p" || value == "1080") return VideoResolution::P1080;
    return VideoResolution::P720;
}

static QString meeting_state_to_string(MeetingState state)
{
    switch (state) {
    case MeetingState::Idle:       return "idle";
    case MeetingState::Joining:    return "joining";
    case MeetingState::InMeeting:  return "in_meeting";
    case MeetingState::Leaving:    return "leaving";
    case MeetingState::Recovering: return "recovering";
    case MeetingState::Failed:     return "failed";
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

    if (!m_token.empty() && cmd != "oauth_callback") {
        const std::string provided = req.value("token").toString().toStdString();
        if (!ct_equal(provided, m_token)) {
            write_response(socket, {{"ok", false}, {"error", "unauthorized"}});
            return;
        }
    }

    if (cmd == "help") {
        QJsonArray commands;
        for (const char *c : {"help", "status", "list_participants", "list_outputs",
                              "assign_output", "assign_output_ex",
                              "join", "leave", "start_engine", "stop_engine",
                              "oauth_callback",
                              "subscribe_events", "recovery_cancel"})
            commands.append(c);
        write_response(socket, {{"ok", true}, {"commands", commands}});
        return;
    }

    if (cmd == "status") {
        const auto &rm = ZoomReconnectManager::instance();
        QJsonObject recovery;
        recovery["active"]        = rm.is_recovering();
        recovery["attempt"]       = rm.attempt_count();
        recovery["max_attempts"]  = rm.policy().max_attempts;
        recovery["next_retry_ms"] = rm.next_retry_ms();
        write_response(socket, {
            {"ok", true},
            {"meeting_state", meeting_state_to_string(ZoomEngineClient::instance().state())},
            {"media_active", ZoomEngineClient::instance().is_media_active()},
            {"last_error", QString::fromStdString(
                ZoomEngineClient::instance().last_error())},
            {"active_speaker_id", static_cast<double>(
                ZoomEngineClient::instance().active_speaker_id())},
            {"recovery", recovery}
        });
        return;
    }

    if (cmd == "list_participants") {
        QJsonArray participants;
        for (const auto &p : ZoomEngineClient::instance().roster())
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

    if (cmd == "iso_recording_start") {
        ZoomIsoRecordConfig cfg;
        cfg.output_dir = req.value("output_dir").toString().toStdString();
        cfg.ffmpeg_path = req.value("ffmpeg_path").toString("ffmpeg").toStdString();
        cfg.record_program = req.value("record_program").toBool(true);
        std::string error;
        const bool ok = ZoomIsoRecorder::instance().start(cfg, &error);
        for (const auto &o : ZoomOutputManager::instance().outputs())
            ZoomIsoRecorder::instance().on_output_updated(o);
        write_response(socket, ok
            ? QJsonObject{{"ok", true}, {"sessions", ZoomIsoRecorder::instance().status_json()}}
            : QJsonObject{{"ok", false}, {"error", QString::fromStdString(error)}});
        return;
    }

    if (cmd == "iso_recording_stop") {
        ZoomIsoRecorder::instance().stop();
        write_response(socket, {{"ok", true}});
        return;
    }

    if (cmd == "iso_recording_status") {
        write_response(socket, {
            {"ok", true},
            {"active", ZoomIsoRecorder::instance().active()},
            {"sessions", ZoomIsoRecorder::instance().status_json()},
        });
        return;
    }

    if (cmd == "assign_output") {
        const QString source = req.value("source").toString();
        uint32_t participant_id = 0;
        if (!json_to_uint32(req, "participant_id", participant_id)) {
            write_response(socket, {{"ok", false}, {"error", "invalid_participant_id"}});
            return;
        }
        const bool active_speaker = req.value("active_speaker").toBool(false);
        const bool isolate_audio = req.value("isolate_audio").toBool(false);
        const QString audio_channels = req.value("audio_channels").toString("mono");
        const AudioChannelMode audio_mode = audio_channels == "stereo"
            ? AudioChannelMode::Stereo : AudioChannelMode::Mono;
        const VideoResolution video_resolution = video_resolution_from_json(req);

        const bool ok = ZoomOutputManager::instance().configure_output(
            source.toStdString(), participant_id, active_speaker,
            isolate_audio, audio_mode, video_resolution);
        QJsonObject response;
        response["ok"] = ok;
        if (!ok) response["error"] = "unknown_output";
        write_response(socket, response);
        return;
    }

    if (cmd == "join") {
        const QString meeting_id   = req.value("meeting_id").toString();
        const QString passcode_in  = req.value("passcode").toString();
        const QString display_name = req.value("display_name").toString("OBS");

        // Accept either a numeric ID or a full Zoom URL in meeting_id.
        const auto parsed = zoom_join_utils::parse_join_input(meeting_id.toStdString());
        if (parsed.meeting_id.empty()) {
            write_response(socket, {{"ok", false}, {"error", "invalid_meeting_id"}});
            return;
        }
        std::string passcode = passcode_in.toStdString();
        if (passcode.empty()) passcode = parsed.passcode;
        ZoomJoinAuthTokens tokens;
        tokens.on_behalf_token = req.value("on_behalf_token").toString(
            QString::fromStdString(parsed.on_behalf_token)).toStdString();
        tokens.user_zak = req.value("user_zak").toString(
            QString::fromStdString(parsed.user_zak)).toStdString();
        tokens.app_privilege_token = req.value("app_privilege_token").toString(
            QString::fromStdString(parsed.app_privilege_token)).toStdString();
        ZoomPluginSettings settings = ZoomPluginSettings::load();
        const bool needs_oauth_zak =
            tokens.user_zak.empty() &&
            tokens.on_behalf_token.empty();
        if (needs_oauth_zak) {
            if (settings.oauth_access_token.empty() &&
                settings.oauth_refresh_token.empty()) {
                write_response(socket, {
                    {"ok", false},
                    {"error", "zoom_auth_required"},
                    {"message", "Sign in with Zoom before joining meetings that require owner/host context."},
                });
                return;
            }

            std::string zak;
            QString zak_error;
            if (!ZoomOAuthManager::instance().fetch_zak_blocking(zak, parsed.meeting_id, &zak_error)) {
                blog(LOG_WARNING, "[obs-zoom-plugin] Control join OAuth ZAK fetch failed: %s",
                     zak_error.toUtf8().constData());
                write_response(socket, {
                    {"ok", false},
                    {"error", "zoom_zak_unavailable"},
                    {"message", zak_error.isEmpty()
                        ? QStringLiteral("Could not fetch Zoom ZAK.")
                        : zak_error},
                });
                return;
            }
            tokens.user_zak = zak;
            blog(LOG_INFO, "[obs-zoom-plugin] Control join fetched OAuth ZAK for Meeting SDK join");
        }

        const bool ok =
            ZoomEngineClient::instance().start(settings.resolved_jwt_token()) &&
            ZoomEngineClient::instance().join(parsed.meeting_id, passcode,
                                              display_name.toStdString(),
                                              MeetingKind::Meeting, tokens);
        write_response(socket, {{"ok", ok}});
        return;
    }

    if (cmd == "oauth_callback") {
        QString error;
        const bool ok = ZoomOAuthManager::instance().handle_redirect_url(
            req.value("url").toString(), &error);
        write_response(socket, ok
            ? QJsonObject{{"ok", true}}
            : QJsonObject{{"ok", false}, {"error", error}});
        return;
    }

    if (cmd == "leave") {
        ZoomEngineClient::instance().leave();
        write_response(socket, {{"ok", true}});
        return;
    }

    if (cmd == "start_engine") {
        if (ZoomEngineClient::instance().state() != MeetingState::InMeeting) {
            write_response(socket, {
                {"ok", false},
                {"error", "not_in_meeting"},
                {"message", "Join the meeting before starting the CoreVideo engine."},
            });
            return;
        }
        ZoomEngineClient::instance().start_media();
        write_response(socket, {{"ok", true}});
        return;
    }

    if (cmd == "stop_engine") {
        ZoomEngineClient::instance().stop_media();
        write_response(socket, {{"ok", true}});
        return;
    }

    // Extended output assignment — supports spotlight, screen_share, failover.
    // Fields: source (str), mode (str: participant|active_speaker|spotlight|screen_share),
    //         participant_id (uint), spotlight_slot (uint), failover_participant_id (uint),
    //         isolate_audio (bool), audio_channels (str: mono|stereo)
    if (cmd == "assign_output_ex") {
        const QString source = req.value("source").toString();
        const QString modeStr = req.value("mode").toString("participant");

        AssignmentMode mode = AssignmentMode::Participant;
        if      (modeStr == "active_speaker") mode = AssignmentMode::ActiveSpeaker;
        else if (modeStr == "spotlight")      mode = AssignmentMode::SpotlightIndex;
        else if (modeStr == "screen_share")   mode = AssignmentMode::ScreenShare;

        uint32_t participant_id = 0, spotlight_slot = 1, failover_id = 0;
        json_to_uint32(req, "participant_id",          participant_id);
        json_to_uint32(req, "spotlight_slot",          spotlight_slot);
        json_to_uint32(req, "failover_participant_id", failover_id);
        if (spotlight_slot < 1) spotlight_slot = 1;

        const bool isolate = req.value("isolate_audio").toBool(false);
        const bool audience = req.value("audience_audio").toBool(false);
        const AudioChannelMode amode =
            req.value("audio_channels").toString() == "stereo"
            ? AudioChannelMode::Stereo : AudioChannelMode::Mono;
        const VideoResolution vres = video_resolution_from_json(req);

        const bool ok = ZoomOutputManager::instance().configure_output_ex(
            source.toStdString(), mode, participant_id, spotlight_slot, failover_id,
            isolate, amode, vres, audience);
        write_response(socket, ok
            ? QJsonObject{{"ok", true}}
            : QJsonObject{{"ok", false}, {"error", "unknown_output"}});
        return;
    }

    if (cmd == "recovery_cancel") {
        ZoomReconnectManager::instance().cancel();
        write_response(socket, {{"ok", true}});
        return;
    }

    // Keep the socket open and stream JSON events until it disconnects.
    if (cmd == "subscribe_events") {
        m_event_subs.insert(socket);
        // Remove from the set if the client disconnects.
        connect(socket, &QTcpSocket::disconnected, this,
                [this, socket]() { remove_subscriber(socket); },
                Qt::UniqueConnection);
        write_response(socket, {{"ok", true}, {"subscribed", true}});
        // Send current state immediately so the subscriber starts with fresh data.
        push_event({{"event", "meeting_state"},
                    {"state", meeting_state_to_string(ZoomEngineClient::instance().state())}});
        const uint32_t spk = ZoomEngineClient::instance().active_speaker_id();
        QString spkName;
        for (const auto &p : ZoomEngineClient::instance().roster())
            if (p.user_id == spk) { spkName = QString::fromStdString(p.display_name); break; }
        push_event({{"event", "active_speaker"},
                    {"user_id", static_cast<double>(spk)},
                    {"name",    spkName}});
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
