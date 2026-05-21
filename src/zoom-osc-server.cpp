#include "zoom-osc-server.h"
#include "obs-utils.h"
#include "zoom-engine-client.h"
#include "zoom-iso-recorder.h"
#include "zoom-output-manager.h"
#include "zoom-reconnect.h"
#include "zoom-settings.h"
#include "zoom-oauth.h"
#include <QHostAddress>
#include <QMetaObject>
#include <QUdpSocket>
#include <obs-module.h>
#include <algorithm>
#include <cstring>

// ── OSC wire-format helpers ──────────────────────────────────────────────────

// Round up to the next multiple of 4.
static int pad4(int n) { return (n + 3) & ~3; }

// Read a null-terminated, 4-byte-padded OSC string from buf[offset].
// Returns "" and leaves offset unchanged on error.
static std::string read_osc_string(const QByteArray &buf, int &offset)
{
    const int start = offset;
    const int len   = buf.size();
    while (offset < len && buf[offset] != '\0') ++offset;
    if (offset >= len) { offset = start; return {}; }
    const std::string s(buf.constData() + start, offset - start);
    ++offset; // consume NUL
    offset = pad4(offset);
    return s;
}

// Read a big-endian int32 from buf[offset].
static int32_t read_int32(const QByteArray &buf, int &offset)
{
    if (offset + 4 > buf.size()) return 0;
    const auto *p = reinterpret_cast<const uint8_t *>(buf.constData() + offset);
    offset += 4;
    return static_cast<int32_t>((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                                 (uint32_t(p[2]) << 8)  |  uint32_t(p[3]));
}

// Read a big-endian float32 from buf[offset].
static float read_float32(const QByteArray &buf, int &offset)
{
    const int32_t raw = read_int32(buf, offset);
    float f; std::memcpy(&f, &raw, 4);
    return f;
}

// Encode a big-endian int32.
static void write_int32(QByteArray &out, int32_t v)
{
    out.append(static_cast<char>((v >> 24) & 0xFF));
    out.append(static_cast<char>((v >> 16) & 0xFF));
    out.append(static_cast<char>((v >>  8) & 0xFF));
    out.append(static_cast<char>( v        & 0xFF));
}

// Encode an OSC string (null-terminated, padded to 4 bytes).
static void write_osc_string(QByteArray &out, const std::string &s)
{
    out.append(s.c_str(), static_cast<int>(s.size()) + 1);
    while (out.size() % 4 != 0) out.append('\0');
}

// Parse a raw OSC message datagram into address + args.
// Returns false if the packet is malformed.
static bool parse_osc(const QByteArray &data,
                      QString &address,
                      std::vector<OscArg> &args)
{
    int offset = 0;
    const std::string addr_str = read_osc_string(data, offset);
    if (addr_str.empty() || addr_str[0] != '/') return false;
    address = QString::fromStdString(addr_str);

    if (offset >= data.size() || data[offset] != ',') return true; // no type tag — valid
    const std::string type_tags = read_osc_string(data, offset);

    for (size_t i = 1; i < type_tags.size(); ++i) {
        OscArg arg;
        switch (type_tags[i]) {
        case 'i':
            arg.type = OscArg::Int32;
            arg.i    = read_int32(data, offset);
            break;
        case 'f':
            arg.type = OscArg::Float32;
            arg.f    = read_float32(data, offset);
            break;
        case 's':
            arg.type = OscArg::String;
            arg.s    = read_osc_string(data, offset);
            break;
        case 'T': arg.type = OscArg::Int32; arg.i = 1; break;
        case 'F': arg.type = OscArg::Int32; arg.i = 0; break;
        default:  return false; // unsupported type
        }
        args.push_back(std::move(arg));
    }
    return true;
}

// Build a complete single-message OSC packet (address + type tags + args).
static QByteArray build_osc(const std::string &address,
                             const std::string &type_tags,
                             const std::vector<OscArg> &args)
{
    QByteArray pkt;
    write_osc_string(pkt, address);
    write_osc_string(pkt, "," + type_tags);
    for (size_t i = 0; i < args.size(); ++i) {
        switch (type_tags[i]) {
        case 'i': write_int32(pkt, args[i].i); break;
        case 's': write_osc_string(pkt, args[i].s); break;
        default: break;
        }
    }
    return pkt;
}

static bool find_output_by_source(const std::string &source, ZoomOutputInfo &out);

// ── ZoomOscServer ────────────────────────────────────────────────────────────

ZoomOscServer &ZoomOscServer::instance()
{
    static ZoomOscServer inst;
    return inst;
}

ZoomOscServer::ZoomOscServer(QObject *parent) : QObject(parent) {}

bool ZoomOscServer::start(quint16 port)
{
    if (m_socket && m_socket->state() == QAbstractSocket::BoundState) return true;
    m_running = false;

    if (!m_socket) {
        m_socket = new QUdpSocket(this);
        connect(m_socket, &QUdpSocket::readyRead, this,
                [this]() { on_datagram_ready(); });
    }

    if (!m_socket->bind(QHostAddress::LocalHost, port)) {
        blog(LOG_ERROR,
             "[obs-zoom-plugin] OSC server failed to bind on 127.0.0.1:%u: %s "
             "— OSC control API unavailable.",
             static_cast<unsigned>(port),
             m_socket->errorString().toUtf8().constData());
        return false;
    }

    blog(LOG_INFO, "[obs-zoom-plugin] OSC server listening on 127.0.0.1:%u",
         static_cast<unsigned>(port));
    m_running = true;

    ZoomEngineClient::instance().add_roster_callback(this, [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            if (!m_running || !m_socket)
                return;
            const QByteArray pkt = build_osc("/zoom/event/roster_changed", "", {});
            for (const auto &sub : m_subscribers)
                m_socket->writeDatagram(pkt, sub.addr, sub.port);
            for (const auto &sub : m_subscribers)
                send_participants(sub.addr, sub.port);
        }, Qt::QueuedConnection);
    });

    m_poll_timer = new QTimer(this);
    connect(m_poll_timer, &QTimer::timeout, this, [this]() { poll_and_push(); });
    m_poll_timer->start(kPollIntervalMs);

    return true;
}

