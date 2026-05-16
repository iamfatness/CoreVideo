#include "../../src/engine-ipc.h"
#include "engine-writer.h"
#include "engine-video.h"
#include "engine-audio.h"
#include <zoom_sdk.h>
#include <auth_service_interface.h>
#include <meeting_service_interface.h>
#if __has_include(<meeting_service_components/meeting_audio_interface.h>)
#include <meeting_service_components/meeting_audio_interface.h>
#else
#include <meeting_audio_interface.h>
#endif
#if __has_include(<meeting_service_components/meeting_participants_ctrl_interface.h>)
#include <meeting_service_components/meeting_participants_ctrl_interface.h>
#else
#include <meeting_participants_ctrl_interface.h>
#endif
#if __has_include(<meeting_service_components/meeting_raw_archiving_interface.h>)
#include <meeting_service_components/meeting_raw_archiving_interface.h>
#define COREVIDEO_HAS_RAW_ARCHIVING 1
#elif __has_include(<meeting_raw_archiving_interface.h>)
#include <meeting_raw_archiving_interface.h>
#define COREVIDEO_HAS_RAW_ARCHIVING 1
#endif
#if __has_include(<meeting_service_components/meeting_recording_interface.h>)
#include <meeting_service_components/meeting_recording_interface.h>
#define COREVIDEO_HAS_RECORDING_CTRL 1
#elif __has_include(<meeting_recording_interface.h>)
#include <meeting_recording_interface.h>
#define COREVIDEO_HAS_RECORDING_CTRL 1
#endif
#if __has_include(<meeting_service_components/meeting_video_interface.h>)
#include <meeting_service_components/meeting_video_interface.h>
#else
#include <meeting_video_interface.h>
#endif
#include <algorithm>
#include <string>
#include <atomic>
#include <mutex>
#include <vector>

// ── Platform setup ────────────────────────────────────────────────────────────
#if defined(WIN32)
#  include <windows.h>

static std::wstring to_zstr(const std::string &utf8)
{
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], len);
    if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
    return wide;
}

static bool ipc_connect_timeout(HANDLE pipe, DWORD timeout_ms)
{
    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    BOOL ok = ConnectNamedPipe(pipe, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        DWORD waited = WaitForSingleObject(ov.hEvent, timeout_ms);
        if (waited != WAIT_OBJECT_0) {
            CancelIo(pipe);
            CloseHandle(ov.hEvent);
            return false;
        }
        DWORD dummy;
        ok = GetOverlappedResult(pipe, &ov, &dummy, FALSE);
    }
    CloseHandle(ov.hEvent);
    return ok || GetLastError() == ERROR_PIPE_CONNECTED;
}

static bool ipc_setup(IpcFd &p2e, IpcFd &e2p)
{
    p2e = CreateNamedPipeA(PIPE_P2E,
                           PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                           1, 65536, 65536, 0, nullptr);
    e2p = CreateNamedPipeA(PIPE_E2P,
                           PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                           1, 65536, 65536, 0, nullptr);
    if (p2e == INVALID_HANDLE_VALUE || e2p == INVALID_HANDLE_VALUE) return false;
    constexpr DWORD kConnectTimeoutMs = 30000;
    if (!ipc_connect_timeout(p2e, kConnectTimeoutMs) ||
        !ipc_connect_timeout(e2p, kConnectTimeoutMs))
        return false;
    return true;
}

static void ipc_teardown(IpcFd p2e, IpcFd e2p)
{
    CloseHandle(p2e);
    CloseHandle(e2p);
}

static void pump_windows_messages()
{
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

static bool ipc_read_line_with_message_pump(IpcFd fd, std::string &out,
                                            size_t max_len = 65536)
{
    out.clear();
    while (true) {
        char ch = 0;
        DWORD n = 0;
        OVERLAPPED ov = {};
        ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return false;

        BOOL ok = ReadFile(fd, &ch, 1, &n, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            HANDLE event = ov.hEvent;
            while (true) {
                DWORD wait = MsgWaitForMultipleObjects(
                    1, &event, FALSE, 50, QS_ALLINPUT);
                if (wait == WAIT_OBJECT_0) {
                    ok = GetOverlappedResult(fd, &ov, &n, FALSE);
                    break;
                }
                if (wait == WAIT_OBJECT_0 + 1 || wait == WAIT_TIMEOUT) {
                    pump_windows_messages();
                    continue;
                }
                CancelIo(fd);
                CloseHandle(ov.hEvent);
                return false;
            }
        }

        const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(ov.hEvent);
        pump_windows_messages();

        if (!ok && err != ERROR_SUCCESS) return false;
        if (n != 1) return false;
        if (ch == '\n') return true;
        if (out.size() >= max_len) return false;
        out += ch;
    }
}

#else // POSIX (macOS / Linux)
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <poll.h>
#  include <unistd.h>
#  include <cstring>

static std::string to_zstr(const std::string &s) { return s; }

static int unix_listen(const char *path)
{
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) return -1;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    unlink(path); // remove stale socket file
    if (bind(srv, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0 ||
        listen(srv, 1) < 0) {
        close(srv);
        return -1;
    }
    return srv;
}

static int unix_accept_timeout(int srv, int timeout_ms)
{
    struct pollfd pfd = {srv, POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) <= 0) return -1;
    return accept(srv, nullptr, nullptr);
}

static bool ipc_setup(IpcFd &p2e, IpcFd &e2p)
{
    int srv_p2e = unix_listen(SOCK_P2E);
    int srv_e2p = unix_listen(SOCK_E2P);
    if (srv_p2e < 0 || srv_e2p < 0) {
        if (srv_p2e >= 0) close(srv_p2e);
        if (srv_e2p >= 0) close(srv_e2p);
        return false;
    }
    constexpr int kConnectTimeoutMs = 30000; // 30 s
    p2e = unix_accept_timeout(srv_p2e, kConnectTimeoutMs);
    e2p = unix_accept_timeout(srv_e2p, kConnectTimeoutMs);
    close(srv_p2e);
    close(srv_e2p);
    return p2e >= 0 && e2p >= 0;
}

