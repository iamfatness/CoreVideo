#include "zoom-meeting.h"
#include "zoom-participants.h"
#include "zoom-auth.h"
#include <obs-module.h>
#include "../third_party/zoom-sdk/h/zoom_sdk.h"

#if defined(WIN32)
#include <windows.h>
static std::wstring to_zstr(const std::string &utf8)
{
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], len);
    if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
    return wide;
}
#else
static std::string to_zstr(const std::string &s) { return s; }
#endif

ZoomMeeting &ZoomMeeting::instance() { static ZoomMeeting inst; return inst; }

bool ZoomMeeting::join(const std::string &meeting_id, const std::string &passcode,
                       const std::string &display_name)
{
    if (ZoomAuth::instance().state() != ZoomAuthState::Authenticated) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Cannot join: not authenticated");
        return false;
    }
    if (meeting_id.empty()) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Meeting ID must not be empty");
        return false;
    }
    if (m_state == MeetingState::InMeeting || m_state == MeetingState::Joining) {
        blog(LOG_WARNING, "[obs-zoom-plugin] Already in/joining a meeting — leaving first");
        leave();
    }

    if (!m_meeting_service) {
        ZOOMSDK::SDKError err = ZOOMSDK::CreateMeetingService(&m_meeting_service);
        if (err != ZOOMSDK::SDKERR_SUCCESS || !m_meeting_service) {
            blog(LOG_ERROR, "[obs-zoom-plugin] CreateMeetingService failed: %d",
                 static_cast<int>(err));
            return false;
        }
        m_meeting_service->SetEvent(this);
    }

    const std::string name = display_name.empty() ? "OBS" : display_name;
    auto wide_name     = to_zstr(name);
    auto wide_passcode = to_zstr(passcode);

    ZOOMSDK::JoinParam join_param;
    join_param.userType = ZOOMSDK::SDK_UT_WITHOUT_LOGIN;

    ZOOMSDK::JoinParam4WithoutLogin &p = join_param.param.withoutloginuserJoin;
    p.meetingNumber             = std::stoull(meeting_id);
    p.userName                  = wide_name.c_str();
    p.psw                       = passcode.empty() ? nullptr : wide_passcode.c_str();
    p.isVideoOff                = true;   // silent observer — no camera
    p.isAudioOff                = true;   // silent observer — no microphone
    p.isMyVoiceInMix            = false;
    // Broadcast quality settings
    p.eAudioRawdataSamplingRate = ZOOMSDK::AudioRawdataSamplingRate_48K;
    p.eVideoRawdataColorspace   = ZOOMSDK::VideoRawdataColorspace_BT709_F;

    ZOOMSDK::SDKError err = m_meeting_service->Join(join_param);
    if (err != ZOOMSDK::SDKERR_SUCCESS) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Join failed: %d", static_cast<int>(err));
        return false;
    }

    m_state = MeetingState::Joining;
    if (m_state_cb) m_state_cb(m_state);
    blog(LOG_INFO, "[obs-zoom-plugin] Joining meeting %s", meeting_id.c_str());
    return true;
}

void ZoomMeeting::leave()
{
    if (m_state == MeetingState::Idle || m_state == MeetingState::Failed) return;

    if (m_meeting_service)
        m_meeting_service->Leave(ZOOMSDK::LEAVE_MEETING);

    // Service is destroyed in onMeetingStatusChanged when DISCONNECTING/ENDED arrives.
    // Do not call DestroyMeetingService here — we are potentially inside an SDK callback chain.
    m_state = MeetingState::Leaving;
    if (m_state_cb) m_state_cb(m_state);
    blog(LOG_INFO, "[obs-zoom-plugin] Leaving meeting");
}

// ── IMeetingServiceEvent ──────────────────────────────────────────────────────

void ZoomMeeting::onMeetingStatusChanged(ZOOMSDK::MeetingStatus status, int iResult)
{
    blog(LOG_INFO, "[obs-zoom-plugin] Meeting status: %d (result %d)",
         static_cast<int>(status), iResult);

    switch (status) {
    case ZOOMSDK::MEETING_STATUS_INMEETING:
        m_state = MeetingState::InMeeting;
        if (m_meeting_service) {
            auto *pc = m_meeting_service->GetMeetingParticipantsController();
            ZoomParticipants::instance().attach(pc);
        }
        break;

    case ZOOMSDK::MEETING_STATUS_DISCONNECTING:
    case ZOOMSDK::MEETING_STATUS_ENDED:
        m_state = MeetingState::Idle;
        ZoomParticipants::instance().detach();
        // Safe to destroy service here — status callbacks fire on the SDK thread
        // after the meeting has fully disconnected, not during an active call.
        if (m_meeting_service) {
            m_meeting_service->SetEvent(nullptr);
            ZOOMSDK::DestroyMeetingService(m_meeting_service);
            m_meeting_service = nullptr;
        }
        break;

    case ZOOMSDK::MEETING_STATUS_FAILED:
        m_state = MeetingState::Failed;
        blog(LOG_ERROR, "[obs-zoom-plugin] Meeting failed, code: %d", iResult);
        ZoomParticipants::instance().detach();
        if (m_meeting_service) {
            m_meeting_service->SetEvent(nullptr);
            ZOOMSDK::DestroyMeetingService(m_meeting_service);
            m_meeting_service = nullptr;
        }
        break;

    case ZOOMSDK::MEETING_STATUS_CONNECTING:
    case ZOOMSDK::MEETING_STATUS_WAITINGFORHOST:
    case ZOOMSDK::MEETING_STATUS_IN_WAITING_ROOM:
    case ZOOMSDK::MEETING_STATUS_RECONNECTING:
        m_state = MeetingState::Joining;
        break;

    default:
        break;
    }

    if (m_state_cb) m_state_cb(m_state);
}

void ZoomMeeting::onMeetingStatisticsWarningNotification(ZOOMSDK::StatisticsWarningType type)
{
    if (type == ZOOMSDK::Statistics_Warning_Network_Quality_Bad)
        blog(LOG_WARNING, "[obs-zoom-plugin] Network quality warning — frames may drop");
}

void ZoomMeeting::onMeetingParameterNotification(const ZOOMSDK::MeetingParameter *p)
{
    if (p)
        blog(LOG_INFO, "[obs-zoom-plugin] Meeting starting: number=%llu topic=%ls",
             static_cast<unsigned long long>(p->meeting_number),
             p->meeting_topic ? p->meeting_topic : L"");
}

void ZoomMeeting::onSuspendParticipantsActivities() {}

void ZoomMeeting::onAICompanionActiveChangeNotice(bool) {}

void ZoomMeeting::onMeetingTopicChanged(const zchar_t *) {}

void ZoomMeeting::onMeetingFullToWatchLiveStream(const zchar_t *)
{
    blog(LOG_WARNING, "[obs-zoom-plugin] Meeting is full");
}

void ZoomMeeting::onUserNetworkStatusChanged(ZOOMSDK::MeetingComponentType type,
                                              ZOOMSDK::ConnectionQuality level,
                                              unsigned int userId, bool uplink)
{
    if (level <= ZOOMSDK::Conn_Quality_Bad)
        blog(LOG_WARNING, "[obs-zoom-plugin] Poor network: component=%d user=%u uplink=%d",
             static_cast<int>(type), userId, uplink ? 1 : 0);
}

#if defined(WIN32)
void ZoomMeeting::onAppSignalPanelUpdated(ZOOMSDK::IMeetingAppSignalHandler *) {}
#endif