void ZoomOscServer::stop()
{
    m_running = false;
    if (m_poll_timer) m_poll_timer->stop();
    ZoomEngineClient::instance().remove_roster_callback(this);
    m_subscribers.clear();
    if (m_socket) m_socket->close();
}

void ZoomOscServer::on_datagram_ready()
{
    if (!m_running || !m_socket)
        return;
    while (m_socket->hasPendingDatagrams()) {
        QHostAddress sender;
        quint16 sender_port = 0;
        QByteArray data;
        data.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        m_socket->readDatagram(data.data(), data.size(), &sender, &sender_port);

        QString address;
        std::vector<OscArg> args;
        if (!parse_osc(data, address, args))
            blog(LOG_WARNING, "[obs-zoom-plugin] OSC: malformed packet ignored");
        else
            dispatch(address, args, sender, sender_port);
    }
}

void ZoomOscServer::dispatch(const QString &address,
                              const std::vector<OscArg> &args,
                              const QHostAddress &sender,
                              quint16 sender_port)
{
    // /zoom/status  →  reply with meeting state + active speaker
    if (address == "/zoom/status") {
        send_status(sender, sender_port);
        return;
    }

    // /zoom/recovery/status  →  reply with recovery state
    if (address == "/zoom/recovery/status") {
        send_recovery_status(sender, sender_port);
        return;
    }

    // /zoom/list_outputs  →  reply with all configured outputs
    if (address == "/zoom/list_outputs") {
        send_outputs(sender, sender_port);
        return;
    }

    // /zoom/recover_stale_outputs [,i force]
    if (address == "/zoom/recover_stale_outputs") {
        const bool force = !args.empty() && args[0].type == OscArg::Int32 &&
            args[0].i != 0;
        const uint32_t recovered =
            ZoomOutputManager::instance().recover_stale_sources(force);
        std::vector<OscArg> reply(1);
        reply[0].type = OscArg::Int32;
        reply[0].i = static_cast<int32_t>(recovered);
        m_socket->writeDatagram(build_osc("/zoom/recover_stale_outputs/result",
                                          "i", reply),
                                sender, sender_port);
        return;
    }

    // /zoom/upgrade_low_quality_outputs [,i force]
    if (address == "/zoom/upgrade_low_quality_outputs") {
        const bool force = !args.empty() && args[0].type == OscArg::Int32 &&
            args[0].i != 0;
        const uint32_t upgraded =
            ZoomOutputManager::instance().upgrade_low_quality_sources(force);
        std::vector<OscArg> reply(1);
        reply[0].type = OscArg::Int32;
        reply[0].i = static_cast<int32_t>(upgraded);
        m_socket->writeDatagram(build_osc("/zoom/upgrade_low_quality_outputs/result",
                                          "i", reply),
                                sender, sender_port);
        return;
    }

    // /zoom/list_participants  →  reply with current roster
    if (address == "/zoom/list_participants") {
        send_participants(sender, sender_port);
        return;
    }

    // /zoom/subscribe  →  register caller for push events
    if (address == "/zoom/subscribe") {
        handle_subscribe(sender, sender_port);
        std::vector<OscArg> a(1);
        a[0].type = OscArg::Int32; a[0].i = kSubscriberTtlMs;
        m_socket->writeDatagram(build_osc("/zoom/subscribed", "i", a), sender, sender_port);
        return;
    }

    // /zoom/unsubscribe
    if (address == "/zoom/unsubscribe") {
        handle_unsubscribe(sender, sender_port);
        m_socket->writeDatagram(build_osc("/zoom/unsubscribed", "", {}), sender, sender_port);
        return;
    }

    // /zoom/ping
    if (address == "/zoom/ping") {
        m_socket->writeDatagram(build_osc("/zoom/pong", "", {}), sender, sender_port);
        return;
    }

    // /zoom/join  ,sss meeting_id passcode display_name
    // The meeting_id arg may also be a full Zoom join URL.
    if (address == "/zoom/join") {
        if (args.size() < 1 || args[0].type != OscArg::String) {
            blog(LOG_WARNING, "[obs-zoom-plugin] OSC /zoom/join: missing meeting_id arg");
            return;
        }
        const auto parsed = zoom_join_utils::parse_join_input(args[0].s);
        if (parsed.meeting_id.empty()) {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] OSC /zoom/join: could not parse meeting ID from '%s'",
                 args[0].s.c_str());
            return;
        }
        std::string passcode = args.size() >= 2 ? args[1].s : std::string();
        if (passcode.empty()) passcode = parsed.passcode;
        const std::string display_name = args.size() >= 3 ? args[2].s : "OBS";
        ZoomJoinAuthTokens tokens;
        tokens.on_behalf_token = parsed.on_behalf_token;
        tokens.user_zak = parsed.user_zak;
        tokens.app_privilege_token = parsed.app_privilege_token;
        const bool needs_oauth_zak =
            tokens.user_zak.empty() &&
            tokens.on_behalf_token.empty();
        const ZoomPluginSettings settings = ZoomPluginSettings::load();
        if (needs_oauth_zak &&
            settings.oauth_access_token.empty() &&
            settings.oauth_refresh_token.empty()) {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] OSC /zoom/join: OAuth authorization required before joining external meetings");
            return;
        }
        if (needs_oauth_zak) {
            std::string zak;
            QString zak_error;
            if (ZoomOAuthManager::instance().fetch_zak_blocking(zak, parsed.meeting_id, &zak_error))
                tokens.user_zak = zak;
            else {
                if (zak_error.isEmpty())
                    zak_error = "Authorize with Zoom before joining external meetings.";
                blog(LOG_WARNING, "[obs-zoom-plugin] OAuth ZAK unavailable: %s",
                     zak_error.toUtf8().constData());
                return;
            }
        }

        if (!ZoomEngineClient::instance().start(settings.resolved_jwt_token(),
                                                settings.sdk_public_app_key)) {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] OSC /zoom/join: engine failed to start");
            return;
        }
        ZoomEngineClient::instance().join(parsed.meeting_id, passcode, display_name,
                                          MeetingKind::Meeting, tokens);
        return;
    }

    // /zoom/leave
    if (address == "/zoom/leave") {
        ZoomEngineClient::instance().leave();
        return;
    }

    // /zoom/assign_output  ,sif source participant_id [active_speaker 0/1]
    // /zoom/assign_output  ,si  source participant_id
    if (address == "/zoom/assign_output") {
        if (args.size() < 2 ||
            args[0].type != OscArg::String ||
            args[1].type != OscArg::Int32) {
            blog(LOG_WARNING, "[obs-zoom-plugin] OSC /zoom/assign_output: "
                 "expected ,si[i] source participant_id [active_speaker]");
            return;
        }
        const std::string source   = args[0].s;
        const uint32_t pid         = static_cast<uint32_t>(args[1].i);
        const bool active_speaker  = args.size() >= 3 && args[2].i != 0;
        // Preserve existing isolate_audio/audio_mode by reading current config.
        bool isolate     = false;
        AudioChannelMode mode = AudioChannelMode::Mono;
        ZoomOutputInfo current;
        if (find_output_by_source(source, current)) {
            isolate = current.isolate_audio;
            mode    = current.audio_mode;
        }
        ZoomOutputManager::instance().configure_output(
            source, pid, active_speaker, isolate, mode);
        return;
    }

    // /zoom/assign_output/active_speaker  ,s source
    if (address == "/zoom/assign_output/active_speaker") {
        if (args.empty() || args[0].type != OscArg::String) {
            blog(LOG_WARNING, "[obs-zoom-plugin] OSC /zoom/assign_output/active_speaker: "
                 "expected ,s source");
            return;
        }
        const std::string source = args[0].s;
        bool isolate     = false;
        AudioChannelMode mode = AudioChannelMode::Mono;
        ZoomOutputInfo current;
        if (find_output_by_source(source, current)) {
            isolate = current.isolate_audio;
            mode    = current.audio_mode;
        }
        ZoomOutputManager::instance().configure_output(source, 0, true, isolate, mode);
        return;
    }

    // /zoom/output/assign_ex  ,sisi  source mode participant_id spotlight_slot
    if (address == "/zoom/output/assign_ex") {
        if (args.size() < 4 ||
            args[0].type != OscArg::String ||
            args[1].type != OscArg::String ||
            args[2].type != OscArg::Int32  ||
            args[3].type != OscArg::Int32) {
            blog(LOG_WARNING, "[obs-zoom-plugin] OSC /zoom/output/assign_ex: "
                 "expected ,sisi source mode participant_id spotlight_slot");
            return;
        }
        const std::string source = args[0].s;
        const std::string mode_s = args[1].s;
        const uint32_t pid       = static_cast<uint32_t>(args[2].i);
        const uint32_t slot      = static_cast<uint32_t>(args[3].i);

        AssignmentMode mode = AssignmentMode::Participant;
        if      (mode_s == "active_speaker") mode = AssignmentMode::ActiveSpeaker;
        else if (mode_s == "spotlight")      mode = AssignmentMode::SpotlightIndex;
        else if (mode_s == "screen_share")   mode = AssignmentMode::ScreenShare;

        bool isolate          = false;
        AudioChannelMode amode = AudioChannelMode::Mono;
        uint32_t failover     = 0;
        ZoomOutputInfo current;
        if (find_output_by_source(source, current)) {
            isolate  = current.isolate_audio;
            amode    = current.audio_mode;
            failover = current.failover_participant_id;
        }
        ZoomOutputManager::instance().configure_output_ex(
            source, mode, pid, slot, failover, isolate, amode);
        return;
    }

    // /zoom/output/audio_mode  ,ss  source "mono"|"stereo"
    if (address == "/zoom/output/audio_mode") {
        if (args.size() < 2 ||
            args[0].type != OscArg::String ||
            args[1].type != OscArg::String) {
            blog(LOG_WARNING, "[obs-zoom-plugin] OSC /zoom/output/audio_mode: "
                 "expected ,ss source mono|stereo");
            return;
        }
        const std::string source = args[0].s;
        const AudioChannelMode amode = (args[1].s == "stereo")
                                       ? AudioChannelMode::Stereo
                                       : AudioChannelMode::Mono;
        ZoomOutputInfo current;
        if (find_output_by_source(source, current)) {
            ZoomOutputManager::instance().configure_output_ex(
                source, current.assignment, current.participant_id,
                current.spotlight_slot, current.failover_participant_id,
                current.isolate_audio, amode);
            return;
        }
        blog(LOG_WARNING, "[obs-zoom-plugin] OSC /zoom/output/audio_mode: unknown source '%s'",
             source.c_str());
        return;
    }

    // /zoom/output/failover  ,si  source failover_participant_id
    if (address == "/zoom/output/failover") {
        if (args.size() < 2 ||
            args[0].type != OscArg::String ||
            args[1].type != OscArg::Int32) {
            blog(LOG_WARNING, "[obs-zoom-plugin] OSC /zoom/output/failover: "
                 "expected ,si source failover_participant_id");
            return;
        }
        const std::string source  = args[0].s;
        const uint32_t failover   = static_cast<uint32_t>(args[1].i);
        ZoomOutputInfo current;
        if (find_output_by_source(source, current)) {
            ZoomOutputManager::instance().configure_output_ex(
                source, current.assignment, current.participant_id,
                current.spotlight_slot, failover, current.isolate_audio,
                current.audio_mode);
            return;
        }
        blog(LOG_WARNING, "[obs-zoom-plugin] OSC /zoom/output/failover: unknown source '%s'",
             source.c_str());
        return;
    }

    // /zoom/isolate_audio  ,si source 0|1
    if (address == "/zoom/isolate_audio") {
        if (args.size() < 2 ||
            args[0].type != OscArg::String ||
            args[1].type != OscArg::Int32) {
            blog(LOG_WARNING, "[obs-zoom-plugin] OSC /zoom/isolate_audio: expected ,si source 0|1");
            return;
        }
        const std::string source = args[0].s;
        const bool isolate = args[1].i != 0;
        ZoomOutputInfo current;
        if (find_output_by_source(source, current)) {
            ZoomOutputManager::instance().configure_output(
                source, current.participant_id, current.active_speaker,
                isolate, current.audio_mode);
            return;
        }
        blog(LOG_WARNING, "[obs-zoom-plugin] OSC /zoom/isolate_audio: unknown source '%s'",
             source.c_str());
        return;
    }

    // /zoom/recovery/cancel
    if (address == "/zoom/recovery/cancel") {
        ZoomReconnectManager::instance().cancel();
        return;
    }

    // /zoom/iso/start [,s output_dir]
    if (address == "/zoom/iso/start") {
        ZoomIsoRecordConfig cfg;
        if (!args.empty() && args[0].type == OscArg::String)
            cfg.output_dir = args[0].s;
        std::string error;
        if (!ZoomIsoRecorder::instance().start(cfg, &error)) {
            blog(LOG_WARNING, "[obs-zoom-plugin] OSC /zoom/iso/start failed: %s",
                 error.c_str());
            return;
        }
        for (const auto &o : ZoomOutputManager::instance().outputs())
            ZoomIsoRecorder::instance().on_output_updated(o);
        return;
    }

    // /zoom/iso/stop
    if (address == "/zoom/iso/stop") {
        ZoomIsoRecorder::instance().stop();
        return;
    }

    blog(LOG_DEBUG, "[obs-zoom-plugin] OSC: unrecognised address '%s'",
         address.toUtf8().constData());
}