static void ipc_teardown(IpcFd p2e, IpcFd e2p)
{
    close(p2e);
    close(e2p);
    unlink(SOCK_P2E);
    unlink(SOCK_E2P);
}
#endif // platform

// ── Minimal JSON field extraction (no external dependency) ───────────────────

static std::string json_str(const std::string &json, const std::string &key)
{
    const std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    std::string result;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '\\') {
            if (pos < json.size()) pos++; // skip escaped character
            continue;
        }
        if (c == '"') break;
        result += c;
    }
    return result;
}

static uint32_t json_uint(const std::string &json, const std::string &key)
{
    const std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos += needle.size();
    try {
        return static_cast<uint32_t>(std::stoul(json.substr(pos)));
    } catch (...) {
        return 0;
    }
}

static std::string json_escape(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

static const char *meeting_fail_name(int code)
{
    switch (code) {
    case ZOOMSDK::MEETING_FAIL_CONNECTION_ERR:
        return "MEETING_FAIL_CONNECTION_ERR";
    case ZOOMSDK::MEETING_FAIL_RECONNECT_ERR:
        return "MEETING_FAIL_RECONNECT_ERR";
    case ZOOMSDK::MEETING_FAIL_PASSWORD_ERR:
        return "MEETING_FAIL_PASSWORD_ERR";
    case ZOOMSDK::MEETING_FAIL_MEETING_OVER:
        return "MEETING_FAIL_MEETING_OVER";
    case ZOOMSDK::MEETING_FAIL_MEETING_NOT_START:
        return "MEETING_FAIL_MEETING_NOT_START";
    case ZOOMSDK::MEETING_FAIL_MEETING_NOT_EXIST:
        return "MEETING_FAIL_MEETING_NOT_EXIST";
    case ZOOMSDK::MEETING_FAIL_MEETING_USER_FULL:
        return "MEETING_FAIL_MEETING_USER_FULL";
    case ZOOMSDK::MEETING_FAIL_CONFLOCKED:
        return "MEETING_FAIL_CONFLOCKED";
    case ZOOMSDK::MEETING_FAIL_MEETING_RESTRICTED:
        return "MEETING_FAIL_MEETING_RESTRICTED";
    case ZOOMSDK::MEETING_FAIL_ENFORCE_LOGIN:
        return "MEETING_FAIL_ENFORCE_LOGIN";
    case ZOOMSDK::MEETING_FAIL_FORBID_TO_JOIN_INTERNAL_MEETING:
        return "MEETING_FAIL_FORBID_TO_JOIN_INTERNAL_MEETING";
    case ZOOMSDK::MEETING_FAIL_HOST_DISALLOW_OUTSIDE_USER_JOIN:
        return "MEETING_FAIL_HOST_DISALLOW_OUTSIDE_USER_JOIN";
    case ZOOMSDK::MEETING_FAIL_UNABLE_TO_JOIN_EXTERNAL_MEETING:
        return "MEETING_FAIL_UNABLE_TO_JOIN_EXTERNAL_MEETING";
    case ZOOMSDK::MEETING_FAIL_BLOCKED_BY_ACCOUNT_ADMIN:
        return "MEETING_FAIL_BLOCKED_BY_ACCOUNT_ADMIN";
    case ZOOMSDK::MEETING_FAIL_NEED_SIGN_IN_FOR_PRIVATE_MEETING:
        return "MEETING_FAIL_NEED_SIGN_IN_FOR_PRIVATE_MEETING";
    case ZOOMSDK::MEETING_FAIL_APP_PRIVILEGE_TOKEN_ERROR:
        return "MEETING_FAIL_APP_PRIVILEGE_TOKEN_ERROR";
    case ZOOMSDK::MEETING_FAIL_AUTHORIZED_USER_NOT_INMEETING:
        return "MEETING_FAIL_AUTHORIZED_USER_NOT_INMEETING";
    case ZOOMSDK::MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING:
        return "MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING";
    case ZOOMSDK::MEETING_FAIL_ON_BEHALF_TOKEN_CONFLICT_LOGIN_ERROR:
        return "MEETING_FAIL_ON_BEHALF_TOKEN_CONFLICT_LOGIN_ERROR";
    case ZOOMSDK::MEETING_FAIL_USER_LEVEL_TOKEN_NOT_HAVE_HOST_ZAK_OBF:
        return "MEETING_FAIL_USER_LEVEL_TOKEN_NOT_HAVE_HOST_ZAK_OBF";
    case ZOOMSDK::MEETING_FAIL_ON_BEHALF_TOKEN_INVALID:
        return "MEETING_FAIL_ON_BEHALF_TOKEN_INVALID";
    case ZOOMSDK::MEETING_FAIL_ON_BEHALF_TOKEN_NOT_MATCH_MEETING:
        return "MEETING_FAIL_ON_BEHALF_TOKEN_NOT_MATCH_MEETING";
    case ZOOMSDK::MEETING_FAIL_JMAK_USER_EMAIL_NOT_MATCH:
        return "MEETING_FAIL_JMAK_USER_EMAIL_NOT_MATCH";
    default:
        return "MEETING_FAIL_UNKNOWN";
    }
}

// UUID may only contain alphanumerics, hyphens, and underscores to prevent
// path traversal when used as a POSIX shared-memory name.
static bool is_valid_source_uuid(const std::string &uuid)
{
    if (uuid.empty() || uuid.size() > 64) return false;
    for (char c : uuid) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    }
    return true;
}

struct ParticipantInfo {
    uint32_t user_id = 0;
    std::string display_name;
    bool has_video = false;
    bool is_talking = false;
    bool is_muted = false;
};

