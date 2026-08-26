#include "../../src/engine-ipc.h"
#include "../../src/engine-command.h"
#include "engine-writer.h"
#include "engine-video.h"
#include "engine-share.h"
#include "engine-audio.h"
#include "engine-json.h"
#include "engine-talkback.h"
#include <zoom_sdk.h>
#include <auth_service_interface.h>
#include <setting_service_interface.h>
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
#include <exception>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

static std::string redacted_tail(const std::string &value)
{
    if (value.empty()) return "empty";
    if (value.size() <= 4) return "****";
    return "****" + value.substr(value.size() - 4);
}

static const char *auth_result_name(ZOOMSDK::AuthResult ret)
{
    switch (ret) {
    case ZOOMSDK::AUTHRET_SUCCESS:
        return "AUTHRET_SUCCESS";
    case ZOOMSDK::AUTHRET_KEYORSECRETEMPTY:
        return "AUTHRET_KEYORSECRETEMPTY";
    case ZOOMSDK::AUTHRET_KEYORSECRETWRONG:
        return "AUTHRET_KEYORSECRETWRONG";
    case ZOOMSDK::AUTHRET_ACCOUNTNOTSUPPORT:
        return "AUTHRET_ACCOUNTNOTSUPPORT";
    case ZOOMSDK::AUTHRET_ACCOUNTNOTENABLESDK:
        return "AUTHRET_ACCOUNTNOTENABLESDK";
    case ZOOMSDK::AUTHRET_UNKNOWN:
        return "AUTHRET_UNKNOWN";
    case ZOOMSDK::AUTHRET_SERVICE_BUSY:
        return "AUTHRET_SERVICE_BUSY";
    case ZOOMSDK::AUTHRET_NONE:
        return "AUTHRET_NONE";
    case ZOOMSDK::AUTHRET_OVERTIME:
        return "AUTHRET_OVERTIME";
    case ZOOMSDK::AUTHRET_NETWORKISSUE:
        return "AUTHRET_NETWORKISSUE";
    case ZOOMSDK::AUTHRET_CLIENT_INCOMPATIBLE:
        return "AUTHRET_CLIENT_INCOMPATIBLE";
    case ZOOMSDK::AUTHRET_JWTTOKENWRONG:
        return "AUTHRET_JWTTOKENWRONG";
    case ZOOMSDK::AUTHRET_LIMIT_EXCEEDED_EXCEPTION:
        return "AUTHRET_LIMIT_EXCEEDED_EXCEPTION";
    default:
        return "AUTHRET_UNRECOGNIZED";
    }
}

static std::string g_current_auth_mode = "unknown";

// ── Platform setup ────────────────────────────────────────────────────────────
#if defined(WIN32)
#  include <windows.h>

static std::wstring to_zstr(const std::string &utf8)
{
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<size_t>(len), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            utf8.c_str(), -1, wide.data(), len);
    if (written <= 0) return {};
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
// json_str / json_escape / zchar_to_utf8 live in engine-json.h now, shared
// with engine-talkback.cpp (they were `static` here, invisible outside this
// translation unit).

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

