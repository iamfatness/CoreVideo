#pragma once
#include <string>
#include <functional>
#include "meeting_service_interface.h"

enum class MeetingState { Idle, Joining, InMeeting, Leaving, Failed };

class ZoomMeeting : public ZOOM_SDK_NAMESPACE::IMeetingServiceEvent {
public:
    static ZoomMeeting &instance();

    bool join(const std::string &meeting_id, const std::string &passcode = "",
              const std::string &display_name = "OBS");
    void leave();
    MeetingState state() const { return m_state; }

    ZOOM_SDK_NAMESPACE::IMeetingService *service() const { return m_meeting_service; }

    using VideoFrameCallback = std::function<void(const uint8_t *, uint32_t, uint32_t)>;
    using AudioCallback      = std::function<void(const float *, size_t, uint32_t)>;
    using StateCallback      = std::function<void(MeetingState)>;
    void on_video_frame(VideoFrameCallback cb) { m_video_cb = cb; }
    void on_audio(AudioCallback cb)            { m_audio_cb = cb; }
    void on_state_change(StateCallback cb)     { m_state_cb = cb; }

    // IMeetingServiceEvent
    void onMeetingStatusChanged(ZOOM_SDK_NAMESPACE::MeetingStatus status, int iResult) override;
    void onMeetingStatisticsWarningNotification(ZOOM_SDK_NAMESPACE::StatisticsWarningType type) override;
    void onMeetingParameterNotification(const ZOOM_SDK_NAMESPACE::MeetingParameter *meeting_param) override;
    void onSuspendParticipantsActivities() override;
    void onAICompanionActiveChangeNotice(bool bActive) override;
    void onMeetingTopicChanged(const zchar_t *sTopic) override;
    void onMeetingFullToWatchLiveStream(const zchar_t *sLiveStreamUrl) override;
    void onUserNetworkStatusChanged(ZOOM_SDK_NAMESPACE::MeetingComponentType type,
                                    ZOOM_SDK_NAMESPACE::ConnectionQuality level,
                                    unsigned int userId, bool uplink) override;
#if defined(WIN32)
    void onAppSignalPanelUpdated(ZOOM_SDK_NAMESPACE::IMeetingAppSignalHandler *pHandler) override;
#endif

private:
    ZoomMeeting() = default;
    MeetingState       m_state = MeetingState::Idle;
    VideoFrameCallback m_video_cb;
    AudioCallback      m_audio_cb;
    StateCallback      m_state_cb;
    ZOOM_SDK_NAMESPACE::IMeetingService *m_meeting_service = nullptr;
};
