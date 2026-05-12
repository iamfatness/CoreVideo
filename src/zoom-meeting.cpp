#include "zoom-meeting.h"
#include "zoom-auth.h"
#include <obs-module.h>
// zoom_sdk.h provides CreateMeetingService, DestroyMeetingService — copy from SDK h/ folder
#include "zoom_sdk.h"
#include <string>
#include <cstdlib>

#if defined(WIN32)
static std::wstring to_zstr(const std::string &s)
{
    return std::wstring(s.begin(), s.end());
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
        ZOOM_SDK_NAMESPACE::SDKError err =
            ZOOM_SDK_NAMESPACE::CreateMeetingService(&m_meeting_service);
        if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS || !m_meeting_service) {
            blog(LOG_ERROR, "[obs-zoom-plugin] CreateMeetingService failed: %d",
                 static_cast<int>(err));
            return false;
        }
        m_meeting_service->SetEvent(this);
    }

    auto wide_name     = to_zstr(display_name.empty() ? std::string("OBS") : display_name);
    auto wide_passcode = to_zstr(passcode);

    ZOOM_SDK_NAMESPACE::JoinParam join_param;
    join_param.userType = ZOOM_SDK_NAMESPACE::SDK_UT_WITHOUT_LOGIN;

    ZOOM_SDK_NAMESPACE::JoinParam4WithoutLogin &p = join_param.param.withoutloginuserJoin;
    p.meetingNumber            = std::stoull(meeting_id);
    p.userName                 = wide_name.c_str();
    p.psw                      = passcode.empty() ? nullptr : wide_passcode.c_str();
    p.isVideoOff               = true;   // we are a silent observer
    p.isAudioOff               = true;
    p.isMyVoiceInMix           = false;
    // Broadcast quality: 48 kHz audio, BT.709 full-range video
    p.eAudioRawdataSamplingRate = ZOOM_SDK_NAMESPACE::AudioRawdataSamplingRate_48K;
    p.eVideoRawdataColorspace   = ZOOM_SDK_NAMESPACE::VideoRawdataColorspace_BT709_F;

    ZOOM_SDK_NAMESPACE::SDKError err = m_meeting_service->Join(join_param);
    if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
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
    if (m_meeting_service) {
        m_meeting_service->Leave(ZOOM_SDK_NAMESPACE::LEAVE_MEETING);
    }
    m_state = MeetingState::Leaving;
    if (m_state_cb) m_state_cb(m_state);
    blog(LOG_INFO, "[obs-zoom-plugin] Leaving meeting");
}

// ── IMeetingServiceEvent callbacks ───────────────────────────────────────────

void ZoomMeeting::onMeetingStatusChanged(ZOOM_SDK_NAMESPACE::MeetingStatus status, int iResult)
{
    blog(LOG_INFO, "[obs-zoom-plugin] Meeting status changed: %d (result %d)",
         static_cast<int>(status), iResult);

    switch (status) {
    case ZOOM_SDK_NAMESPACE::MEETING_STATUS_INMEETING:
        m_state = MeetingState::InMeeting;
        break;
    case ZOOM_SDK_NAMESPACE::MEETING_STATUS_DISCONNECTING:
    case ZOOM_SDK_NAMESPACE::MEETING_STATUS_ENDED:
        m_state = MeetingState::Idle;
        break;
    case ZOOM_SDK_NAMESPACE::MEETING_STATUS_FAILED:
        m_state = MeetingState::Failed;
        blog(LOG_ERROR, "[obs-zoom-plugin] Meeting failed, code: %d", iResult);
        break;
    case ZOOM_SDK_NAMESPACE::MEETING_STATUS_CONNECTING:
    case ZOOM_SDK_NAMESPACE::MEETING_STATUS_WAITINGFORHOST:
    case ZOOM_SDK_NAMESPACE::MEETING_STATUS_IN_WAITING_ROOM:
        m_state = MeetingState::Joining;
        break;
    default:
        break;
    }

    if (m_state_cb) m_state_cb(m_state);
}

void ZoomMeeting::onMeetingStatisticsWarningNotification(
    ZOOM_SDK_NAMESPACE::StatisticsWarningType type)
{
    if (type == ZOOM_SDK_NAMESPACE::Statistics_Warning_Network_Quality_Bad)
        blog(LOG_WARNING, "[obs-zoom-plugin] Network quality warning");
}

void ZoomMeeting::onMeetingParameterNotification(
    const ZOOM_SDK_NAMESPACE::MeetingParameter *meeting_param)
{
    if (meeting_param)
        blog(LOG_INFO, "[obs-zoom-plugin] Meeting starting, number: %llu",
             static_cast<unsigned long long>(meeting_param->meeting_number));
}

void ZoomMeeting::onSuspendParticipantsActivities() {}

void ZoomMeeting::onAICompanionActiveChangeNotice(bool) {}

void ZoomMeeting::onMeetingTopicChanged(const zchar_t *) {}

void ZoomMeeting::onMeetingFullToWatchLiveStream(const zchar_t *sLiveStreamUrl)
{
    blog(LOG_WARNING, "[obs-zoom-plugin] Meeting full — live stream available");
    (void)sLiveStreamUrl;
}

void ZoomMeeting::onUserNetworkStatusChanged(ZOOM_SDK_NAMESPACE::MeetingComponentType type,
                                              ZOOM_SDK_NAMESPACE::ConnectionQuality level,
                                              unsigned int userId, bool uplink)
{
    if (level <= ZOOM_SDK_NAMESPACE::Conn_Quality_Bad)
        blog(LOG_WARNING, "[obs-zoom-plugin] Poor network: component=%d user=%u uplink=%d",
             static_cast<int>(type), userId, uplink ? 1 : 0);
}

#if defined(WIN32)
void ZoomMeeting::onAppSignalPanelUpdated(ZOOM_SDK_NAMESPACE::IMeetingAppSignalHandler *) {}
#endif