// Extracts a JSON array of strings, e.g. "nominees":["Sarah Muller","Luis
// Ortiz"]. Only talkback_nominate uses this today, so it lives here rather
// than in engine-json.h (json_str/json_escape/zchar_to_utf8 moved there
// because engine-talkback.cpp needed them too; this one is main.cpp-only,
// same as json_uint above). Escape handling mirrors json_str(): a backslash
// consumes the next character verbatim rather than decoding it, so a forged
// value cannot collapse into something that matches another string by
// accident. Malformed input (missing key, missing closing bracket, a
// non-string element) yields whatever prefix parsed cleanly rather than
// looping or throwing -- a hostile or buggy peer gets a short/empty list,
// not a hung engine.
//
// TASK 3 FIX ROUND 1 (review, Minor 1): that "short/empty list" was harmless
// while a re-nomination was REFUSED. It is not any more -- an empty list now
// means DENOMINATE, so a truncated pipe line, an absent key, or a display name
// carrying an unescaped quote or bracket would destroy the standing talent
// channels and report the same nominate_done a deliberate denominate reports.
// The destructive interpretation must not also be the FAILURE interpretation.
// `*well_formed` (when non-null) is true only when the key was present as an
// array AND the scan closed on `]` with every element a complete string --
// i.e. only when an empty list the caller is about to act on is one the peer
// actually sent.
static std::vector<std::string> json_str_array(const std::string &json, const std::string &key,
                                               bool *well_formed = nullptr)
{
    std::vector<std::string> out;
    if (well_formed) *well_formed = false;
    const std::string needle = "\"" + key + "\":[";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return out;
    pos += needle.size();
    while (pos < json.size() && json[pos] != ']') {
        if (json[pos] == ',' || json[pos] == ' ') { ++pos; continue; }
        if (json[pos] != '"') return out;   // non-string element: malformed
        ++pos;
        std::string s;
        bool closed = false;
        while (pos < json.size()) {
            char c = json[pos++];
            if (c == '\\') {
                if (pos < json.size()) s += json[pos++];
                continue;
            }
            if (c == '"') { closed = true; break; }
            s += c;
        }
        // An unterminated string means the line was truncated (or a name
        // carried an unescaped quote): everything after it is guesswork.
        if (!closed) return out;
        out.push_back(std::move(s));
    }
    if (pos >= json.size()) return out;     // ran off the end: no closing ']'
    if (well_formed) *well_formed = true;
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
    return std::all_of(uuid.begin(), uuid.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

struct ParticipantInfo {
    uint32_t user_id = 0;
    std::string display_name;
    bool has_video = false;
    bool is_talking = false;
    bool is_muted = false;
    bool is_sharing_screen = false;
};

class EngineParticipants : public ZOOMSDK::IMeetingParticipantsCtrlEvent,
                           public ZOOMSDK::IMeetingAudioCtrlEvent,
                           public ZOOMSDK::IMeetingVideoCtrlEvent,
                           public EngineShareRosterSink {
public:
    explicit EngineParticipants(IpcFd e2p) : m_e2p(e2p) {}

    // Task 4: wires the roster-change path to EngineTalkback's re-resolution
    // without giving this class its own copy of the meeting-service pointer.
    // `meeting_svc_ptr` mirrors EngineMeetingEvent's `&meeting_svc` pattern
    // (main()'s local is reassigned across Join calls; storing the address
    // rather than a snapshot keeps this reading whatever is current). Called
    // once from main() right after both `talkback` and `meeting_svc` exist.
    void attach_talkback(EngineTalkback *talkback,
                         ZOOMSDK::IMeetingService * const *meeting_svc_ptr)
    {
        m_talkback = talkback;
        m_meeting_svc_ptr = meeting_svc_ptr;
    }

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

    void set_active_share_user(uint32_t user_id) override
    {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_active_share_user.store(user_id, std::memory_order_release);
            for (auto &p : m_roster)
                p.is_sharing_screen = user_id != 0 && p.user_id == user_id;
        }
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
            m_active_share_user.store(0, std::memory_order_release);
        }
        send_roster();
    }

    void onUserJoin(ZOOMSDK::IList<unsigned int> *, const zchar_t *) override { roster_changed(); }
    void onUserLeft(ZOOMSDK::IList<unsigned int> *, const zchar_t *) override { roster_changed(); }
    void onUserNamesChanged(ZOOMSDK::IList<unsigned int> *) override { roster_changed(); }
    void onUserAudioStatusChange(ZOOMSDK::IList<ZOOMSDK::IUserAudioStatus *> *, const zchar_t *) override { roster_changed(); }

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
                    const auto participant = std::find_if(
                        m_roster.begin(), m_roster.end(),
                        [uid](const ParticipantInfo &p) {
                            return p.user_id == uid;
                        });
                    if (participant != m_roster.end())
                        participant->is_talking = true;
                }
            }
        }
        EngineIpc::write( R"({"cmd":"active_speaker","participant_id":)" +
                       std::to_string(active) + "}");
        send_roster();
    }

    void onUserVideoStatusChange(unsigned int, ZOOMSDK::VideoStatus) override { roster_changed(); }
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
        info.is_sharing_screen =
            info.user_id == m_active_share_user.load(std::memory_order_acquire);
        return info;
    }

    // Task 4: the single entry point for all five roster-change SDK
    // callbacks (onUserJoin, onUserLeft, onUserNamesChanged,
    // onUserAudioStatusChange, onUserVideoStatusChange). Collapsing the
    // previous "rebuild_roster(); send_roster();" pair each callback wrote
    // by hand into one place is what makes adding the talkback re-resolution
    // a one-line change here instead of five. resolve_roster_change() reads
    // the roster itself (via the participants controller, same as
    // rebuild_roster() above) rather than being handed this class's
    // ParticipantInfo list -- EngineTalkback has no reason to depend on this
    // class's roster shape, and the two already agree on the only fact that
    // matters (who is in the meeting, by name) because both ask the same SDK
    // controller.
    void roster_changed()
    {
        rebuild_roster();
        send_roster();
        if (m_talkback && m_meeting_svc_ptr)
            m_talkback->resolve_roster_change(*m_meeting_svc_ptr);
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
        const auto active = std::find_if(m_roster.begin(), m_roster.end(),
            [](const ParticipantInfo &p) { return p.is_talking; });
        if (active != m_roster.end())
            m_active_speaker = active->user_id;
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
                R"(,"is_muted":)" + (p.is_muted ? "true" : "false") +
                R"(,"is_sharing_screen":)" +
                (p.is_sharing_screen ? "true" : "false") + "}";
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
    std::atomic<uint32_t> m_active_share_user{0};
    // Task 4: set once by attach_talkback() after both `talkback` and
    // `meeting_svc` exist in main(). Never owned here -- see
    // attach_talkback()'s own comment for why a pointer-to-pointer instead
    // of a snapshot.
    EngineTalkback *m_talkback = nullptr;
    ZOOMSDK::IMeetingService * const *m_meeting_svc_ptr = nullptr;
};

// ── Auth event handler ────────────────────────────────────────────────────────

class EngineAuthEvent : public ZOOMSDK::IAuthServiceEvent {
public:
    explicit EngineAuthEvent(IpcFd e2p) : m_e2p(e2p) {}