static std::string zchar_to_utf8(const zchar_t *name)
{
    if (!name) return {};
#if defined(WIN32)
    int len = WideCharToMultiByte(CP_UTF8, 0, name, -1,
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    if (!out.empty()) {
        WideCharToMultiByte(CP_UTF8, 0, name, -1, &out[0], len,
                            nullptr, nullptr);
    }
    return out;
#else
    return name;
#endif
}

class EngineParticipants : public ZOOMSDK::IMeetingParticipantsCtrlEvent,
                           public ZOOMSDK::IMeetingAudioCtrlEvent,
                           public ZOOMSDK::IMeetingVideoCtrlEvent {
public:
    explicit EngineParticipants(IpcFd e2p) : m_e2p(e2p) {}

    void attach(ZOOMSDK::IMeetingParticipantsController *part_ctrl,
                ZOOMSDK::IMeetingAudioController *audio_ctrl,
                ZOOMSDK::IMeetingVideoController *video_ctrl)
    {
        m_ctrl = part_ctrl;
        if (m_ctrl) m_ctrl->SetEvent(this);
        m_audio_ctrl = audio_ctrl;
        if (m_audio_ctrl) m_audio_ctrl->SetEvent(this);
        m_video_ctrl = video_ctrl;
        if (m_video_ctrl) m_video_ctrl->SetEvent(this);
        rebuild_roster();
        send_roster();
    }

    void detach()
    {
        if (m_video_ctrl) {
            m_video_ctrl->SetEvent(nullptr);
            m_video_ctrl = nullptr;
        }
        if (m_audio_ctrl) {
            m_audio_ctrl->SetEvent(nullptr);
            m_audio_ctrl = nullptr;
        }
        if (m_ctrl) {
            m_ctrl->SetEvent(nullptr);
            m_ctrl = nullptr;
        }
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_roster.clear();
            m_active_speaker = 0;
        }
        send_roster();
    }

    void onUserJoin(ZOOMSDK::IList<unsigned int> *, const zchar_t *) override { rebuild_roster(); send_roster(); }
    void onUserLeft(ZOOMSDK::IList<unsigned int> *, const zchar_t *) override { rebuild_roster(); send_roster(); }
    void onUserNamesChanged(ZOOMSDK::IList<unsigned int> *) override { rebuild_roster(); send_roster(); }
    void onUserAudioStatusChange(ZOOMSDK::IList<ZOOMSDK::IUserAudioStatus *> *, const zchar_t *) override { rebuild_roster(); send_roster(); }

    void onUserActiveAudioChange(ZOOMSDK::IList<unsigned int> *lst) override
    {
        uint32_t active = 0;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            active = (lst && lst->GetCount() > 0) ? lst->GetItem(0) : 0;
            m_active_speaker = active;
            for (auto &p : m_roster) p.is_talking = false;
            if (lst) {
                for (int i = 0; i < lst->GetCount(); ++i) {
                    const uint32_t uid = lst->GetItem(i);
                    for (auto &p : m_roster) {
                        if (p.user_id == uid) {
                            p.is_talking = true;
                            break;
                        }
                    }
                }
            }
        }
        EngineIpc::write( R"({"cmd":"active_speaker","participant_id":)" +
                       std::to_string(active) + "}");
        send_roster();
    }

    void onUserVideoStatusChange(unsigned int, ZOOMSDK::VideoStatus) override { rebuild_roster(); send_roster(); }
    void onActiveSpeakerVideoUserChanged(unsigned int userId) override
    {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_active_speaker == 0) m_active_speaker = userId;
        }
        EngineIpc::write( R"({"cmd":"active_speaker","participant_id":)" +
                       std::to_string(userId) + "}");
    }

    void onActiveVideoUserChanged(unsigned int) override {}
    void onSpotlightedUserListChangeNotification(ZOOMSDK::IList<unsigned int> *) override {}
    void onHostRequestStartVideo(ZOOMSDK::IRequestStartVideoHandler *) override {}
    void onUserVideoQualityChanged(ZOOMSDK::VideoConnectionQuality, unsigned int) override {}
    void onHostVideoOrderUpdated(ZOOMSDK::IList<unsigned int> *) override {}
    void onLocalVideoOrderUpdated(ZOOMSDK::IList<unsigned int> *) override {}
    void onFollowHostVideoOrderChanged(bool) override {}
    void onVideoAlphaChannelStatusChanged(bool) override {}
    void onCameraControlRequestReceived(unsigned int, ZOOMSDK::CameraControlRequestType, ZOOMSDK::ICameraControlRequestHandler *) override {}
    void onCameraControlRequestResult(unsigned int, ZOOMSDK::CameraControlRequestResult) override {}
    void onHostChangeNotification(unsigned int) override {}
    void onLowOrRaiseHandStatusChanged(bool, unsigned int) override {}
    void onCoHostChangeNotification(unsigned int, bool) override {}
    void onInvalidReclaimHostkey() override {}
    void onAllHandsLowered() override {}
    void onLocalRecordingStatusChanged(unsigned int, ZOOMSDK::RecordingStatus) override {}
    void onAllowParticipantsRenameNotification(bool) override {}
    void onAllowParticipantsUnmuteSelfNotification(bool) override {}
    void onAllowParticipantsStartVideoNotification(bool) override {}
    void onAllowParticipantsShareWhiteBoardNotification(bool) override {}
    void onRequestLocalRecordingPrivilegeChanged(ZOOMSDK::LocalRecordingRequestPrivilegeStatus) override {}
    void onAllowParticipantsRequestCloudRecording(bool) override {}
    void onInMeetingUserAvatarPathUpdated(unsigned int) override {}
    void onParticipantProfilePictureStatusChange(bool) override {}
    void onFocusModeStateChanged(bool) override {}
    void onFocusModeShareTypeChanged(ZOOMSDK::FocusModeShareType) override {}
    void onBotAuthorizerRelationChanged(unsigned int) override {}
    void onVirtualNameTagStatusChanged(bool, unsigned int) override {}
    void onVirtualNameTagRosterInfoUpdated(unsigned int) override {}
