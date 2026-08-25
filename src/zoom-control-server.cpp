#include "zoom-control-server.h"
#include "obs-utils.h"
#include "speaker-director.h"
#include "zoom-control-parse.h"
#include "zoom-engine-client.h"
#include "zoom-iso-recorder.h"
#include "zoom-oauth.h"
#include "zoom-output-manager.h"
#include "zoom-participant-audio-source.h"
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
#include <util/platform.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// ct_equal, json_to_uint32, video_resolution_from_json and
// meeting_state_to_string now live in zoom-control-parse.h so they can be
// unit tested on the host without OBS/Qt-network/Zoom-SDK dependencies.

static QJsonObject speaker_director_to_json();

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
    m_running = false;

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
    m_running = true;

    // Roster changes: marshal to Qt main thread, then push event to subscribers.
    ZoomEngineClient::instance().add_roster_callback(this, [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            if (!m_running)
                return;
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
    m_running = false;
    if (m_poll_timer) m_poll_timer->stop();
    ZoomEngineClient::instance().remove_roster_callback(this);
    m_event_subs.clear();
    if (!m_server) return;
    m_server->close();
}

void ZoomControlServer::set_token(const std::string &token)
{
    m_token = token;
}

void ZoomControlServer::push_event(const QJsonObject &event)
{
    if (!m_running)
        return;
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
    if (!m_running)
        return;
    const auto newState = ZoomEngineClient::instance().state();
    if (newState != m_last_state) {
        m_last_state = newState;
        push_event({{"event", "meeting_state"}, {"state", meeting_state_to_string(newState)}});
    }

    const uint32_t spk = ZoomEngineClient::instance().active_speaker_id();
    if (spk != m_last_speaker) {
        m_last_speaker = spk;
        QString name;
        const auto roster = ZoomEngineClient::instance().roster();
        const auto speaker = std::find_if(roster.begin(), roster.end(),
            [spk](const ParticipantInfo &p) { return p.user_id == spk; });
        if (speaker != roster.end())
            name = QString::fromStdString(speaker->display_name);
        push_event({
            {"event",   "active_speaker"},
            {"user_id", static_cast<double>(spk)},
            {"name",    name},
            {"speaker_director", speaker_director_to_json()},
        });
    }

    const SpeakerDirectorSnapshot director =
        SpeakerDirector::instance().snapshot(os_gettime_ns() / 1000000ULL);
    if (director.directed_speaker_id != m_last_directed_speaker ||
        director.candidate_speaker_id != m_last_candidate_speaker ||
        director.manual_speaker_id != m_last_manual_speaker) {
        m_last_directed_speaker = director.directed_speaker_id;
        m_last_candidate_speaker = director.candidate_speaker_id;
        m_last_manual_speaker = director.manual_speaker_id;
        push_event({
            {"event", "speaker_director_changed"},
            {"speaker_director", speaker_director_to_json()},
        });
    }
}

void ZoomControlServer::on_new_connection()
{
    if (!m_running || !m_server)
        return;
    while (m_server->hasPendingConnections()) {
        auto *socket = m_server->nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            if (!m_running)
                return;
            while (socket->canReadLine())
                handle_line(socket, socket->readLine(4096).trimmed());
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    }
}

static QJsonObject output_to_json(const ZoomOutputInfo &o)
{
    const auto roster = ZoomEngineClient::instance().roster();
    auto find_participant = [&roster](uint32_t participant_id) {
        return std::find_if(roster.begin(), roster.end(),
            [participant_id](const ParticipantInfo &participant) {
                return participant.user_id == participant_id;
            });
    };
    const auto sharing_participant = std::find_if(
        roster.begin(), roster.end(),
        [](const ParticipantInfo &participant) {
            return participant.is_sharing_screen;
        });

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
    if (o.assignment == AssignmentMode::ScreenShare) {
        obj["screen_share_available"] = sharing_participant != roster.end();
        obj["screen_share_participant_id"] = sharing_participant != roster.end()
            ? static_cast<double>(sharing_participant->user_id)
            : 0.0;
        obj["screen_share_participant_name"] = sharing_participant != roster.end()
            ? QString::fromStdString(sharing_participant->display_name)
            : QString();
    }
    if (o.assignment == AssignmentMode::SpotlightIndex) {
        const auto spotlight_owner = std::find_if(
            roster.begin(), roster.end(),
            [&o](const ParticipantInfo &participant) {
                return participant.spotlight_index == o.spotlight_slot;
            });
        obj["spotlight_participant_id"] = spotlight_owner != roster.end()
            ? static_cast<double>(spotlight_owner->user_id)
            : 0.0;
        obj["spotlight_participant_name"] = spotlight_owner != roster.end()
            ? QString::fromStdString(spotlight_owner->display_name)
            : QString();
    }
    if (o.assignment == AssignmentMode::Participant && o.participant_id != 0) {
        const auto participant = find_participant(o.participant_id);
        obj["participant_present"] = participant != roster.end();
        obj["participant_name"] = participant != roster.end()
            ? QString::fromStdString(participant->display_name)
            : QString();
    }
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
    obj["last_frame_age_ms"] = static_cast<double>(o.last_frame_age_ms);
    obj["video_stale"] = o.video_stale;
    obj["stale_recovery_attempts"] =
        static_cast<double>(o.stale_recovery_attempts);
    obj["stale_recovery_cooldown_ms"] =
        static_cast<double>(o.stale_recovery_cooldown_ms);
    obj["quality_upgrade_attempts"] =
        static_cast<double>(o.quality_upgrade_attempts);
    obj["quality_upgrade_cooldown_ms"] =
        static_cast<double>(o.quality_upgrade_cooldown_ms);
    obj["subscribed_age_ms"] = static_cast<double>(o.subscribed_age_ms);
    obj["audio_delay_ms"]   = static_cast<double>(o.audio_delay_ms);
    obj["audio_latency_us"] = static_cast<double>(o.audio_latency_us);
    obj["video_latency_us"] = static_cast<double>(o.video_latency_us);
    // The EBU R37 number: positive means audio is EARLY relative to video, so
    // that much delay (in ms) is what to dial into audio_delay_ms to close it.
    //
    // NULL, not 0, when either latency has not been measured yet. A latency of
    // 0 means "no frame/buffer has been through this path", and emitting an
    // av_offset_us of 0 for it told automation the rig was PERFECTLY IN SYNC --
    // the most confident possible reading of no data at all. The dock has
    // always drawn "-" in exactly this case (av_offset_text()); the API now
    // agrees with it.
    if (o.audio_latency_us == 0 || o.video_latency_us == 0) {
        obj["av_offset_us"] = QJsonValue(QJsonValue::Null);
    } else {
        obj["av_offset_us"] =
            static_cast<double>(static_cast<int64_t>(o.video_latency_us) -
                                static_cast<int64_t>(o.audio_latency_us));
    }
    // audio_delay_ms/audio_latency_us/video_latency_us/av_offset_us all
    // describe THIS output's own embedded audio path (this ZoomSource's
    // obs_source_output_audio() call) -- NOT the separate, disjoint
    // CoreVideoAudioSource participant/active-speaker/audience sources a show
    // may route to program instead. See README's Output Manager section for
    // why these two audio paths cannot currently be reconciled into one
    // number.
    obj["audio_path"] = "embedded";
    obj["duplicate_participant_assignment"] = o.duplicate_participant_assignment;
    obj["health_reason"] = output_health_reason_id(o.health_reason);
    obj["health_label"] = output_health_reason_label(o.health_reason);
    obj["signal_below_requested"] = output_signal_below_requested(o);
    obj["signal_missing_or_stale"] = output_signal_missing_or_stale(o);
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
    obj["is_host"] = p.is_host;
    obj["is_co_host"] = p.is_co_host;
    obj["raised_hand"] = p.raised_hand;
    obj["spotlight_index"] = static_cast<double>(p.spotlight_index);
    obj["is_sharing_screen"] = p.is_sharing_screen;
    return obj;
}

static QJsonObject director_participant_to_json(
    const std::vector<ParticipantInfo> &roster, uint32_t participant_id)
{
    QJsonObject obj;
    obj["id"] = static_cast<double>(participant_id);
    obj["present"] = false;
    obj["name"] = QString();
    obj["has_video"] = false;
    obj["is_talking"] = false;
    obj["is_muted"] = false;
    if (participant_id == 0)
        return obj;

    const auto it = std::find_if(roster.begin(), roster.end(),
        [participant_id](const ParticipantInfo &p) {
            return p.user_id == participant_id;
        });
    if (it == roster.end())
        return obj;

    obj["present"] = true;
    obj["name"] = QString::fromStdString(it->display_name);
    obj["has_video"] = it->has_video;
    obj["is_talking"] = it->is_talking;
    obj["is_muted"] = it->is_muted;
    return obj;
}

static QJsonObject speaker_director_to_json()
{
    const SpeakerDirectorSnapshot s =
        SpeakerDirector::instance().snapshot(os_gettime_ns() / 1000000ULL);
    const auto roster = ZoomEngineClient::instance().roster();
    QJsonObject obj;
    obj["directed_speaker_id"] = static_cast<double>(s.directed_speaker_id);
    obj["raw_speaker_id"] = static_cast<double>(s.raw_speaker_id);
    obj["candidate_speaker_id"] = static_cast<double>(s.candidate_speaker_id);
    obj["last_speaker_id"] = static_cast<double>(s.last_speaker_id);
    obj["manual_speaker_id"] = static_cast<double>(s.manual_speaker_id);
    obj["manual_active"] = s.manual_active;
    obj["candidate_elapsed_ms"] = static_cast<double>(s.candidate_elapsed_ms);
    obj["hold_remaining_ms"] = static_cast<double>(s.hold_remaining_ms);
    obj["sensitivity_ms"] = static_cast<double>(s.sensitivity_ms);
    obj["hold_ms"] = static_cast<double>(s.hold_ms);
    obj["require_video"] = s.require_video;
    QJsonArray excluded;
    QJsonArray excluded_participants;
    for (const uint32_t id : s.excluded_participant_ids) {
        excluded.append(static_cast<double>(id));
        excluded_participants.append(director_participant_to_json(roster, id));
    }
    obj["excluded_participant_ids"] = excluded;
    obj["excluded_participants"] = excluded_participants;
    obj["directed_speaker"] = director_participant_to_json(roster, s.directed_speaker_id);
    obj["raw_speaker"] = director_participant_to_json(roster, s.raw_speaker_id);
    obj["candidate_speaker"] = director_participant_to_json(roster, s.candidate_speaker_id);
    obj["last_speaker"] = director_participant_to_json(roster, s.last_speaker_id);
    obj["manual_speaker"] = director_participant_to_json(roster, s.manual_speaker_id);
    if (s.manual_active)
        obj["status"] = "manual_supersede";
    else if (s.candidate_speaker_id != 0)
        obj["status"] = "candidate_pending";
    else if (s.directed_speaker_id != 0)
        obj["status"] = "holding";
    else
        obj["status"] = "waiting_for_speaker";
    return obj;
}

void ZoomControlServer::handle_line(QTcpSocket *socket, const QByteArray &line)
{
    const ParsedControlRequest parsed = parse_control_request(line, m_token);
    if (parsed.error == ControlRequestError::InvalidJson) {
        write_response(socket, {
            {"ok", false},
            {"error", "invalid_json"}
        });
        return;
    }
    if (parsed.error == ControlRequestError::Unauthorized) {
        write_response(socket, {{"ok", false}, {"error", "unauthorized"}});
        return;
    }

    const QJsonObject &req = parsed.request;
    const QString &cmd = parsed.cmd;

    if (cmd == "help") {
        QJsonArray commands;
        for (const char *c : known_control_commands())
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
            {"raw_active_speaker_id", static_cast<double>(
                ZoomEngineClient::instance().raw_active_speaker_id())},
            {"speaker_director", speaker_director_to_json()},
            {"recovery", recovery}
        });
        return;
    }

    if (cmd == "speaker_director_status") {
        write_response(socket, {
            {"ok", true},
            {"speaker_director", speaker_director_to_json()}
        });
        return;
    }

    if (cmd == "speaker_director_configure") {
        uint32_t sensitivity_ms = 0;
        uint32_t hold_ms = 0;
        if (!json_to_uint32(req, "sensitivity_ms", sensitivity_ms) ||
            !json_to_uint32(req, "hold_ms", hold_ms)) {
            write_response(socket, {{"ok", false}, {"error", "invalid_director_timing"}});
            return;
        }
        const bool require_video = req.value("require_video").toBool(true);
        auto settings = ZoomPluginSettings::load();
        settings.speaker_sensitivity_ms = sensitivity_ms;
        settings.speaker_hold_ms = hold_ms;
        settings.speaker_require_video = require_video;
        if (req.contains("excluded_participant_ids") &&
            req.value("excluded_participant_ids").isArray()) {
            const QJsonArray excluded = req.value("excluded_participant_ids").toArray();
            settings.speaker_exclude_participant_1 = excluded.size() > 0
                ? static_cast<uint32_t>(excluded.at(0).toInt(0)) : 0;
            settings.speaker_exclude_participant_2 = excluded.size() > 1
                ? static_cast<uint32_t>(excluded.at(1).toInt(0)) : 0;
        }
        settings.save();
        std::vector<uint32_t> excluded;
        if (settings.speaker_exclude_participant_1 != 0)
            excluded.push_back(settings.speaker_exclude_participant_1);
        if (settings.speaker_exclude_participant_2 != 0 &&
            settings.speaker_exclude_participant_2 !=
                settings.speaker_exclude_participant_1)
            excluded.push_back(settings.speaker_exclude_participant_2);
        SpeakerDirector::instance().configure(sensitivity_ms, hold_ms,
                                              require_video, excluded);
        write_response(socket, {
            {"ok", true},
            {"speaker_director", speaker_director_to_json()}
        });
        return;
    }

    if (cmd == "speaker_director_take") {
        uint32_t participant_id = 0;
        if (!json_to_uint32(req, "participant_id", participant_id)) {
            write_response(socket, {{"ok", false}, {"error", "invalid_participant_id"}});
            return;
        }
        const bool ok = SpeakerDirector::instance().set_manual_speaker(
            participant_id, os_gettime_ns() / 1000000ULL);
        if (ok)
            ZoomOutputManager::instance().resubscribe_all();
        write_response(socket, ok
            ? QJsonObject{{"ok", true}, {"speaker_director", speaker_director_to_json()}}
            : QJsonObject{{"ok", false}, {"error", "participant_not_available"}});
        return;
    }

    if (cmd == "speaker_director_release") {
        const bool changed = SpeakerDirector::instance().clear_manual_speaker(
            os_gettime_ns() / 1000000ULL);
        if (changed)
            ZoomOutputManager::instance().resubscribe_all();
        write_response(socket, {
            {"ok", true},
            {"speaker_director", speaker_director_to_json()}
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

    // The DEDICATED audio path, which list_outputs does not and cannot cover:
    // a CoreVideoAudioSource is not a ZoomSource and is not registered with
    // ZoomOutputManager. This is where the ring's loss counter
    // (`overrun_slots`) and the dedicated path's measured latency become
    // readable at all -- the spec requires both to be surfaced, and until this
    // command existed neither reached anything but a rate-limited log line.
    if (cmd == "list_audio_sources") {
        QJsonArray sources;
        for (const auto &a : corevideo_audio_source_infos()) {
            QJsonObject obj;
            obj["source_name"]  = QString::fromStdString(a.source_name);
            obj["source_uuid"]  = QString::fromStdString(a.source_uuid);
            obj["kind"]         = QString::fromUtf8(a.kind);
            obj["participant_id"] = static_cast<double>(a.participant_id);
            obj["subscribed"]   = a.subscribed;
            obj["audio_delay_ms"] = static_cast<double>(a.audio_delay_ms);
            // Same convention as list_outputs' av_offset_us: null means NOT
            // MEASURED, which is not the same as zero.
            obj["audio_latency_us"] = a.audio_latency_us == 0
                ? QJsonValue(QJsonValue::Null)
                : QJsonValue(static_cast<double>(a.audio_latency_us));
            // Ring slots the engine overwrote before this source drained them.
            obj["overrun_slots"] = static_cast<double>(a.overrun_slots);
            obj["frame_count"]   = static_cast<double>(a.frame_count);
            obj["audio_path"]    = "dedicated";
            sources.append(obj);
        }
        write_response(socket, {{"ok", true}, {"audio_sources", sources}});
        return;
    }

    if (cmd == "recover_stale_outputs") {
        const bool force = req.value("force").toBool(false);
        const uint32_t recovered =
            ZoomOutputManager::instance().recover_stale_sources(force);
        write_response(socket, {
            {"ok", true},
            {"recovered", static_cast<double>(recovered)}
        });
        return;
    }

    if (cmd == "upgrade_low_quality_outputs") {
        const bool force = req.value("force").toBool(false);
        const uint32_t upgraded =
            ZoomOutputManager::instance().upgrade_low_quality_sources(force);
        write_response(socket, {
            {"ok", true},
            {"upgraded", static_cast<double>(upgraded)}
        });
        return;
    }

    if (cmd == "iso_recording_start") {
        ZoomIsoRecordConfig cfg;
        cfg.output_dir = req.value("output_dir").toString().toStdString();
        cfg.ffmpeg_path = req.value("ffmpeg_path").toString("ffmpeg").toStdString();
        cfg.video_encoder =
            req.value("video_encoder").toString("libx264").toStdString();
        cfg.record_program = req.value("record_program").toBool(true);
        std::string error;
        const bool ok = ZoomIsoRecorder::instance().start(cfg, &error);
        for (const auto &o : ZoomOutputManager::instance().outputs())
            ZoomIsoRecorder::instance().on_output_updated(o);
        write_response(socket, ok
            ? QJsonObject{{"ok", true},
                          {"recorder", ZoomIsoRecorder::instance().status_overview()},
                          {"sessions", ZoomIsoRecorder::instance().status_json()}}
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
            {"recorder", ZoomIsoRecorder::instance().status_overview()},
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

        // kAudioDelayKeepCurrentMs (not 0) is the default so an assign_output
        // call that doesn't mention audio_delay_ms -- e.g. automation
        // reassigning a participant -- can't silently reset a delay the
        // operator dialed in through the Output Manager. A malformed value
        // (wrong type, negative, non-integral) is likewise left as "keep
        // current" rather than coerced to 0 or 500. Clamp whatever IS
        // validly supplied; never trust a caller-provided value.
        uint32_t audio_delay_ms = kAudioDelayKeepCurrentMs;
        if (req.contains("audio_delay_ms")) {
            uint32_t parsed = 0;
            if (json_to_uint32(req, "audio_delay_ms", parsed))
                audio_delay_ms = std::min<uint32_t>(parsed, 500);
        }

        const bool ok = ZoomOutputManager::instance().configure_output(
            source.toStdString(), participant_id, active_speaker,
            isolate_audio, audio_mode, video_resolution,
            /*audience_audio=*/false, audio_delay_ms);
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
        // Named distinctly from the outer `parsed` (the ParsedControlRequest
        // for this whole dispatch call) -- they are different types and
        // reusing the name shadowed the outer variable (cppcheck shadowVariable).
        const auto join_input = zoom_join_utils::parse_join_input(meeting_id.toStdString());
        if (join_input.meeting_id.empty()) {
            write_response(socket, {{"ok", false}, {"error", "invalid_meeting_id"}});
            return;
        }
        std::string passcode = passcode_in.toStdString();
        if (passcode.empty()) passcode = join_input.passcode;
        ZoomJoinAuthTokens tokens;
        tokens.on_behalf_token = req.value("on_behalf_token").toString(
            QString::fromStdString(join_input.on_behalf_token)).toStdString();
        tokens.user_zak = req.value("user_zak").toString(
            QString::fromStdString(join_input.user_zak)).toStdString();
        tokens.app_privilege_token = req.value("app_privilege_token").toString(
            QString::fromStdString(join_input.app_privilege_token)).toStdString();
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
            if (!ZoomOAuthManager::instance().fetch_zak_blocking(zak, join_input.meeting_id, &zak_error)) {
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

        std::string public_app_key =
            settings.resolved_meeting_sdk_public_app_key();
        std::string jwt = public_app_key.empty()
            ? settings.resolved_jwt_token()
            : std::string();
        if (!public_app_key.empty() && settings.use_broker_sdk_jwt()) {
            QString sdk_jwt_error;
            if (!ZoomOAuthManager::instance().fetch_sdk_jwt_blocking(jwt, &sdk_jwt_error)) {
                write_response(socket, {
                    {"ok", false},
                    {"error", "zoom_sdk_auth_unavailable"},
                    {"message", sdk_jwt_error.isEmpty()
                        ? QStringLiteral("Could not fetch Meeting SDK auth from CoreVideo broker.")
                        : sdk_jwt_error},
                });
                return;
            }
            public_app_key.clear();
        }
        blog(LOG_INFO,
             "[obs-zoom-plugin] Control join Meeting SDK auth mode=%s jwt_present=%d public_app_key_present=%d broker_jwt=%d",
             settings.meeting_sdk_auth_mode.c_str(),
             jwt.empty() ? 0 : 1,
             public_app_key.empty() ? 0 : 1,
             settings.use_broker_sdk_jwt() ? 1 : 0);
        const bool ok =
            ZoomEngineClient::instance().start(jwt, public_app_key) &&
            ZoomEngineClient::instance().join(join_input.meeting_id, passcode,
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
        blog(LOG_INFO, "[obs-zoom-plugin] Control API: leave");
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

    // Milestone 1: fires the talkback probe against one named participant.
    // The probe itself is async and reports its stages as OBS log lines
    // ("talkback_probe" events in ZoomEngineClient::handle_event), not over
    // this connection -- this response only confirms the trigger was
    // accepted.
    if (cmd == "talkback_probe") {
        const QString participant = req.value("participant").toString();
        if (participant.isEmpty()) {
            write_response(socket, {
                {"ok", false},
                {"error", "participant_required"},
                {"message", "participant name required"},
            });
            return;
        }
        if (ZoomEngineClient::instance().state() != MeetingState::InMeeting) {
            write_response(socket, {
                {"ok", false},
                {"error", "not_in_meeting"},
                {"message", "Join the meeting before running the talkback probe."},
            });
            return;
        }
        blog(LOG_INFO, "[obs-zoom-plugin] Control API: talkback_probe participant=%s",
             participant.toStdString().c_str());
        ZoomEngineClient::instance().talkback_probe(participant.toStdString());
        write_response(socket, {
            {"ok", true},
            {"note", "probe started; watch the OBS log for talkback_probe stages"},
        });
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
        blog(LOG_INFO, "[obs-zoom-plugin] Control API: recovery_cancel");
        ZoomEngineClient::instance().stop();
        write_response(socket, {
            {"ok", true},
            {"meeting_state", meeting_state_to_string(ZoomEngineClient::instance().state())},
            {"recovery", QJsonObject{
                {"active", ZoomReconnectManager::instance().is_recovering()},
                {"next_retry_ms", ZoomReconnectManager::instance().next_retry_ms()}
            }}
        });
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
        const auto roster = ZoomEngineClient::instance().roster();
        const auto speaker = std::find_if(roster.begin(), roster.end(),
            [spk](const ParticipantInfo &p) { return p.user_id == spk; });
        if (speaker != roster.end())
            spkName = QString::fromStdString(speaker->display_name);
        push_event({{"event", "active_speaker"},
                    {"user_id", static_cast<double>(spk)},
                    {"name",    spkName},
                    {"speaker_director", speaker_director_to_json()}});
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
