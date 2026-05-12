#include "../../src/engine-ipc.h"
#include "engine-video.h"
#include "engine-audio.h"
// zoom_sdk.h provides InitSDK, CreateAuthService, CreateMeetingService — copy from SDK h/ folder
#include "../../third_party/zoom-sdk/h/zoom_sdk.h"
#include "../../third_party/zoom-sdk/h/auth_service_interface.h"
#include "../../third_party/zoom-sdk/h/meeting_service_interface.h"
#include <windows.h>
#include <string>
#include <atomic>

// ── Simple line I/O over named pipes ─────────────────────────────────────────

static std::string read_line(HANDLE pipe)
{
    std::string line; char ch; DWORD n;
    while (ReadFile(pipe, &ch, 1, &n, nullptr) && n == 1) {
        if (ch == '\n') break;
        line += ch;
    }
    return line;
}

static void write_line(HANDLE pipe, const std::string &msg)
{
    std::string out = msg + "\n"; DWORD written;
    WriteFile(pipe, out.c_str(), static_cast<DWORD>(out.size()), &written, nullptr);
}

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

class EngineAuthEvent : public ZOOM_SDK_NAMESPACE::IAuthServiceEvent {
public:
    explicit EngineAuthEvent(HANDLE e2p) : m_e2p(e2p) {}

    void onAuthenticationReturn(ZOOM_SDK_NAMESPACE::AuthResult ret) override {
        if (ret == ZOOM_SDK_NAMESPACE::AUTHRET_SUCCESS)
            write_line(m_e2p, R"({"cmd":"auth_ok"})");
        else
            write_line(m_e2p, R"({"cmd":"auth_fail","code":)" +
                       std::to_string(static_cast<int>(ret)) + "}");
    }
    void onLoginReturnWithReason(ZOOM_SDK_NAMESPACE::LOGINSTATUS,
                                 ZOOM_SDK_NAMESPACE::IAccountInfo *,
                                 ZOOM_SDK_NAMESPACE::LoginFailReason) override {}
    void onLogout() override {}
    void onZoomIdentityExpired() override {
        write_line(m_e2p, R"({"cmd":"error","msg":"identity_expired"})");
    }
    void onZoomAuthIdentityExpired() override {}
    void onNotificationServiceStatus(ZOOM_SDK_NAMESPACE::SDKNotificationServiceStatus,
                                     ZOOM_SDK_NAMESPACE::SDKNotificationServiceError) override {}
private:
    HANDLE m_e2p;
};

// ── Meeting event handler ─────────────────────────────────────────────────────

class EngineMeetingEvent : public ZOOM_SDK_NAMESPACE::IMeetingServiceEvent {
public:
    explicit EngineMeetingEvent(HANDLE e2p) : m_e2p(e2p) {}

    void onMeetingStatusChanged(ZOOM_SDK_NAMESPACE::MeetingStatus status, int iResult) override {
        switch (status) {
        case ZOOM_SDK_NAMESPACE::MEETING_STATUS_INMEETING:
            write_line(m_e2p, R"({"cmd":"joined"})");
            break;
        case ZOOM_SDK_NAMESPACE::MEETING_STATUS_DISCONNECTING:
        case ZOOM_SDK_NAMESPACE::MEETING_STATUS_ENDED:
            write_line(m_e2p, R"({"cmd":"left"})");
            break;
        case ZOOM_SDK_NAMESPACE::MEETING_STATUS_FAILED:
            write_line(m_e2p, R"({"cmd":"error","msg":"meeting_failed","code":)" +
                       std::to_string(iResult) + "}");
            break;
        default: break;
        }
    }
    void onMeetingStatisticsWarningNotification(ZOOM_SDK_NAMESPACE::StatisticsWarningType) override {}
    void onMeetingParameterNotification(const ZOOM_SDK_NAMESPACE::MeetingParameter *) override {}
    void onSuspendParticipantsActivities() override {}
    void onAICompanionActiveChangeNotice(bool) override {}
    void onMeetingTopicChanged(const zchar_t *) override {}
    void onMeetingFullToWatchLiveStream(const zchar_t *) override {}
    void onUserNetworkStatusChanged(ZOOM_SDK_NAMESPACE::MeetingComponentType,
                                    ZOOM_SDK_NAMESPACE::ConnectionQuality,
                                    unsigned int, bool) override {}
    void onAppSignalPanelUpdated(ZOOM_SDK_NAMESPACE::IMeetingAppSignalHandler *) override {}
private:
    HANDLE m_e2p;
};

// ── Entry point ───────────────────────────────────────────────────────────────