#if defined(WIN32)
    void onCreateCompanionRelation(unsigned int, unsigned int) override {}
    void onRemoveCompanionRelation(unsigned int) override {}
#endif
    void onGrantCoOwnerPrivilegeChanged(bool) override {}
    void onHostRequestStartAudio(ZOOMSDK::IRequestStartAudioHandler *) override {}
    void onJoin3rdPartyTelephonyAudio(const zchar_t *) override {}
    void onMuteOnEntryStatusChange(bool) override {}

private:
    ParticipantInfo user_to_info(ZOOMSDK::IUserInfo *u)
    {
        ParticipantInfo info;
        if (!u) return info;
        info.user_id = u->GetUserID();
        info.display_name = zchar_to_utf8(u->GetUserName());
        info.has_video = u->IsVideoOn();
        info.is_talking = u->IsTalking();
        info.is_muted = u->IsAudioMuted();
        return info;
    }

    void rebuild_roster()
    {
        // Call SDK getters outside our mutex to avoid re-entrant deadlock:
        // the SDK may fire a callback from within GetParticipantsList() on
        // some platforms, which would try to re-acquire m_mtx.
        if (!m_ctrl) return;
        auto *list = m_ctrl->GetParticipantsList();
        if (!list) return;
        std::vector<ParticipantInfo> new_roster;
        new_roster.reserve(static_cast<size_t>(list->GetCount()));
        for (int i = 0; i < list->GetCount(); ++i) {
            auto *user = m_ctrl->GetUserByUserID(list->GetItem(i));
            if (!user) continue;
            new_roster.push_back(user_to_info(user));
        }
        std::lock_guard<std::mutex> lk(m_mtx);
        m_roster = std::move(new_roster);
        m_active_speaker = 0;
        for (const auto &p : m_roster)
            if (p.is_talking) { m_active_speaker = p.user_id; break; }
    }

    void send_roster()
    {
        std::vector<ParticipantInfo> roster;
        uint32_t active = 0;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            roster = m_roster;
            active = m_active_speaker;
        }

        std::string msg = R"({"cmd":"participants","active_speaker_id":)" +
            std::to_string(active) + R"(,"participants":[)";
        for (size_t i = 0; i < roster.size(); ++i) {
            const auto &p = roster[i];
            if (i) msg += ",";
            msg += R"({"id":)" + std::to_string(p.user_id) +
                R"(,"name":")" + json_escape(p.display_name) +
                R"(","has_video":)" + (p.has_video ? "true" : "false") +
                R"(,"is_talking":)" + (p.is_talking ? "true" : "false") +
                R"(,"is_muted":)" + (p.is_muted ? "true" : "false") + "}";
        }
        msg += "]}";
        EngineIpc::write( msg);
    }

    IpcFd m_e2p;
    ZOOMSDK::IMeetingParticipantsController *m_ctrl = nullptr;
    ZOOMSDK::IMeetingAudioController *m_audio_ctrl = nullptr;
    ZOOMSDK::IMeetingVideoController *m_video_ctrl = nullptr;
    std::mutex m_mtx;
    std::vector<ParticipantInfo> m_roster;
    uint32_t m_active_speaker = 0;
};

// ── Auth event handler ────────────────────────────────────────────────────────

class EngineAuthEvent : public ZOOMSDK::IAuthServiceEvent {
public:
    explicit EngineAuthEvent(IpcFd e2p) : m_e2p(e2p) {}

    void onAuthenticationReturn(ZOOMSDK::AuthResult ret) override {
        if (ret == ZOOMSDK::AUTHRET_SUCCESS)
            EngineIpc::write( R"({"cmd":"auth_ok"})");
        else
            EngineIpc::write( R"({"cmd":"auth_fail","code":)" +
                           std::to_string(static_cast<int>(ret)) + "}");
    }
    void onLoginReturnWithReason(ZOOMSDK::LOGINSTATUS,
                                 ZOOMSDK::IAccountInfo *,
                                 ZOOMSDK::LoginFailReason) override {}
    void onLogout() override {}
    void onZoomIdentityExpired() override {
        EngineIpc::write( R"({"cmd":"error","msg":"identity_expired"})");
    }
    void onZoomAuthIdentityExpired() override {}
#if defined(WIN32)
    void onNotificationServiceStatus(ZOOMSDK::SDKNotificationServiceStatus,
                                     ZOOMSDK::SDKNotificationServiceError) override {}
#endif
private:
    IpcFd m_e2p;
};

// ── Meeting event handler ─────────────────────────────────────────────────────

class EngineMeetingEvent : public ZOOMSDK::IMeetingServiceEvent
#if defined(COREVIDEO_HAS_RECORDING_CTRL)
                         , public ZOOMSDK::IMeetingRecordingCtrlEvent
#endif
#if defined(COREVIDEO_HAS_LIVE_STREAM_CTRL)
                         , public ZOOMSDK::IMeetingLiveStreamCtrlEvent