// ── Reply helpers ─────────────────────────────────────────────────────────────

static std::string meeting_state_str(MeetingState s)
{
    switch (s) {
    case MeetingState::Idle:       return "idle";
    case MeetingState::Joining:    return "joining";
    case MeetingState::InMeeting:  return "in_meeting";
    case MeetingState::Leaving:    return "leaving";
    case MeetingState::Recovering: return "recovering";
    case MeetingState::Failed:     return "failed";
    }
    return "unknown";
}

static bool find_output_by_source(const std::string &source, ZoomOutputInfo &out)
{
    const auto outputs = ZoomOutputManager::instance().outputs();
    const auto it = std::find_if(outputs.begin(), outputs.end(),
        [&source](const ZoomOutputInfo &o) {
            return o.source_name == source;
        });
    if (it == outputs.end())
        return false;
    out = *it;
    return true;
}

void ZoomOscServer::send_status(const QHostAddress &to, quint16 port)
{
    if (!m_running || !m_socket)
        return;
    const std::string state = meeting_state_str(ZoomEngineClient::instance().state());
    const uint32_t spk      = ZoomEngineClient::instance().active_speaker_id();

    // /zoom/status/meeting_state ,s <state>
    {
        std::vector<OscArg> a(1);
        a[0].type = OscArg::String; a[0].s = state;
        m_socket->writeDatagram(build_osc("/zoom/status/meeting_state", "s", a), to, port);
    }
    // /zoom/status/active_speaker ,i <id>
    {
        std::vector<OscArg> a(1);
        a[0].type = OscArg::Int32; a[0].i = static_cast<int32_t>(spk);
        m_socket->writeDatagram(build_osc("/zoom/status/active_speaker", "i", a), to, port);
    }
}

