#include "../../src/engine-ipc.h"
#include "engine-video.h"
#include "engine-audio.h"
#include <zoom_sdk.h>
#include <auth_service_interface.h>
#include <meeting_service_interface.h>
#include <string>
#include <atomic>

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
    auto end = json.find('"', pos);
    return end == std::string::npos ? std::string{} : json.substr(pos, end - pos);
}

static uint32_t json_uint(const std::string &json, const std::string &key)
{
    const std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos += needle.size();
    return static_cast<uint32_t>(std::stoul(json.substr(pos)));
}

// ── Auth event handler ────────────────────────────────────────────────────────

class EngineAuthEvent : public ZOOMSDK::IAuthServiceEvent {
public:
    explicit EngineAuthEvent(IpcFd e2p) : m_e2p(e2p) {}

    void onAuthenticationReturn(ZOOMSDK::AuthResult ret) override {
        if (ret == ZOOMSDK::AUTHRET_SUCCESS)
            ipc_write_line(m_e2p, R"({"cmd":"auth_ok"})");
        else
            ipc_write_line(m_e2p, R"({"cmd":"auth_fail","code":)" +
                           std::to_string(static_cast<int>(ret)) + "}");
    }
    void onLoginReturnWithReason(ZOOMSDK::LOGINSTATUS,
                                 ZOOMSDK::IAccountInfo *,
                                 ZOOMSDK::LoginFailReason) override {}
    void onLogout() override {}
    void onZoomIdentityExpired() override {
        ipc_write_line(m_e2p, R"({"cmd":"error","msg":"identity_expired"})");
    }
    void onZoomAuthIdentityExpired() override {}
    void onNotificationServiceStatus(ZOOMSDK::SDKNotificationServiceStatus,
                                     ZOOMSDK::SDKNotificationServiceError) override {}
private:
    IpcFd m_e2p;
};

// ── Meeting event handler ─────────────────────────────────────────────────────

class EngineMeetingEvent : public ZOOMSDK::IMeetingServiceEvent {
public:
    explicit EngineMeetingEvent(IpcFd e2p) : m_e2p(e2p) {}