#endif
{
public:
    EngineMeetingEvent(IpcFd e2p, ZOOMSDK::IMeetingService **meeting_svc,
                       EngineParticipants *participants,
                       EngineVideo *video_engine)
        : m_e2p(e2p), m_meeting_svc(meeting_svc),
          m_participants(participants), m_video_engine(video_engine) {}

    void resubscribe_raw_media(const char *reason)
    {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"raw_media_ready","reason":")" +
            std::string(reason ? reason : "unknown") + "\"}");
        m_raw_media_active = true;
        if (m_video_engine)
            m_video_engine->resubscribe_all();
        EngineAudio::instance().retry_subscribe(reason ? reason : "raw_media_ready");
    }

    bool start_raw_media(const char *reason)
    {
        m_raw_media_requested = true;
#if defined(COREVIDEO_HAS_RECORDING_CTRL)
        if (!m_meeting_svc || !*m_meeting_svc) {
            EngineIpc::write(
                R"({"cmd":"error","msg":"raw_media_start_failed","reason":"not_in_meeting"})");
            return false;
        }

        auto *rec = (*m_meeting_svc)->GetMeetingRecordingController();
        if (!rec) {
            EngineIpc::write(
                R"({"cmd":"debug","stage":"recording_controller","code":-1})");
            return false;
        }

        const ZOOMSDK::SDKError set_event = rec->SetEvent(this);
        EngineIpc::write(
            R"({"cmd":"debug","stage":"recording_set_event","code":)" +
            std::to_string(static_cast<int>(set_event)) + "}");
        const ZOOMSDK::SDKError can_raw = rec->CanStartRawRecording();
        EngineIpc::write(
            R"({"cmd":"debug","stage":"can_start_raw_recording","code":)" +
            std::to_string(static_cast<int>(can_raw)) + "}");

        if (can_raw == ZOOMSDK::SDKERR_SUCCESS) {
            const ZOOMSDK::SDKError start_raw = rec->StartRawRecording();
            EngineIpc::write(
                R"({"cmd":"debug","stage":"start_raw_recording","code":)" +
                std::to_string(static_cast<int>(start_raw)) + "}");
            if (start_raw == ZOOMSDK::SDKERR_SUCCESS) {
                resubscribe_raw_media(reason ? reason : "manual_start");
                return true;
            }
            return false;
        }

        const ZOOMSDK::SDKError support_req =
            rec->IsSupportRequestLocalRecordingPrivilege();
        EngineIpc::write(
            R"({"cmd":"debug","stage":"support_recording_privilege_request","code":)" +
            std::to_string(static_cast<int>(support_req)) + "}");
        const ZOOMSDK::SDKError req = rec->RequestLocalRecordingPrivilege();
        EngineIpc::write(
            R"({"cmd":"debug","stage":"request_recording_privilege","code":)" +
            std::to_string(static_cast<int>(req)) + "}");
        return req == ZOOMSDK::SDKERR_SUCCESS;
#else
        EngineIpc::write(
            R"({"cmd":"error","msg":"raw_media_start_failed","reason":"recording_controller_unavailable"})");
        return false;
#endif
    }

    void stop_raw_media(const char *reason)
    {
        m_raw_media_requested = false;
#if defined(COREVIDEO_HAS_RECORDING_CTRL)
        if (m_meeting_svc && *m_meeting_svc) {
            auto *rec = (*m_meeting_svc)->GetMeetingRecordingController();
            if (rec) {
                const ZOOMSDK::SDKError err = rec->StopRawRecording();
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"stop_raw_recording","code":)" +
                    std::to_string(static_cast<int>(err)) + "}");
            }
        }
#endif
        if (m_video_engine)
            m_video_engine->unsubscribe_all();
        EngineAudio::instance().reset_subscription(reason ? reason : "manual_stop");
        m_raw_media_active = false;
        EngineIpc::write(
            R"({"cmd":"debug","stage":"raw_media_stopped","reason":")" +
            std::string(reason ? reason : "manual_stop") + "\"}");
    }

#if defined(COREVIDEO_HAS_LIVE_STREAM_CTRL)
    bool start_raw_live_stream(const char *reason)
    {
        if (!m_meeting_svc || !*m_meeting_svc) return false;
        auto *stream = (*m_meeting_svc)->GetMeetingLiveStreamController();
        if (!stream) {
            EngineIpc::write(
                R"({"cmd":"debug","stage":"raw_live_stream_controller","code":-1})");
            return false;
        }

        const ZOOMSDK::SDKError set_event = stream->SetEvent(this);
        EngineIpc::write(
            R"({"cmd":"debug","stage":"raw_live_stream_set_event","code":)" +
            std::to_string(static_cast<int>(set_event)) + "}");

        const bool supported = stream->IsRawLiveStreamSupported();
        EngineIpc::write(
            R"({"cmd":"debug","stage":"raw_live_stream_supported","supported":)" +
            std::string(supported ? "true" : "false") + "}");
        if (!supported) return false;

        const ZOOMSDK::SDKError can_raw = stream->CanStartRawLiveStream();
        EngineIpc::write(
            R"({"cmd":"debug","stage":"can_start_raw_live_stream","code":)" +
            std::to_string(static_cast<int>(can_raw)) + "}");

#if defined(WIN32)
        const zchar_t *broadcast_url = L"https://corevideo.local/raw";
        const zchar_t *broadcast_name = L"CoreVideo Raw Media";
#else
        const zchar_t *broadcast_url = "https://corevideo.local/raw";
        const zchar_t *broadcast_name = "CoreVideo Raw Media";
#endif
        if (can_raw == ZOOMSDK::SDKERR_SUCCESS) {
            const ZOOMSDK::SDKError start_raw =
                stream->StartRawLiveStreaming(broadcast_url, broadcast_name);
            EngineIpc::write(
                R"({"cmd":"debug","stage":"start_raw_live_stream","code":)" +
                std::to_string(static_cast<int>(start_raw)) + "}");
            if (start_raw == ZOOMSDK::SDKERR_SUCCESS) {
                resubscribe_raw_media(reason ? reason : "raw_live_stream_started");
                return true;
            }
        }

        const ZOOMSDK::SDKError req =
            stream->RequestRawLiveStreaming(broadcast_url, broadcast_name);
        EngineIpc::write(
            R"({"cmd":"debug","stage":"request_raw_live_stream","code":)" +
            std::to_string(static_cast<int>(req)) + "}");
        return false;
    }
