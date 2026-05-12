#include "zoom-meeting.h"
#include "zoom-auth.h"
#include <obs-module.h>
ZoomMeeting &ZoomMeeting::instance() { static ZoomMeeting inst; return inst; }
bool ZoomMeeting::join(const std::string &meeting_id, const std::string &passcode)
{
    if (ZoomAuth::instance().state() != ZoomAuthState::Authenticated) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Cannot join: not authenticated"); return false;
    }
    if (m_state == MeetingState::InMeeting) {
        blog(LOG_WARNING, "[obs-zoom-plugin] Already in a meeting — leaving first"); leave();
    }
    m_state = MeetingState::Joining;
    if (m_state_cb) m_state_cb(m_state);
    blog(LOG_INFO, "[obs-zoom-plugin] Joining meeting %s", meeting_id.c_str());
    // ZOOM_SDK_NAMESPACE::IMeetingService *svc = nullptr; ZOOM_SDK_NAMESPACE::CreateMeetingService(&svc);
    // ZOOM_SDK_NAMESPACE::JoinParam join; join.userType = SDK_UT_NORMALUSER;
    // auto &p = join.param.normaluserJoinParam; p.meetingNumber = std::stoull(meeting_id); svc->Join(join);
    return true;
}
void ZoomMeeting::leave()
{
    if (m_state != MeetingState::InMeeting && m_state != MeetingState::Joining) return;
    m_state = MeetingState::Leaving;
    if (m_state_cb) m_state_cb(m_state);
    blog(LOG_INFO, "[obs-zoom-plugin] Leaving meeting");
    // ZOOM_SDK_NAMESPACE::IMeetingService *svc = nullptr; ZOOM_SDK_NAMESPACE::CreateMeetingService(&svc);
    // svc->Leave(ZOOM_SDK_NAMESPACE::LEAVE_MEETING);
}
