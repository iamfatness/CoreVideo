#include "zoom-osc-server.h"
#include "obs-utils.h"
#include "zoom-engine-client.h"
#include "zoom-output-manager.h"
#include "zoom-reconnect.h"
#include "zoom-settings.h"
#include <QHostAddress>
#include <QUdpSocket>
#include <obs-module.h>
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
    return true;
}

void ZoomOscServer::stop()
{
    if (m_socket) m_socket->close();
}

void ZoomOscServer::on_datagram_ready()
{
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

    // /zoom/list_participants  →  reply with current roster
    if (address == "/zoom/list_participants") {
        send_participants(sender, sender_port);
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

        const ZoomPluginSettings settings = ZoomPluginSettings::load();
        if (!ZoomEngineClient::instance().start(settings.jwt_token)) {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] OSC /zoom/join: engine failed to start");
            return;
        }
        ZoomEngineClient::instance().join(parsed.meeting_id, passcode, display_name);
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
        for (const auto &o : ZoomOutputManager::instance().outputs()) {
            if (o.source_name == source) {
                isolate = o.isolate_audio;
                mode    = o.audio_mode;
                break;
            }
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
        for (const auto &o : ZoomOutputManager::instance().outputs()) {
            if (o.source_name == source) {
                isolate = o.isolate_audio;
                mode    = o.audio_mode;
                break;
            }
        }
        ZoomOutputManager::instance().configure_output(source, 0, true, isolate, mode);
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
        for (const auto &o : ZoomOutputManager::instance().outputs()) {
            if (o.source_name == source) {
                ZoomOutputManager::instance().configure_output(
                    source, o.participant_id, o.active_speaker, isolate, o.audio_mode);
                return;
            }
        }
        blog(LOG_WARNING, "[obs-zoom-plugin] OSC /zoom/isolate_audio: unknown source '%s'",
             source.c_str());
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

void ZoomOscServer::send_status(const QHostAddress &to, quint16 port)
{
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