    void onMeetingStatusChanged(ZOOMSDK::MeetingStatus status, int iResult) override {
        switch (status) {
        case ZOOMSDK::MEETING_STATUS_INMEETING:
            ipc_write_line(m_e2p, R"({"cmd":"joined"})");
            break;
        case ZOOMSDK::MEETING_STATUS_DISCONNECTING:
        case ZOOMSDK::MEETING_STATUS_ENDED:
            ipc_write_line(m_e2p, R"({"cmd":"left"})");
            break;
        case ZOOMSDK::MEETING_STATUS_FAILED:
            ipc_write_line(m_e2p, R"({"cmd":"error","msg":"meeting_failed","code":)" +
                           std::to_string(iResult) + "}");
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
private:
    IpcFd m_e2p;
};

// ── Entry point ───────────────────────────────────────────────────────────────

int main()
{
    IpcFd p2e = kIpcInvalidFd;
    IpcFd e2p = kIpcInvalidFd;
    if (!ipc_setup(p2e, e2p)) return 1;

    ipc_write_line(e2p, R"({"cmd":"ready"})");

    ZOOMSDK::IAuthService    *auth_svc    = nullptr;
    ZOOMSDK::IMeetingService *meeting_svc = nullptr;
    EngineAuthEvent    auth_event(e2p);
    EngineMeetingEvent meeting_event(e2p);
    EngineVideo        video_engine;

    // Persistent wide-string storage for async SDK calls (JoinParam / AuthContext
    // hold raw pointers — these must outlive the Join/SDKAuth call).
#if defined(WIN32)
    std::wstring g_wide_jwt;
    std::wstring g_wide_name;
    std::wstring g_wide_psw;
#else
    std::string g_wide_jwt;
    std::string g_wide_name;
    std::string g_wide_psw;
#endif

    std::atomic<bool> running{true};
    std::string line;

    while (running) {
        if (!ipc_read_line(p2e, line)) break; // EOF or connection closed
        if (line.empty()) continue;

        if (line.find(IPC_CMD_QUIT) != std::string::npos) {
            running = false;

        } else if (line.find(IPC_CMD_INIT) != std::string::npos) {
            std::string jwt = json_str(line, "jwt");

            ZOOMSDK::InitParam init_param;
#if defined(WIN32)
            init_param.strWebDomain = L"https://zoom.us";
#else
            init_param.strWebDomain = "https://zoom.us";
#endif
            init_param.enableGenerateDump = true;
            init_param.obConfigOpts.optionalFeatures = ENABLE_CUSTOMIZED_UI_FLAG;
            init_param.rawdataOpts.videoRawdataMemoryMode = ZOOMSDK::ZoomSDKRawDataMemoryModeHeap;
            init_param.rawdataOpts.audioRawdataMemoryMode = ZOOMSDK::ZoomSDKRawDataMemoryModeHeap;
            ZOOMSDK::InitSDK(init_param);

            ZOOMSDK::CreateAuthService(&auth_svc);
            if (auth_svc) {
                auth_svc->SetEvent(&auth_event);
                g_wide_jwt = to_zstr(jwt); // persists for async SDKAuth call
                ZOOMSDK::AuthContext ctx;
                ctx.jwt_token = g_wide_jwt.c_str();
                auth_svc->SDKAuth(ctx);
            } else {
                ipc_write_line(e2p, R"({"cmd":"auth_fail","code":-1})");
            }

        } else if (line.find(IPC_CMD_JOIN) != std::string::npos) {
            std::string meeting_id   = json_str(line, "meeting_id");
            std::string passcode     = json_str(line, "passcode");
            std::string display_name = json_str(line, "display_name");
            if (display_name.empty()) display_name = "OBS";

            if (!meeting_svc) {
                ZOOMSDK::CreateMeetingService(&meeting_svc);
                if (meeting_svc) meeting_svc->SetEvent(&meeting_event);
            }
            if (meeting_svc && !meeting_id.empty()) {
                // Store as persistent variables so JoinParam raw pointers
                // remain valid for the duration of the async Join() call.
                g_wide_name = to_zstr(display_name);
                g_wide_psw  = to_zstr(passcode);

                ZOOMSDK::JoinParam jp;
                jp.userType = ZOOMSDK::SDK_UT_WITHOUT_LOGIN;
                ZOOMSDK::JoinParam4WithoutLogin &p = jp.param.withoutloginuserJoin;
                p.meetingNumber             = std::stoull(meeting_id);
                p.userName                  = g_wide_name.c_str();
                p.psw                       = passcode.empty() ? nullptr : g_wide_psw.c_str();
                p.isVideoOff                = true;
                p.isAudioOff                = true;
                p.isMyVoiceInMix            = false;
                p.eAudioRawdataSamplingRate = ZOOMSDK::AudioRawdataSamplingRate_48K;
                p.eVideoRawdataColorspace   = ZOOMSDK::VideoRawdataColorspace_BT709_F;
                meeting_svc->Join(jp);
            }

        } else if (line.find(IPC_CMD_LEAVE) != std::string::npos) {
            if (meeting_svc)
                meeting_svc->Leave(ZOOMSDK::LEAVE_MEETING);

        } else if (line.find(IPC_CMD_SUBSCRIBE) != std::string::npos) {
            std::string uuid = json_str(line, "source_uuid");
            uint32_t    pid  = json_uint(line, "participant_id");
            if (!uuid.empty()) {
                video_engine.subscribe(pid, uuid, e2p);
                EngineAudio::instance().init(e2p, uuid);
            }

        } else if (line.find(IPC_CMD_UNSUBSCRIBE) != std::string::npos) {
            std::string uuid = json_str(line, "source_uuid");
            if (!uuid.empty())
                video_engine.unsubscribe(uuid);
        }
    }

    if (meeting_svc) meeting_svc->Leave(ZOOMSDK::LEAVE_MEETING);
    EngineAudio::instance().shutdown();
    ZOOMSDK::CleanUPSDK();
    ipc_teardown(p2e, e2p);
    return 0;
}