    void onAuthenticationReturn(ZOOMSDK::AuthResult ret) override {
        if (ret == ZOOMSDK::AUTHRET_SUCCESS) {
            // Opt into HD video so the meeting negotiates Group HD / 1080p
            // streams when the account is entitled. Defaults to off on
            // many account tiers — without this the SDK may downgrade.
            ZOOMSDK::ISettingService *settings = nullptr;
            ZOOMSDK::SDKError s_err =
                ZOOMSDK::CreateSettingService(&settings);
            if (s_err == ZOOMSDK::SDKERR_SUCCESS && settings) {
                if (auto *vs = settings->GetVideoSettings()) {
                    ZOOMSDK::SDKError h_err = vs->EnableHDVideo(true);
                    EngineIpc::write(
                        R"({"cmd":"debug","stage":"enable_hd_video","code":)" +
                        std::to_string(static_cast<int>(h_err)) +
                        R"(,"enabled":)" +
                        std::string(vs->IsHDVideoEnabled() ? "true" : "false") +
                        "}");
                }
                ZOOMSDK::DestroySettingService(settings);
            } else {
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"create_setting_service_failed","code":)" +
                    std::to_string(static_cast<int>(s_err)) + "}");
            }
            EngineIpc::write( R"({"cmd":"auth_ok"})");
        } else
            EngineIpc::write( R"({"cmd":"auth_fail","code":)" +
                           std::to_string(static_cast<int>(ret)) +
                           R"(,"name":")" + auth_result_name(ret) +
                           R"(","auth_mode":")" +
                           json_escape(g_current_auth_mode) + "\"}");
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
                       EngineVideo *video_engine,
                       EngineShare *share_engine)
        : m_e2p(e2p), m_meeting_svc(meeting_svc),
          m_participants(participants), m_video_engine(video_engine),
          m_share_engine(share_engine) {}

    void resubscribe_raw_media(const char *reason)
    {
        EngineIpc::write(
            R"({"cmd":"debug","stage":"raw_media_ready","reason":")" +
            std::string(reason ? reason : "unknown") + "\"}");
        m_raw_media_active = true;
        if (m_video_engine) {
            m_video_engine->set_raw_media_active(true);
            m_video_engine->resubscribe_all();
        }
        if (m_share_engine) {
            m_share_engine->set_raw_media_active(true);
            m_share_engine->resubscribe_all();
        }
        EngineAudio::instance().set_raw_media_active(true);
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

    void set_host_start_fallback(uint64_t meeting_number,
                                 const std::string &display_name,
                                 const std::string &user_zak)
    {
        m_host_start_attempted = false;
        m_host_start_meeting_number = meeting_number;
        m_host_start_name = to_zstr(display_name);
        m_host_start_zak = to_zstr(user_zak);
    }

    void clear_host_start_fallback()
    {
        m_host_start_attempted = false;
        m_host_start_meeting_number = 0;
        m_host_start_name.clear();
        m_host_start_zak.clear();
    }

    bool try_host_start_after_external_join_failure()
    {
        if (m_host_start_attempted || m_host_start_meeting_number == 0 ||
            m_host_start_zak.empty() || !m_meeting_svc || !*m_meeting_svc) {
            return false;
        }
        m_host_start_attempted = true;

        ZOOMSDK::StartParam sp;
        sp.userType = ZOOMSDK::SDK_UT_WITHOUT_LOGIN;
        ZOOMSDK::StartParam4WithoutLogin &p = sp.param.withoutloginStart;
        p.meetingNumber = m_host_start_meeting_number;
        p.userZAK = m_host_start_zak.c_str();
        p.userName = m_host_start_name.empty() ? nullptr : m_host_start_name.c_str();
        p.zoomuserType = ZOOMSDK::ZoomUserType_APIUSER;
        p.isVideoOff = true;
        p.isAudioOff = false;
        p.isMyVoiceInMix = true;
        p.eAudioRawdataSamplingRate = ZOOMSDK::AudioRawdataSamplingRate_48K;
        p.eVideoRawdataColorspace = ZOOMSDK::VideoRawdataColorspace_BT709_F;

        const ZOOMSDK::SDKError err = (*m_meeting_svc)->Start(sp);
        EngineIpc::write(R"({"cmd":"debug","stage":"host_start_after_external_join_failure","code":)" +
                         std::to_string(static_cast<int>(err)) + "}");
        return err == ZOOMSDK::SDKERR_SUCCESS;
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
        if (m_video_engine) {
            m_video_engine->set_raw_media_active(false);
            // suspend_all() KEEPS the desired-state map so a later
            // resubscribe_all() can rebuild every source. That is right for
            // the operator stopping and restarting raw recording INSIDE one
            // meeting -- forgetting the intent there made sources come back
            // empty and be re-picked by hand, on air.
            //
            // It is WRONG at a meeting boundary, which is why
            // clear_media_intent() below exists and why DISCONNECTING/ENDED
            // calls it. Zoom user IDs are meeting-scoped 32-bit values, so
            // intent carried from meeting A into meeting B can resubscribe a
            // colliding ID and put the WRONG PARTICIPANT on an output. Do not
            // re-unify these two paths.
            m_video_engine->suspend_all();
        }
        if (m_share_engine)
            m_share_engine->set_raw_media_active(false);
        EngineAudio::instance().set_raw_media_active(false);
        EngineAudio::instance().reset_subscription(reason ? reason : "manual_stop");
        m_raw_media_active = false;
        EngineIpc::write(
            R"({"cmd":"debug","stage":"raw_media_stopped","reason":")" +
            std::string(reason ? reason : "manual_stop") + "\"}");
    }

    // Meeting-boundary teardown, on top of stop_raw_media(). Leaving meeting A
    // and joining meeting B in the same engine process must NOT carry meeting
    // A's participant IDs into B's subscribes -- see the suspend_all() comment
    // above. Only the DISCONNECTING/ENDED path calls this; the operator's
    // stop/start of raw recording deliberately does not.
    void clear_media_intent(const char *reason)
    {
        if (m_video_engine)
            m_video_engine->unsubscribe_all();
        EngineIpc::write(
            R"({"cmd":"debug","stage":"media_intent_cleared","reason":")" +
            std::string(reason ? reason : "meeting_left") + "\"}");
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

        // A waiting room, or a meeting that has not started yet, is an
        // open-ended wait on a human -- not a stalled join. Neither state
        // produces any further status change until someone acts, so the
        // plugin's join watchdog could not tell them apart from a wedge and
        // auto-left a real meeting after two minutes in the waiting room
        // (2026-08-22). Reported as its own event rather than left for the
        // plugin to infer from the debug line above: debug output is for
        // humans and is filtered by stage, and control flow must not depend
        // on it. Sent on EVERY status change, including the ones that end the
        // wait, so the flag needs no edge tracking on either side.
        {
            const bool awaiting =
                status == ZOOMSDK::MEETING_STATUS_IN_WAITING_ROOM ||
                status == ZOOMSDK::MEETING_STATUS_WAITINGFORHOST;
            EngineIpc::write(
                std::string(R"({"cmd":"awaiting_admission","active":)") +
                (awaiting ? "true" : "false") + "}");
        }

        switch (status) {
        case ZOOMSDK::MEETING_STATUS_INMEETING:
            EngineIpc::write( R"({"cmd":"joined"})");
            if (m_participants && m_meeting_svc && *m_meeting_svc) {
                auto *part_ctrl = (*m_meeting_svc)->GetMeetingParticipantsController();
                if (!part_ctrl) {
                    // Without the participants controller we cannot build a
                    // roster (GetParticipantsList/GetUserByUserID live on it);
                    // surface it rather than silently attaching null.
                    EngineIpc::write(
                        R"({"cmd":"debug","stage":"participants_controller","code":-1})");
                }
                m_participants->attach(
                    part_ctrl,
                    (*m_meeting_svc)->GetMeetingAudioController(),
                    (*m_meeting_svc)->GetMeetingVideoController());
            }
            if (m_share_engine && m_meeting_svc && *m_meeting_svc) {
                auto *share_ctrl = (*m_meeting_svc)->GetMeetingShareController();
                if (!share_ctrl) {
                    EngineIpc::write(
                        R"({"cmd":"debug","stage":"share_controller","code":-1})");
                }
                m_share_engine->attach(share_ctrl);
            }
            // Re-entering a session (breakout room join or leave, or a
            // reconnect) does not pass through DISCONNECTING/ENDED, so
            // stop_raw_media() never runs and m_raw_media_active is still true
            // from the session we just left. The SDK has meanwhile revoked the
            // local-recording privilege, so every createRenderer() call
            // returns SDKERR_NO_PERMISSION (12) and each source fails
            // video_subscribe_failed_all -- observed on 2026-08-15 and again
            // on 2026-08-16, both times a breakout round trip.
            //
            // Treat re-entry as "permission unknown": drop the stale active
            // flag and run the ordinary request path again. If the privilege
            // is still held, CanStartRawRecording() succeeds and
            // start_raw_media() resubscribes immediately; if it is not,
            // RequestLocalRecordingPrivilege() runs and
            // onLocalRecordingPrivilegeRequestStatus() resubscribes on grant.
            // Both are already-proven paths.
            //
            // Deliberately NOT stop_raw_media(): that clears
            // m_raw_media_requested, throwing away the operator's intent, and
            // calls StopRawRecording() on a session where recording is already
            // not running.
            if (m_raw_media_requested && m_raw_media_active) {
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"raw_media_rearm","reason":"session_reentry"})");
                m_raw_media_active = false;
                if (m_video_engine) {
                    m_video_engine->set_raw_media_active(false);
                    m_video_engine->suspend_all();
                }
                if (m_share_engine)
                    m_share_engine->set_raw_media_active(false);
                EngineAudio::instance().set_raw_media_active(false);
                EngineAudio::instance().reset_subscription("session_reentry");
                start_raw_media("session_reentry");
            }
            break;
        case ZOOMSDK::MEETING_STATUS_DISCONNECTING:
        case ZOOMSDK::MEETING_STATUS_ENDED:
            stop_raw_media("meeting_left");
            // A meeting boundary genuinely ends the intent: the next join is a
            // different meeting with its own, unrelated 32-bit user IDs.
            clear_media_intent("meeting_left");
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
            if (m_share_engine) m_share_engine->detach();
            EngineIpc::write( R"({"cmd":"left"})");
            break;
        case ZOOMSDK::MEETING_STATUS_FAILED:
            if (iResult == ZOOMSDK::MEETING_FAIL_UNABLE_TO_JOIN_EXTERNAL_MEETING &&
                try_host_start_after_external_join_failure()) {
                EngineIpc::write(
                    R"({"cmd":"debug","stage":"external_join_failed_retrying_host_start"})");
                break;
            }
            EngineAudio::instance().reset_subscription("meeting_failed");
            if (m_participants) m_participants->detach();
            if (m_share_engine) m_share_engine->detach();
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
    EngineShare *m_share_engine = nullptr;
    bool m_raw_media_requested = false;
    bool m_raw_media_active = false;
    bool m_host_start_attempted = false;
    uint64_t m_host_start_meeting_number = 0;
    std::basic_string<zchar_t> m_host_start_name;
    std::basic_string<zchar_t> m_host_start_zak;
};