void ZoomOscServer::send_outputs(const QHostAddress &to, quint16 port)
{
    if (!m_running || !m_socket)
        return;
    for (const auto &o : ZoomOutputManager::instance().outputs()) {
        // /zoom/output ,sisii source participant_id display_name active_speaker isolate_audio
        std::vector<OscArg> a(5);
        a[0].type = OscArg::String; a[0].s = o.source_name;
        a[1].type = OscArg::Int32;  a[1].i = static_cast<int32_t>(o.participant_id);
        a[2].type = OscArg::String; a[2].s = o.display_name.empty() ? o.source_name : o.display_name;
        a[3].type = OscArg::Int32;  a[3].i = o.active_speaker ? 1 : 0;
        a[4].type = OscArg::Int32;  a[4].i = o.isolate_audio  ? 1 : 0;
        m_socket->writeDatagram(build_osc("/zoom/output", "sisii", a), to, port);
    }
}

void ZoomOscServer::send_recovery_status(const QHostAddress &to, quint16 port)
{
    if (!m_running || !m_socket)
        return;
    const auto &rm = ZoomReconnectManager::instance();
    // /zoom/recovery/status ,iii active attempt max_attempts
    std::vector<OscArg> a(3);
    a[0].type = OscArg::Int32; a[0].i = rm.is_recovering() ? 1 : 0;
    a[1].type = OscArg::Int32; a[1].i = rm.attempt_count();
    a[2].type = OscArg::Int32; a[2].i = rm.policy().max_attempts;
    m_socket->writeDatagram(build_osc("/zoom/recovery/status", "iii", a), to, port);
    // /zoom/recovery/next_retry_ms ,i ms_remaining
    std::vector<OscArg> b(1);
    b[0].type = OscArg::Int32; b[0].i = rm.next_retry_ms();
    m_socket->writeDatagram(build_osc("/zoom/recovery/next_retry_ms", "i", b), to, port);
}