int main()
{
    // Create pipes first so the plugin can connect
    HANDLE p2e = CreateNamedPipeA(PIPE_P2E, PIPE_ACCESS_INBOUND,
                                  PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,
                                  1, 65536, 65536, 0, nullptr);
    HANDLE e2p = CreateNamedPipeA(PIPE_E2P, PIPE_ACCESS_OUTBOUND,
                                  PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,
                                  1, 65536, 65536, 0, nullptr);
    ConnectNamedPipe(p2e, nullptr);
    ConnectNamedPipe(e2p, nullptr);

    write_line(e2p, R"({"cmd":"ready"})");

    ZOOM_SDK_NAMESPACE::IAuthService    *auth_svc    = nullptr;
    ZOOM_SDK_NAMESPACE::IMeetingService *meeting_svc = nullptr;
    EngineAuthEvent    auth_event(e2p);
    EngineMeetingEvent meeting_event(e2p);
    EngineVideo        video_engine;

    std::atomic<bool> running{true};

    while (running) {
        std::string line = read_line(p2e);
        if (line.empty()) continue;

        if (line.find(IPC_CMD_QUIT) != std::string::npos) {
            running = false;

        } else if (line.find(IPC_CMD_INIT) != std::string::npos) {
            // {"cmd":"init","jwt":"<token>"}
            std::string jwt = json_str(line, "jwt");

            ZOOM_SDK_NAMESPACE::InitParam init_param;
            init_param.strWebDomain      = L"https://zoom.us";
            init_param.enableGenerateDump = true;
            ZOOM_SDK_NAMESPACE::InitSDK(init_param);

            ZOOM_SDK_NAMESPACE::CreateAuthService(&auth_svc);
            if (auth_svc) {
                auth_svc->SetEvent(&auth_event);
                std::wstring wjwt(jwt.begin(), jwt.end());
                ZOOM_SDK_NAMESPACE::AuthContext ctx;
                ctx.jwt_token = wjwt.c_str();
                auth_svc->SDKAuth(ctx);
            } else {
                write_line(e2p, R"({"cmd":"auth_fail","code":-1})");
            }

        } else if (line.find(IPC_CMD_JOIN) != std::string::npos) {
            // {"cmd":"join","meeting_id":"<id>","passcode":"<pw>","display_name":"OBS"}
            std::string meeting_id   = json_str(line, "meeting_id");
            std::string passcode     = json_str(line, "passcode");
            std::string display_name = json_str(line, "display_name");
            if (display_name.empty()) display_name = "OBS";

            if (!meeting_svc) {
                ZOOM_SDK_NAMESPACE::CreateMeetingService(&meeting_svc);
                if (meeting_svc) meeting_svc->SetEvent(&meeting_event);
            }
            if (meeting_svc && !meeting_id.empty()) {
                std::wstring wname(display_name.begin(), display_name.end());
                std::wstring wpsw(passcode.begin(), passcode.end());

                ZOOM_SDK_NAMESPACE::JoinParam jp;
                jp.userType = ZOOM_SDK_NAMESPACE::SDK_UT_WITHOUT_LOGIN;
                ZOOM_SDK_NAMESPACE::JoinParam4WithoutLogin &p = jp.param.withoutloginuserJoin;
                p.meetingNumber             = std::stoull(meeting_id);
                p.userName                  = wname.c_str();
                p.psw                       = passcode.empty() ? nullptr : wpsw.c_str();
                p.isVideoOff                = true;
                p.isAudioOff                = true;
                p.isMyVoiceInMix            = false;
                p.eAudioRawdataSamplingRate = ZOOM_SDK_NAMESPACE::AudioRawdataSamplingRate_48K;
                p.eVideoRawdataColorspace   = ZOOM_SDK_NAMESPACE::VideoRawdataColorspace_BT709_F;
                meeting_svc->Join(jp);
            }

        } else if (line.find(IPC_CMD_LEAVE) != std::string::npos) {
            if (meeting_svc)
                meeting_svc->Leave(ZOOM_SDK_NAMESPACE::LEAVE_MEETING);

        } else if (line.find(IPC_CMD_SUBSCRIBE) != std::string::npos) {
            // {"cmd":"subscribe","source_uuid":"<uuid>","participant_id":<id>}
            std::string uuid = json_str(line, "source_uuid");
            uint32_t    pid  = json_uint(line, "participant_id");
            if (!uuid.empty())
                video_engine.subscribe(pid, uuid, e2p);

            EngineAudio::instance().init(e2p, uuid);

        } else if (line.find(IPC_CMD_UNSUBSCRIBE) != std::string::npos) {
            // {"cmd":"unsubscribe","source_uuid":"<uuid>"}
            std::string uuid = json_str(line, "source_uuid");
            if (!uuid.empty())
                video_engine.unsubscribe(uuid);
        }
    }

    if (meeting_svc) meeting_svc->Leave(ZOOM_SDK_NAMESPACE::LEAVE_MEETING);
    ZOOM_SDK_NAMESPACE::CleanUPSDK();
    CloseHandle(p2e);
    CloseHandle(e2p);
    return 0;
}