#endif

    void onMeetingStatusChanged(ZOOMSDK::MeetingStatus status, int iResult) override {
        EngineIpc::write(R"({"cmd":"debug","stage":"meeting_status","status":)" +
            std::to_string(static_cast<int>(status)) +
            R"(,"result":)" + std::to_string(iResult) + "}");
        switch (status) {
        case ZOOMSDK::MEETING_STATUS_INMEETING:
            EngineIpc::write( R"({"cmd":"joined"})");
            if (m_participants && m_meeting_svc && *m_meeting_svc) {
                m_participants->attach(
                    (*m_meeting_svc)->GetMeetingParticipantsController(),
                    (*m_meeting_svc)->GetMeetingAudioController(),
                    (*m_meeting_svc)->GetMeetingVideoController());
            }
            break;
        case ZOOMSDK::MEETING_STATUS_DISCONNECTING:
        case ZOOMSDK::MEETING_STATUS_ENDED:
            stop_raw_media("meeting_left");
#if defined(COREVIDEO_HAS_LIVE_STREAM_CTRL)
            if (m_meeting_svc && *m_meeting_svc) {
                auto *stream = (*m_meeting_svc)->GetMeetingLiveStreamController();
                if (stream) {
                    stream->SetEvent(nullptr);
                    const ZOOMSDK::SDKError err = stream->StopRawLiveStream();
                    EngineIpc::write(
                        R"({"cmd":"debug","stage":"stop_raw_live_stream","code":)" +
                        std::to_string(static_cast<int>(err)) + "}");
                }
            }
#endif
#if defined(COREVIDEO_HAS_RAW_ARCHIVING)
            if (m_meeting_svc && *m_meeting_svc) {
                auto *raw = (*m_meeting_svc)->GetMeetingRawArchivingController();
                if (raw) {
                    const ZOOMSDK::SDKError err = raw->StopRawArchiving();
                    EngineIpc::write(
                        R"({"cmd":"debug","stage":"stop_raw_archiving","code":)" +
                        std::to_string(static_cast<int>(err)) + "}");
                }
            }
#endif
            if (m_participants) m_participants->detach();
            EngineIpc::write( R"({"cmd":"left"})");
            break;
        case ZOOMSDK::MEETING_STATUS_FAILED:
            EngineAudio::instance().reset_subscription("meeting_failed");
            if (m_participants) m_participants->detach();
            EngineIpc::write( R"({"cmd":"error","msg":"meeting_failed","code":)" +
                           std::to_string(iResult) + R"(,"reason":")" +
                           meeting_fail_name(iResult) + "\"}");
            break;
        default: break;
        }
    }
    void onMeetingStatisticsWarningNotification(ZOOMSDK::StatisticsWarningType) override {}
    void onMeetingParameterNotification(const ZOOMSDK::MeetingParameter *) override {}
    void onSuspendParticipantsActivities() override {}
    void onAICompanionActiveChangeNotice(bool) override {}
    void onMeetingTopicChanged(const zchar_t *) override {}
    void onMeetingFullToWatchLiveStream(const zchar_t *) override {}
    void onUserNetworkStatusChanged(ZOOMSDK::MeetingComponentType,
                                    ZOOMSDK::ConnectionQuality,
                                    unsigned int, bool) override {}
#if defined(WIN32)
    void onAppSignalPanelUpdated(ZOOMSDK::IMeetingAppSignalHandler *) override {}
#endif

#if defined(COREVIDEO_HAS_RECORDING_CTRL)
    void onRecordingStatus(ZOOMSDK::RecordingStatus status) override
    {
        EngineIpc::write(R"({"cmd":"debug","stage":"recording_status","status":)" +
            std::to_string(static_cast<int>(status)) + "}");
    }
    void onCloudRecordingStatus(ZOOMSDK::RecordingStatus status) override
    {
        EngineIpc::write(R"({"cmd":"debug","stage":"cloud_recording_status","status":)" +
            std::to_string(static_cast<int>(status)) + "}");
    }
    void onRecordPrivilegeChanged(bool can_rec) override
    {
        EngineIpc::write(R"({"cmd":"debug","stage":"record_privilege_changed","can_record":)" +
            std::string(can_rec ? "true" : "false") + "}");
    }
    void onLocalRecordingPrivilegeRequestStatus(
        ZOOMSDK::RequestLocalRecordingStatus status) override
    {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"recording_privilege_request_status","status":)" +
            std::to_string(static_cast<int>(status)) + "}");
        if (status != ZOOMSDK::RequestLocalRecording_Granted ||
            !m_raw_media_requested || !m_meeting_svc || !*m_meeting_svc)
            return;

        auto *rec = (*m_meeting_svc)->GetMeetingRecordingController();
        if (!rec) return;
        const ZOOMSDK::SDKError start_raw = rec->StartRawRecording();
        EngineIpc::write(
            R"({"cmd":"debug","stage":"start_raw_recording_after_grant","code":)" +
            std::to_string(static_cast<int>(start_raw)) + "}");
        if (start_raw == ZOOMSDK::SDKERR_SUCCESS)
            resubscribe_raw_media("recording_privilege_granted");
#if defined(COREVIDEO_HAS_LIVE_STREAM_CTRL)
        else
            start_raw_live_stream("recording_grant_start_failed");
#endif
    }
    void onRequestCloudRecordingResponse(
        ZOOMSDK::RequestStartCloudRecordingStatus status) override
    {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"cloud_recording_request_status","status":)" +
            std::to_string(static_cast<int>(status)) + "}");
    }
    void onLocalRecordingPrivilegeRequested(
        ZOOMSDK::IRequestLocalRecordingPrivilegeHandler *) override {}
    void onStartCloudRecordingRequested(
        ZOOMSDK::IRequestStartCloudRecordingHandler *) override {}
#if defined(WIN32)
    void onRecording2MP4Done(bool, int, const zchar_t *) override {}
    void onRecording2MP4Processing(int) override {}
    void onCustomizedLocalRecordingSourceNotification(
        ZOOMSDK::ICustomizedLocalRecordingLayoutHelper *) override {}