// ── Entry point ───────────────────────────────────────────────────────────────

int main()
{
    IpcFd p2e = kIpcInvalidFd;
    IpcFd e2p = kIpcInvalidFd;
    if (!ipc_setup(p2e, e2p)) return 1;
    EngineIpc::init(e2p); // must be called before any SDK callbacks can fire

    // An uncaught exception -- ours or the SDK's, on ANY thread -- otherwise
    // dies as a bare 0xc0000409 fastfail in ucrtbase with nothing in our log
    // (observed 2026-08-17: four such crashes in a reconnect loop while two
    // OBS instances collided over this process's singletons, and the only
    // evidence was Windows Error Reporting). Exit with a distinct code
    // instead: the plugin already logs "ZoomObsEngine exited unexpectedly
    // (code N)", so the code alone identifies terminate() vs a real crash.
    //
    // Deliberately NOTHING ELSE in this handler. A first version wrote a
    // {"cmd":"error"} line to the pipe -- but EngineIpc::write takes a global
    // mutex and then blocks in WriteFile with no timeout, and a FULL PIPE is
    // precisely the condition most likely to have caused the terminate; the
    // handler would hang the process instead of exiting, turning a
    // diagnosable exit into a heartbeat timeout. It also allocated (a
    // bad_alloc-induced terminate would re-enter), and exit code 3 is already
    // mapped to RecoveryReason::SdkError, silently re-routing crashes around
    // the operator's on_engine_crash reconnect policy. Code 5 is unmapped and
    // falls through to the default EngineCrash classification.
    std::set_terminate([]() { _exit(5); });

    EngineIpc::write(R"({"cmd":"ready"})");

    ZOOMSDK::IAuthService    *auth_svc    = nullptr;
    ZOOMSDK::IMeetingService *meeting_svc = nullptr;
    EngineAuthEvent    auth_event(e2p);
    EngineParticipants participants(e2p);
    EngineVideo        video_engine;
    EngineShare        share_engine(&participants);
    EngineMeetingEvent meeting_event(e2p, &meeting_svc, &participants,
                                     &video_engine, &share_engine);
    // Static storage duration, not a plain local: the TalkbackProbe branch
    // below runs a thread that calls talkback.tick()/is_idle() for up to
    // ~30s. A plain local's lifetime ends when main() returns, and a thread
    // still running against it at that point is a use-after-free; static
    // duration means the object outlives that, so the driving thread can
    // never dangle a reference to it (Ruling B, task-5-brief).
    static EngineTalkback talkback;
    // Task 4: wire the roster-change path (EngineParticipants' five SDK
    // callbacks) to EngineTalkback's re-resolution. Deferred until here
    // because it needs `talkback`, which is declared on this line -- and
    // `&meeting_svc` rather than its current value, since Join reassigns it
    // and this must always see the meeting the roster events belong to (same
    // pattern as EngineMeetingEvent's own `&meeting_svc` above).
    participants.attach_talkback(&talkback, &meeting_svc);
    // The thread is kept joinable (never detached) and joined explicitly --
    // both before starting a fresh one and, critically, before any SDK
    // teardown call at process exit (see the Quit path below). talkback.tick()
    // calls through m_ctrl/m_svc, raw ZOOMSDK pointers that CleanUPSDK()
    // invalidates; a detached thread has no point at which the engine can
    // prove it is no longer touching those pointers before tearing the SDK
    // down (review round 4 finding).
    static std::thread talkback_thread;
    // Lets the driving loop exit promptly on Quit instead of running out its
    // full ~30s bound before main() can join it.
    static std::atomic<bool> talkback_stop{false};

    // Persistent wide-string storage for async SDK calls (JoinParam / AuthContext
    // hold raw pointers — these must outlive the Join/SDKAuth call).
#if defined(WIN32)
    std::wstring g_wide_jwt;
    std::wstring g_wide_public_app_key;
    std::wstring g_wide_name;
    std::wstring g_wide_psw;
    std::wstring g_wide_on_behalf_token;
    std::wstring g_wide_user_zak;
    std::wstring g_wide_app_privilege_token;
#else
    std::string g_wide_jwt;
    std::string g_wide_public_app_key;
    std::string g_wide_name;
    std::string g_wide_psw;
    std::string g_wide_on_behalf_token;
    std::string g_wide_user_zak;
    std::string g_wide_app_privilege_token;
#endif

    std::atomic<bool> running{true};
    std::string line;

    // Heartbeat: emit a ping every ~2s so the plugin can detect a hung-but-alive
    // engine (process running, pipe silent). Stop the loop if a write fails.
    std::thread heartbeat([&running]() {
        while (running.load(std::memory_order_acquire)) {
            for (int i = 0; i < 20 && running.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!running.load(std::memory_order_acquire)) break;
            // cppcheck-suppress knownConditionTrueFalse ; false positive --
            // cppcheck's bounded (--check-level=normal) branch analysis of
            // ipc_write_line() (engine-ipc.h), which has multiple return
            // points inside a retry loop, misreads it as always returning
            // false. It legitimately returns true on a successful write.
            if (!EngineIpc::write(R"({"cmd":"ping"})")) break;
        }
    });

    while (running) {
#if defined(WIN32)
        if (!ipc_read_line_with_message_pump(p2e, line)) break;
#else
        if (!ipc_read_line(p2e, line)) break; // EOF or connection closed
#endif
        if (line.empty()) continue;

        // Exact match on the declared "cmd" field, never a substring of the
        // line: "unsubscribe" contains "subscribe", and testing substrings in
        // source order routed every unsubscribe into the subscribe branch.
        // See src/engine-command.h.
        const IpcCommand command = ipc_command_of(line);

        if (command == IpcCommand::Quit) {
            running = false;

        } else if (command == IpcCommand::Init) {
            std::string jwt = json_str(line, "jwt");
            std::string public_app_key = json_str(line, "public_app_key");
            EngineIpc::write(R"({"cmd":"debug","stage":"init_received"})");

            ZOOMSDK::InitParam init_param;
#if defined(WIN32)
            init_param.strWebDomain = L"https://zoom.us";
#else
            init_param.strWebDomain = "https://zoom.us";
#endif
            init_param.enableGenerateDump = true;
            init_param.enableLogByDefault = true;
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
            auth_svc->SetEvent(&auth_event);
            ZOOMSDK::AuthContext ctx{};
            if (!public_app_key.empty()) {
                jwt.clear();
                g_wide_jwt.clear();
                g_wide_public_app_key = to_zstr(public_app_key);
                ctx.publicAppKey = g_wide_public_app_key.c_str();
                ctx.jwt_token = nullptr;
                g_current_auth_mode = "public_app_key";
            } else {
                g_wide_public_app_key.clear();
                g_wide_jwt = to_zstr(jwt); // persists for async SDKAuth call
                ctx.jwt_token = g_wide_jwt.c_str();
                g_current_auth_mode = "jwt";
            }
            EngineIpc::write(
                R"({"cmd":"debug","stage":"before_sdk_auth","auth_mode":")" +
                std::string(public_app_key.empty() ? "jwt" : "public_app_key") +
                R"(","jwt_present":)" +
                std::string(jwt.empty() ? "false" : "true") +
                R"(,"public_app_key_present":)" +
                std::string(public_app_key.empty() ? "false" : "true") +
                R"(,"public_app_key_tail":")" +
                json_escape(redacted_tail(public_app_key)) + "\"" +
                "}");
            err = auth_svc->SDKAuth(ctx);
            EngineIpc::write(R"({"cmd":"debug","stage":"after_sdk_auth","code":)" +
                std::to_string(static_cast<int>(err)) + "}");
            if (err != ZOOMSDK::SDKERR_SUCCESS) {
                EngineIpc::write(R"({"cmd":"auth_fail","stage":"sdk_auth","code":)" +
                    std::to_string(static_cast<int>(err)) + "}");
            }

        } else if (command == IpcCommand::Join) {
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
                if (!user_zak.empty())
                    meeting_event.set_host_start_fallback(meeting_number, display_name, user_zak);
                else
                    meeting_event.clear_host_start_fallback();

                ZOOMSDK::JoinParam jp;
                jp.userType = ZOOMSDK::SDK_UT_WITHOUT_LOGIN;
                ZOOMSDK::JoinParam4WithoutLogin &p = jp.param.withoutloginuserJoin;
                p.meetingNumber             = meeting_number;
                p.userName                  = g_wide_name.c_str();
                p.psw                       = passcode.empty() ? nullptr : g_wide_psw.c_str();
                p.onBehalfToken             = on_behalf_token.empty() ? nullptr : g_wide_on_behalf_token.c_str();
                p.userZAK                   = user_zak.empty() ? nullptr : g_wide_user_zak.c_str();
                p.app_privilege_token       = app_privilege_token.empty() ? nullptr : g_wide_app_privilege_token.c_str();
                p.isVideoOff                = false;
                p.isAudioOff                = false;
                p.isMyVoiceInMix            = true;
                p.eAudioRawdataSamplingRate = ZOOMSDK::AudioRawdataSamplingRate_48K;
                p.eVideoRawdataColorspace   = ZOOMSDK::VideoRawdataColorspace_BT709_F;
                ZOOMSDK::SDKError err = meeting_svc->Join(jp);
                EngineIpc::write(R"({"cmd":"debug","stage":"after_join","code":)" +
                    std::to_string(static_cast<int>(err)) + "}");
            }

        } else if (command == IpcCommand::TalkbackProbe) {
            const std::string who = json_str(line, "participant");
            if (!meeting_svc) {
                EngineIpc::write(
                    R"({"cmd":"talkback_probe","stage":"controller","ok":false,)"
                    R"("reason":"not_in_meeting"})");
            } else if (!talkback.is_idle()) {
                // A ladder is live. Refuse immediately -- no join, no block.
                // Joining first (an earlier round of this change did exactly
                // that) blocks THIS command -- the single ipc_read_line
                // thread -- for up to the full ~30s driving-thread bound,
                // during which leave/stop_engine/quit (quit only sets a flag
                // this same loop must come back around to see) all stall.
                // Refusing is correct for a probe; blocking the one reader
                // thread to wait one out is not (review round 3 finding).
                //
                // INVARIANT: this branch must touch NOTHING but the pipe --
                // no talkback.probe() call, no talkback_thread, no
                // talkback_stop. "Not idle" is exactly the state that can
                // change out from under this branch: a callback thread can
                // flip phase to Done between the is_idle() check above and
                // any further action here (the same onCreateChannelResponse
                // early-failure path the idle branch below cites). A
                // talkback.probe() call made from here would then see
                // Idle/Done, pass ITS OWN guard, and silently start a real
                // ladder -- CreateChannel goes out to Zoom -- with no driving
                // thread ever spawned for it (we're in the refusal branch).
                // That ladder can never time out or self-destroy (tick() is
                // what runs kAwaitTimeout and the Destroying retries), so it
                // leaks a live Zoom channel for the rest of the meeting and
                // wedges every later talkback_probe in this same branch
                // forever -- and the operator sees no "busy", so nothing
                // even hints a probe was silently dropped (review round 4
                // finding). A bare pipe write has no guard for a callback
                // thread to race, so it's the only action here that's safe
                // regardless of what phase does after the check above.
                EngineIpc::write(
                    R"({"cmd":"talkback_probe","stage":"busy",)"
                    R"("reason":"a talkback probe is already in progress"})");
            } else {
                // is_idle() is a plain, non-atomic-compound check-then-act
                // here, and that is safe ONLY because of two facts specific
                // to this call site: (1) this command loop is the sole
                // caller of probe() -- no other thread can start a new
                // ladder between the check above and the join/probe() below,
                // so nothing can flip phase from Done back into "busy" out
                // from under us; (2) every SDK-callback-thread path in
                // engine-talkback.cpp only ever advances phase TOWARD Done
                // (AwaitingChannel/Invite/Sending/Destroying -> Done), never
                // away from it, so once Idle/Done is observed here it stays
                // that way until THIS thread calls probe() again. That also
                // means the driving thread from a PRIOR probe, if still
                // joinable, has already tripped its own is_idle() exit (or
                // will within one 10ms tick if a callback thread set Done
                // directly, e.g. onCreateChannelResponse's early-failure
                // path) -- so this join costs at most one tick interval, not
                // the ~30s bound.
                if (talkback_thread.joinable()) talkback_thread.join();
                talkback_stop.store(false, std::memory_order_release);

                // probe() returns true if and only if it issued a
                // CreateChannel -- every refusal, not just the re-entrancy
                // guard, returns false. Spawning the driving thread only on
                // true is what keeps "a ladder started" and "a driver is
                // running" the same fact. That matters beyond thread count:
                // the driving thread is the only thread besides this one
                // that drives the batch-destroy API, and a thread spawned
                // for a probe that created nothing can be draining strays
                // while an onCreateChannelResponse branch destroys on this
                // thread. Do not "helpfully" spawn it on a refusal to get
                // strays drained -- probe() drains them itself on those
                // paths. See probe()'s return-value contract in
                // engine-talkback.h.
                if (talkback.probe(meeting_svc, who)) {
                    // Drive tick() off a dedicated thread: ipc_read_line_with_
                    // message_pump() blocks, so tick() cannot live on the read
                    // loop's thread. Bounded at 3000 x 10ms (~30s), not the
                    // ~12s a naive read of the probe's own "~3s of tone"
                    // comment suggests -- AwaitingChannel and AwaitingInvite
                    // each carry their own 10s timeout (kAwaitTimeout in
                    // engine-talkback.cpp) before falling through to
                    // Destroying, which itself retries up to
                    // kMaxDestroyAttempts times. Worst case is 10s timeout
                    // plus several destroy retries; 12s could expire the
                    // thread mid-destroy and strand a channel -- exactly what
                    // the destroy-retry machinery exists to prevent (Ruling A,
                    // task-5-brief). is_idle() breaks the loop as soon as the
                    // ladder settles so the happy path does not spin the full
                    // bound; talkback_stop lets Quit break it early too.
                    talkback_thread = std::thread([]() {
                        for (int i = 0; i < 3000; ++i) {
                            if (talkback_stop.load(std::memory_order_acquire)) break;
                            talkback.tick();
                            // has_pending_work(), not is_idle(): a stray
                            // channel queued by a late/duplicate
                            // onCreateChannelResponse can leave the ladder
                            // itself Idle/Done while drain_stray_channels()
                            // (reached from tick(), which only this thread
                            // drives, and from probe()'s own refusal paths,
                            // which run only when this thread does not
                            // exist) still has not run for it. If
                            // this loop exited on is_idle() alone it could
                            // stop right after AwaitingChannel's 10s timeout
                            // -> Destroying -> Done settles, and then a
                            // genuinely late create_channel_response arrives
                            // at t+11s, queues a real Zoom channel, and
                            // nothing ever destroys it (F3 review-round
                            // finding). is_idle() is still exactly right for
                            // the refusal gate above -- see the comment on
                            // has_pending_work() in engine-talkback.h for why
                            // the two must not be conflated.
                            if (!talkback.has_pending_work()) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                    });
                }
            }

        } else if (command == IpcCommand::TalkbackOpen) {
            talkback.open_audio(json_str(line, "region"),
                                static_cast<uint32_t>(json_uint(line, "rate")),
                                static_cast<uint16_t>(json_uint(line, "channels")));

        } else if (command == IpcCommand::TalkbackAudio) {
            // Drained on THIS thread deliberately: on Windows this loop is
            // also the SDK's message-pump thread (see
            // ipc_read_line_with_message_pump above), so every SDK call stays
            // on the thread the SDK already uses. The Milestone 1 probe's
            // separate driving thread was the first in this engine to call SDK
            // APIs off the pump; this path does not repeat that.
            talkback.drain_audio();

        } else if (command == IpcCommand::TalkbackClose) {
            talkback.close_audio();

        } else if (command == IpcCommand::TalkbackStart) {
            // F3 review-round fix: session_stop()'s own comment used to
            // claim R1's mutual exclusion (probe() refuses while
            // m_session_live, session_start() refuses while
            // has_pending_work()) alone guarantees the probe's driving
            // thread can never be mid-tick() while the session runs -- but
            // that gate is a single-instant check made when a probe or a
            // session STARTS. It says nothing about a driving thread that
            // already passed has_pending_work(), went to sleep_for(10ms),
            // and wakes again after this branch has since started a
            // session. Mirror the TalkbackProbe branch above: join the
            // driving thread here too, BEFORE ever calling session_start(),
            // so no probe ladder can still be ticking when a session starts.
            if (talkback_thread.joinable()) talkback_thread.join();
            // Task 3: a key press names a TARGET (kTalkbackAllTalentTarget or
            // a nominee's name), not a participant to open a channel for --
            // session_start() now selects an already-provisioned channel.
            // Task 5 fix round 1 (F7): the "participant" fallback that used
            // to live here was a compatibility shim for the plugin's
            // pre-Task-5 wire shape, with an explicit "delete once Task 5
            // ships" note. Task 5 shipped and the review confirmed the new
            // shape end-to-end (src/zoom-engine-client.cpp sends "target"
            // unconditionally) -- deleted.
            const std::string target = json_str(line, "target");
            talkback.session_start(meeting_svc, target);

        } else if (command == IpcCommand::TalkbackStop) {
            talkback.session_stop();

        } else if (command == IpcCommand::TalkbackNominate) {
            // Mirror the TalkbackStart branch above: join the probe's
            // driving thread first so nominate() (which reassigns
            // m_svc/m_ctrl, same as session_start()) can never race it --
            // see the comment there.
            if (talkback_thread.joinable()) talkback_thread.join();
            // Task 3 fix round 1 (Minor 1): an empty nominee list is a
            // DELIBERATE denominate -- it destroys the standing set -- so a
            // request whose list could not be parsed must never reach
            // nominate(). Refuse loudly instead; the operator retries, and the
            // talent channels are still standing when they do.
            bool nominees_ok = false;
            const std::vector<std::string> nominees =
                json_str_array(line, "nominees", &nominees_ok);
            if (!nominees_ok) {
                EngineIpc::write(
                    R"({"cmd":"talkback_nominate","stage":"nominate","ok":false,)"
                    R"("reason":"malformed_nominees"})");
            } else {
                // Fix round 2 (N1): this bool used to be discarded outright.
                // nominate() reports its own terminal outcome on every
                // return-false path today (that discipline is the actual
                // fix for N1 -- see nomination_create_next()'s two abort
                // branches), so this is a backstop, not the primary signal:
                // if a FUTURE path inside nominate() ever returns false
                // without having reported anything, a silently discarded
                // bool is exactly how that would go unnoticed again. This
                // "debug" line is diagnostic-only -- handle_event()'s
                // generic "debug" branch just logs it, it does not
                // participate in the plugin's nomination state machine, so
                // it can never double up with report_nomination()'s own
                // terminal line.
                if (!talkback.nominate(meeting_svc, nominees)) {
                    EngineIpc::write(
                        R"({"cmd":"debug","stage":"nominate_returned_false"})");
                }
            }

        } else if (command == IpcCommand::Leave) {
            // F4 review-round fix: mirror the quit path below (see the
            // "Join the talkback driving thread BEFORE any SDK teardown
            // call" comment near the bottom of main()) -- the same
            // constraint applies here. tick() may be mid SDK-call on the
            // driving thread via m_ctrl/m_svc (raw ZOOMSDK pointers
            // captured at probe() time) at the moment this command-thread
            // branch calls Leave() through meeting_svc; that is the same
            // "SDK callbacks racing teardown" class the quit path already
            // guards against. Signal stop first so this does not wait out
            // the full ~30s bound -- in the common case is_idle() has
            // already stopped the loop well before that, so the join costs
            // at most one ~10ms tick.
            talkback_stop.store(true, std::memory_order_release);
            if (talkback_thread.joinable()) talkback_thread.join();
            // F4 review-round fix: only the explicit talkback_close branch
            // used to call this. Leave() destroys the talkback channel
            // meeting-side, but m_audio_open stayed true and the ring
            // region stayed mapped -- a director who never sent
            // talkback_close before leaving left the engine holding a
            // mapping (and, once the destroy-path fix elsewhere in this
            // round clears m_channel_id_z, a channel id that no longer
            // exists) for as long as the process lives. close_audio() is
            // idempotent (bails immediately if !m_audio_open), so calling
            // it here is safe even when talkback was never opened.
            talkback.close_audio();
            // The persistent session is a separate channel from the probe's
            // (see engine-talkback.h) and needs its own explicit teardown
            // here for the same reason close_audio() does: Leave() destroys
            // meeting-side state out from under it otherwise. session_stop()
            // is idempotent (bails immediately if nothing is live), so this
            // is safe even when no session was ever started.
            talkback.session_stop();
            // Provisioned channels and their membership are meeting-scoped
            // (design doc's "Meeting rejoin" row) -- once Leave() below
            // runs there is nothing left on Zoom's side for the nomination
            // table to reference. Bookkeeping only, no SDK call; see
            // nomination_reset()'s own comment.
            talkback.nomination_reset();
            if (meeting_svc)
                meeting_svc->Leave(ZOOMSDK::LEAVE_MEETING);

        } else if (command == IpcCommand::StartMedia) {
            meeting_event.start_raw_media("manual_start");

        } else if (command == IpcCommand::StopMedia) {
            meeting_event.stop_raw_media("manual_stop");

        } else if (command == IpcCommand::SubscribeAudio) {
            std::string uuid = json_str(line, "source_uuid");
            uint32_t    pid  = json_uint(line, "participant_id");
            const bool isolate_audio =
                line.find(R"("isolate_audio":true)") != std::string::npos;
            const bool audience_audio =
                line.find(R"("audience_audio":true)") != std::string::npos;
            if (is_valid_source_uuid(uuid)) {
                EngineAudio::instance().init(e2p, uuid, pid,
                                             isolate_audio, audience_audio);
            }

        } else if (command == IpcCommand::Subscribe) {
            std::string uuid = json_str(line, "source_uuid");
            uint32_t    pid  = json_uint(line, "participant_id");
            uint32_t    res  = json_uint(line, "resolution");
            if (res > 2) res = 1;
            const bool isolate_audio =
                line.find(R"("isolate_audio":true)") != std::string::npos;
            const bool audience_audio =
                line.find(R"("audience_audio":true)") != std::string::npos;
            // Opt-in, default off: a source that only wants video says so, and
            // the engine skips registering it as an audio target. Without it
            // every subscriber received mixed meeting audio — an SHM write plus
            // an {"cmd":"audio"} IPC line per audio buffer — even when it had no
            // use for a single sample. See ipc_subscribe_is_video_only().
            const bool video_only = ipc_subscribe_is_video_only(line);
            if (is_valid_source_uuid(uuid)) {
                const std::string mode = json_str(line, "mode");
                if (mode == "screenshare") {
                    share_engine.subscribe(uuid, e2p);
                } else {
                    video_engine.subscribe(pid, uuid, e2p, res);
                    if (video_only) {
                        // Drop any target a previous non-video-only subscribe
                        // of this uuid left behind, so switching a source to
                        // video-only actually stops the audio traffic.
                        EngineAudio::instance().remove(uuid);
                    } else {
                        EngineAudio::instance().init(e2p, uuid, pid,
                                                     isolate_audio,
                                                     audience_audio);
                    }
                }
            }

        } else if (command == IpcCommand::Unsubscribe) {
            std::string uuid = json_str(line, "source_uuid");
            if (is_valid_source_uuid(uuid)) {
                video_engine.unsubscribe(uuid);
                share_engine.unsubscribe(uuid);
                EngineAudio::instance().remove(uuid);
            }
        }
    }

    // Stop the heartbeat before tearing down the SDK / IPC.
    running.store(false, std::memory_order_release);
    if (heartbeat.joinable()) heartbeat.join();

    // Join the talkback driving thread BEFORE any SDK teardown call, not
    // just before CleanUPSDK() -- Leave() below also runs through
    // meeting_svc, and tick() calls through m_ctrl/m_svc (raw ZOOMSDK
    // pointers captured at probe() time) on every iteration. A tick() still
    // in flight when either of those tears down is a callback racing
    // teardown on invalidated SDK pointers -- the defect class this project
    // documents in CLAUDE.md's "Engine teardown" invariant. Signal stop
    // first so this does not wait out the full ~30s bound.
    talkback_stop.store(true, std::memory_order_release);
    if (talkback_thread.joinable()) talkback_thread.join();
    // F4 review-round fix: same reasoning as the Leave branch above -- quit
    // is another path that used to leave m_audio_open true and the ring
    // region mapped. Close it before the SDK teardown calls below run, same
    // ordering discipline as the driving-thread join right above (SDK
    // callbacks must never race teardown).
    talkback.close_audio();
    // Same reasoning as the Leave branch above: tear down the session's own
    // channel before the SDK teardown calls below run.
    talkback.session_stop();
    // Same reasoning as the Leave branch above: the nomination table only
    // holds bookkeeping for meeting-scoped state, so clear it before the SDK
    // teardown calls below run. No SDK call in nomination_reset() itself.
    talkback.nomination_reset();

    if (meeting_svc) meeting_svc->Leave(ZOOMSDK::LEAVE_MEETING);
    share_engine.detach();
    EngineAudio::instance().shutdown();
    ZOOMSDK::CleanUPSDK();
    ipc_teardown(p2e, e2p);
    return 0;
}