void ZoomOscServer::send_participants(const QHostAddress &to, quint16 port)
{
    if (!m_running || !m_socket)
        return;
    for (const auto &p : ZoomEngineClient::instance().roster()) {
        // /zoom/participant ,isiii id name has_video is_talking is_muted
        std::vector<OscArg> a(5);
        a[0].type = OscArg::Int32;  a[0].i = static_cast<int32_t>(p.user_id);
        a[1].type = OscArg::String; a[1].s = p.display_name;
        a[2].type = OscArg::Int32;  a[2].i = p.has_video  ? 1 : 0;
        a[3].type = OscArg::Int32;  a[3].i = p.is_talking ? 1 : 0;
        a[4].type = OscArg::Int32;  a[4].i = p.is_muted   ? 1 : 0;
        m_socket->writeDatagram(build_osc("/zoom/participant", "isiii", a), to, port);
    }
}

// ── Push / subscription helpers ───────────────────────────────────────────────

void ZoomOscServer::handle_subscribe(const QHostAddress &addr, quint16 port)
{
    if (!m_running)
        return;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto it = std::find_if(m_subscribers.begin(), m_subscribers.end(),
        [&addr, port](const auto &sub) {
            return sub.addr == addr && sub.port == port;
        });
    if (it != m_subscribers.end()) {
        it->renewed_at = now;
        return;
    }
    m_subscribers.append({addr, port, now});
}