#endif
    void onCloudRecordingStorageFull(time_t) override {}
    void onEnableAndStartSmartRecordingRequested(
        ZOOMSDK::IRequestEnableAndStartSmartRecordingHandler *) override {}
    void onSmartRecordingEnableActionCallback(
        ZOOMSDK::ISmartRecordingEnableActionHandler *) override {}
#if defined(__linux__)
    void onTranscodingStatusChanged(ZOOMSDK::TranscodingStatus, const zchar_t *) override {}
#endif
#endif
#if defined(COREVIDEO_HAS_LIVE_STREAM_CTRL)
    void onLiveStreamStatusChange(ZOOMSDK::LiveStreamStatus status) override
    {
        EngineIpc::write(R"({"cmd":"debug","stage":"live_stream_status","status":)" +
            std::to_string(static_cast<int>(status)) + "}");
    }
    void onRawLiveStreamPrivilegeChanged(bool has_privilege) override
    {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"raw_live_stream_privilege_changed","has_privilege":)" +
            std::string(has_privilege ? "true" : "false") + "}");
        if (has_privilege)
            start_raw_live_stream("raw_live_stream_privilege_granted");
    }
    void onRawLiveStreamPrivilegeRequestTimeout() override
    {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"raw_live_stream_privilege_timeout"})");
    }
    void onUserRawLiveStreamPrivilegeChanged(unsigned int userid,
                                             bool has_privilege) override
    {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"user_raw_live_stream_privilege_changed","user_id":)" +
            std::to_string(userid) + R"(,"has_privilege":)" +
            std::string(has_privilege ? "true" : "false") + "}");
    }
    void onRawLiveStreamPrivilegeRequested(
        ZOOMSDK::IRequestRawLiveStreamPrivilegeHandler *) override {}
    void onUserRawLiveStreamingStatusChanged(
        ZOOMSDK::IList<ZOOMSDK::RawLiveStreamInfo> *) override {}
    void onLiveStreamReminderStatusChanged(bool) override {}
    void onLiveStreamReminderStatusChangeFailed() override {}
    void onUserThresholdReachedForLiveStream(int) override {}
#endif
private:
    IpcFd m_e2p;
    ZOOMSDK::IMeetingService **m_meeting_svc = nullptr;
    EngineParticipants *m_participants = nullptr;
    EngineVideo *m_video_engine = nullptr;
    bool m_raw_media_requested = false;
    bool m_raw_media_active = false;
};

// ── Entry point ───────────────────────────────────────────────────────────────

