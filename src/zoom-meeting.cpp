#include "zoom-meeting.h"
#include "zoom-participants.h"
#include "zoom-share-delegate.h"
#include "zoom-audio-router.h"
#include "zoom-auth.h"
#include <obs-module.h>
#include <chrono>
#include <zoom_sdk.h>

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

// ── Public API ────────────────────────────────────────────────────────────────

bool ZoomMeeting::join(const std::string &meeting_id, const std::string &passcode,
                       const std::string &display_name)
{
    m_last_meeting_id    = meeting_id;
    m_last_passcode      = passcode;
    m_last_display_name  = display_name;
    m_reconnect_attempts = 0;
    m_user_leaving       = false;
    return join_impl(meeting_id, passcode, display_name);
}

void ZoomMeeting::leave()
{
    m_user_leaving = true;
    if (m_state == MeetingState::Idle || m_state == MeetingState::Failed) return;

    if (m_meeting_service)
        m_meeting_service->Leave(ZOOMSDK::LEAVE_MEETING);

    // Service is destroyed in onMeetingStatusChanged when DISCONNECTING/ENDED arrives.
    m_state = MeetingState::Leaving;
    fire_state_cb();
    blog(LOG_INFO, "[obs-zoom-plugin] Leaving meeting");
}

// ── Private helpers ───────────────────────────────────────────────────────────

bool ZoomMeeting::join_impl(const std::string &meeting_id, const std::string &passcode,
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
        // Don't call leave() here — that sets m_user_leaving; instead do a raw leave
        if (m_meeting_service) m_meeting_service->Leave(ZOOMSDK::LEAVE_MEETING);
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
    p.isVideoOff                = true;
    p.isAudioOff                = true;
    p.isMyVoiceInMix            = false;
    p.eAudioRawdataSamplingRate = ZOOMSDK::AudioRawdataSamplingRate_48K;
    p.eVideoRawdataColorspace   = ZOOMSDK::VideoRawdataColorspace_BT709_F;

    ZOOMSDK::SDKError err = m_meeting_service->Join(join_param);
    if (err != ZOOMSDK::SDKERR_SUCCESS) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Join failed: %d", static_cast<int>(err));
        return false;
    }

    m_state = MeetingState::Joining;
    fire_state_cb();
    blog(LOG_INFO, "[obs-zoom-plugin] Joining meeting %s (attempt %d/%d)",
         meeting_id.c_str(), m_reconnect_attempts, kMaxReconnectAttempts);
    return true;
}

void ZoomMeeting::do_reconnect()
{
    if (m_user_leaving || m_last_meeting_id.empty()) return;
    join_impl(m_last_meeting_id, m_last_passcode, m_last_display_name);
}

void ZoomMeeting::on_state_change(void *key, StateCallback cb)
{
    if (cb)
        m_state_cbs[key] = std::move(cb);
    else
        m_state_cbs.erase(key);
}

void ZoomMeeting::fire_state_cb()
{
    for (auto &[key, cb] : m_state_cbs)
        if (cb) cb(m_state);
}

void ZoomMeeting::schedule_reconnect()
{
    const int attempt  = m_reconnect_attempts;
    const int delay_ms = kReconnectBaseMs << std::min(attempt - 1, 4); // 2s→4s→8s→16s→32s

    blog(LOG_INFO, "[obs-zoom-plugin] Reconnect attempt %d/%d in %d ms",
         attempt, kMaxReconnectAttempts, delay_ms);

    struct Task { ZoomMeeting *self; };
    auto *task = new Task{this};
    std::thread([task, delay_ms]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        obs_queue_task(OBS_TASK_UI, [](void *p) {
            auto *t = static_cast<Task *>(p);
            t->self->do_reconnect();
            delete t;
        }, task, false);
    }).detach();
}

// ── IMeetingServiceEvent ──────────────────────────────────────────────────────

void ZoomMeeting::onMeetingStatusChanged(ZOOMSDK::MeetingStatus status, int iResult)
{
    blog(LOG_INFO, "[obs-zoom-plugin] Meeting status: %d (result %d)",
         static_cast<int>(status), iResult);

    switch (status) {
    case ZOOMSDK::MEETING_STATUS_INMEETING:
        m_state = MeetingState::InMeeting;
        m_reconnect_attempts = 0;
        ZoomAudioRouter::instance().subscribe();
        if (m_meeting_service) {
            ZoomParticipants::instance().attach(
                m_meeting_service->GetMeetingParticipantsController(),
                m_meeting_service->GetMeetingAudioController());
            ZoomShareDelegate::instance().attach(
                m_meeting_service->GetMeetingShareController());
        }
        break;

    case ZOOMSDK::MEETING_STATUS_RECONNECTING:
        // SDK is attempting its own reconnect — stay in Joining state.
        // Do NOT detach delegates so subscriptions survive the brief gap.
        m_state = MeetingState::Joining;
        blog(LOG_INFO, "[obs-zoom-plugin] SDK reconnecting — keeping delegates alive");
        break;

    case ZOOMSDK::MEETING_STATUS_DISCONNECTING:
    case ZOOMSDK::MEETING_STATUS_ENDED:
        m_state = MeetingState::Idle;
        ZoomAudioRouter::instance().unsubscribe();
        ZoomParticipants::instance().detach();
        ZoomShareDelegate::instance().detach();
        if (m_meeting_service) {
            m_meeting_service->SetEvent(nullptr);
            ZOOMSDK::DestroyMeetingService(m_meeting_service);
            m_meeting_service = nullptr;
        }
        break;

    case ZOOMSDK::MEETING_STATUS_FAILED:
        m_state = MeetingState::Failed;
        blog(LOG_ERROR, "[obs-zoom-plugin] Meeting failed, code: %d", iResult);
        ZoomAudioRouter::instance().unsubscribe();
        ZoomParticipants::instance().detach();
        ZoomShareDelegate::instance().detach();
        if (m_meeting_service) {
            m_meeting_service->SetEvent(nullptr);
            ZOOMSDK::DestroyMeetingService(m_meeting_service);
            m_meeting_service = nullptr;
        }
        if (!m_user_leaving && !m_last_meeting_id.empty() &&
            m_reconnect_attempts < kMaxReconnectAttempts) {
            ++m_reconnect_attempts;
            schedule_reconnect();
        } else if (m_reconnect_attempts >= kMaxReconnectAttempts) {
            blog(LOG_WARNING, "[obs-zoom-plugin] Giving up after %d reconnect attempts",
                 m_reconnect_attempts);
            m_reconnect_attempts = 0;
        }
        break;

    case ZOOMSDK::MEETING_STATUS_CONNECTING:
    case ZOOMSDK::MEETING_STATUS_WAITINGFORHOST:
    case ZOOMSDK::MEETING_STATUS_IN_WAITING_ROOM:
        m_state = MeetingState::Joining;
        break;

    default:
        break;
    }

    fire_state_cb();
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