void ZoomOscServer::handle_unsubscribe(const QHostAddress &addr, quint16 port)
{
    for (int i = m_subscribers.size() - 1; i >= 0; --i) {
        if (m_subscribers[i].addr == addr && m_subscribers[i].port == port)
            m_subscribers.removeAt(i);
    }
}

void ZoomOscServer::push_to_all(const std::string &address,
                                 const std::string &type_tags,
                                 const std::vector<OscArg> &args)
{
    if (!m_running || !m_socket)
        return;
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addMSecs(-kSubscriberTtlMs);
    for (int i = m_subscribers.size() - 1; i >= 0; --i) {
        if (m_subscribers[i].renewed_at < cutoff)
            m_subscribers.removeAt(i);
    }
    const QByteArray pkt = build_osc(address, type_tags, args);
    for (const auto &sub : m_subscribers)
        m_socket->writeDatagram(pkt, sub.addr, sub.port);
}

void ZoomOscServer::poll_and_push()
{
    if (!m_running)
        return;
    const MeetingState newState = ZoomEngineClient::instance().state();
    if (newState != m_last_state) {
        m_last_state = newState;
        std::vector<OscArg> a(1);
        a[0].type = OscArg::String; a[0].s = meeting_state_str(newState);
        push_to_all("/zoom/event/meeting_state", "s", a);
    }

    const uint32_t spk = ZoomEngineClient::instance().active_speaker_id();
    if (spk != m_last_speaker) {
        m_last_speaker = spk;
        std::string name;
        const auto roster = ZoomEngineClient::instance().roster();
        const auto speaker = std::find_if(roster.begin(), roster.end(),
            [spk](const ParticipantInfo &p) { return p.user_id == spk; });
        if (speaker != roster.end())
            name = speaker->display_name;
        std::vector<OscArg> a(2);
        a[0].type = OscArg::Int32;  a[0].i = static_cast<int32_t>(spk);
        a[1].type = OscArg::String; a[1].s = name;
        push_to_all("/zoom/event/active_speaker", "is", a);
    }

    // Prune expired subscribers (push_to_all prunes on send; this covers the
    // quiet path where no events fire for a long time).
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addMSecs(-kSubscriberTtlMs);
    for (int i = m_subscribers.size() - 1; i >= 0; --i) {
        if (m_subscribers[i].renewed_at < cutoff)
            m_subscribers.removeAt(i);
    }
}