int main()
{
    IpcFd p2e = kIpcInvalidFd;
    IpcFd e2p = kIpcInvalidFd;
    if (!ipc_setup(p2e, e2p)) return 1;
    EngineIpc::init(e2p); // must be called before any SDK callbacks can fire

    EngineIpc::write(R"({"cmd":"ready"})");

    ZOOMSDK::IAuthService    *auth_svc    = nullptr;
    ZOOMSDK::IMeetingService *meeting_svc = nullptr;
    EngineAuthEvent    auth_event(e2p);
    EngineParticipants participants(e2p);
    EngineVideo        video_engine;
    EngineMeetingEvent meeting_event(e2p, &meeting_svc, &participants, &video_engine);

    // Persistent wide-string storage for async SDK calls (JoinParam / AuthContext
    // hold raw pointers — these must outlive the Join/SDKAuth call).
#if defined(WIN32)
    std::wstring g_wide_jwt;
    std::wstring g_wide_name;
    std::wstring g_wide_psw;
    std::wstring g_wide_on_behalf_token;
    std::wstring g_wide_user_zak;
    std::wstring g_wide_app_privilege_token;
#else
    std::string g_wide_jwt;
    std::string g_wide_name;
    std::string g_wide_psw;
    std::string g_wide_on_behalf_token;
    std::string g_wide_user_zak;
    std::string g_wide_app_privilege_token;
#endif

    std::atomic<bool> running{true};
    std::string line;

    while (running) {
#if defined(WIN32)
        if (!ipc_read_line_with_message_pump(p2e, line)) break;
#else
        if (!ipc_read_line(p2e, line)) break; // EOF or connection closed
#endif
        if (line.empty()) continue;

        if (line.find(IPC_CMD_QUIT) != std::string::npos) {
            running = false;

        } else if (line.find(IPC_CMD_INIT) != std::string::npos) {
            std::string jwt = json_str(line, "jwt");
            EngineIpc::write(R"({"cmd":"debug","stage":"init_received"})");

            ZOOMSDK::InitParam init_param;
#if defined(WIN32)
            init_param.strWebDomain = L"https://zoom.us";
#else
            init_param.strWebDomain = "https://zoom.us";
#endif
            init_param.enableGenerateDump = true;
            init_param.rawdataOpts.videoRawdataMemoryMode = ZOOMSDK::ZoomSDKRawDataMemoryModeHeap;
            init_param.rawdataOpts.audioRawdataMemoryMode = ZOOMSDK::ZoomSDKRawDataMemoryModeHeap;
            EngineIpc::write(R"({"cmd":"debug","stage":"before_init_sdk"})");
            ZOOMSDK::SDKError err = ZOOMSDK::InitSDK(init_param);
            EngineIpc::write(R"({"cmd":"debug","stage":"after_init_sdk","code":)" +
                std::to_string(static_cast<int>(err)) + "}");
            if (err != ZOOMSDK::SDKERR_SUCCESS) {
                EngineIpc::write(R"({"cmd":"auth_fail","stage":"init","code":)" +
                    std::to_string(static_cast<int>(err)) + "}");
                continue;
            }

            EngineIpc::write(R"({"cmd":"debug","stage":"before_create_auth"})");
            err = ZOOMSDK::CreateAuthService(&auth_svc);
            EngineIpc::write(R"({"cmd":"debug","stage":"after_create_auth","code":)" +
                std::to_string(static_cast<int>(err)) + "}");
            if (err != ZOOMSDK::SDKERR_SUCCESS || !auth_svc) {
                EngineIpc::write(R"({"cmd":"auth_fail","stage":"create_auth","code":)" +
                    std::to_string(static_cast<int>(err)) + "}");
                continue;
            }
            if (auth_svc) {
                auth_svc->SetEvent(&auth_event);
                g_wide_jwt = to_zstr(jwt); // persists for async SDKAuth call
                ZOOMSDK::AuthContext ctx;
                ctx.jwt_token = g_wide_jwt.c_str();
                EngineIpc::write(R"({"cmd":"debug","stage":"before_sdk_auth"})");
                err = auth_svc->SDKAuth(ctx);
                EngineIpc::write(R"({"cmd":"debug","stage":"after_sdk_auth","code":)" +
                    std::to_string(static_cast<int>(err)) + "}");
                if (err != ZOOMSDK::SDKERR_SUCCESS) {
                    EngineIpc::write(R"({"cmd":"auth_fail","stage":"sdk_auth","code":)" +
                        std::to_string(static_cast<int>(err)) + "}");
                }
            }

        } else if (line.find(IPC_CMD_JOIN) != std::string::npos) {
            std::string meeting_id   = json_str(line, "meeting_id");
            std::string passcode     = json_str(line, "passcode");
            std::string display_name = json_str(line, "display_name");
            std::string on_behalf_token = json_str(line, "on_behalf_token");
            std::string user_zak = json_str(line, "user_zak");
            std::string app_privilege_token = json_str(line, "app_privilege_token");
            if (display_name.empty()) display_name = "OBS";
            EngineIpc::write(R"({"cmd":"debug","stage":"join_received","meeting_id":")" +
                json_escape(meeting_id) + R"(","has_on_behalf_token":)" +
                std::string(on_behalf_token.empty() ? "false" : "true") +
                R"(,"has_user_zak":)" +
                std::string(user_zak.empty() ? "false" : "true") +
                R"(,"has_app_privilege_token":)" +
                std::string(app_privilege_token.empty() ? "false" : "true") + "}");

            if (!meeting_svc) {
                ZOOMSDK::CreateMeetingService(&meeting_svc);
                if (meeting_svc) meeting_svc->SetEvent(&meeting_event);
            }
            if (meeting_svc && !meeting_id.empty()) {
                // Store as persistent variables so JoinParam raw pointers
                // remain valid for the duration of the async Join() call.
                g_wide_name = to_zstr(display_name);
                g_wide_psw  = to_zstr(passcode);
                g_wide_on_behalf_token = to_zstr(on_behalf_token);
                g_wide_user_zak = to_zstr(user_zak);
                g_wide_app_privilege_token = to_zstr(app_privilege_token);

                uint64_t meeting_number = 0;
                try {
                    meeting_number = std::stoull(meeting_id);
                } catch (...) {
                    EngineIpc::write( R"({"cmd":"error","msg":"invalid_meeting_id"})");
                    continue;
                }

                ZOOMSDK::JoinParam jp;
                jp.userType = ZOOMSDK::SDK_UT_WITHOUT_LOGIN;
                ZOOMSDK::JoinParam4WithoutLogin &p = jp.param.withoutloginuserJoin;
                p.meetingNumber             = meeting_number;
                p.userName                  = g_wide_name.c_str();
                p.psw                       = passcode.empty() ? nullptr : g_wide_psw.c_str();
                p.onBehalfToken             = on_behalf_token.empty() ? nullptr : g_wide_on_behalf_token.c_str();
                p.userZAK                   = user_zak.empty() ? nullptr : g_wide_user_zak.c_str();
                p.app_privilege_token       = app_privilege_token.empty() ? nullptr : g_wide_app_privilege_token.c_str();
                p.isVideoOff                = true;
                p.isAudioOff                = false;
                p.isMyVoiceInMix            = true;
                p.eAudioRawdataSamplingRate = ZOOMSDK::AudioRawdataSamplingRate_48K;
                p.eVideoRawdataColorspace   = ZOOMSDK::VideoRawdataColorspace_BT709_F;
                ZOOMSDK::SDKError err = meeting_svc->Join(jp);
                EngineIpc::write(R"({"cmd":"debug","stage":"after_join","code":)" +
                    std::to_string(static_cast<int>(err)) + "}");
            }

        } else if (line.find(IPC_CMD_LEAVE) != std::string::npos) {
            if (meeting_svc)
                meeting_svc->Leave(ZOOMSDK::LEAVE_MEETING);

        } else if (line.find(IPC_CMD_START_MEDIA) != std::string::npos) {
            meeting_event.start_raw_media("manual_start");

        } else if (line.find(IPC_CMD_STOP_MEDIA) != std::string::npos) {
            meeting_event.stop_raw_media("manual_stop");

        } else if (line.find(IPC_CMD_SUBSCRIBE) != std::string::npos) {
            std::string uuid = json_str(line, "source_uuid");
            uint32_t    pid  = json_uint(line, "participant_id");
            const bool isolate_audio =
                line.find(R"("isolate_audio":true)") != std::string::npos;
            if (is_valid_source_uuid(uuid)) {
                video_engine.subscribe(pid, uuid, e2p);
                EngineAudio::instance().init(e2p, uuid, pid, isolate_audio);
            }

        } else if (line.find(IPC_CMD_UNSUBSCRIBE) != std::string::npos) {
            std::string uuid = json_str(line, "source_uuid");
            if (is_valid_source_uuid(uuid)) {
                video_engine.unsubscribe(uuid);
                EngineAudio::instance().remove(uuid);
            }
        }
    }

    if (meeting_svc) meeting_svc->Leave(ZOOMSDK::LEAVE_MEETING);
    EngineAudio::instance().shutdown();
    ZOOMSDK::CleanUPSDK();
    ipc_teardown(p2e, e2p);
    return 0;
}
