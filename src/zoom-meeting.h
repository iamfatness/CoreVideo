#pragma once
#include <string>
#include <functional>
#include <thread>
#include <unordered_map>
#include "../third_party/zoom-sdk/h/meeting_service_interface.h"

enum class MeetingState { Idle, Joining, InMeeting, Leaving, Failed };

static constexpr int kMaxReconnectAttempts = 5;
static constexpr int kReconnectBaseMs      = 2000; // 2 s, 4 s, 8 s, 16 s, 32 s

class ZoomMeeting : public ZOOMSDK::IMeetingServiceEvent {
public:
    static ZoomMeeting &instance();

    bool join(const std::string &meeting_id, const std::string &passcode = "",
              const std::string &display_name = "OBS");
    void leave();
    MeetingState state() const { return m_state; }

    ZOOMSDK::IMeetingService *service() const { return m_meeting_service; }

    using StateCallback = std::function<void(MeetingState)>;
    // Register a state-change callback keyed by an opaque pointer (typically the
    // obs_source_t or 'this' of the caller).  Pass cb = nullptr to remove.
    void on_state_change(void *key, StateCallback cb);
    void remove_state_change(void *key) { on_state_change(key, nullptr); }

    // IMeetingServiceEvent
    void onMeetingStatusChanged(ZOOMSDK::MeetingStatus status, int iResult) override;
    void onMeetingStatisticsWarningNotification(ZOOMSDK::StatisticsWarningType type) override;
    void onMeetingParameterNotification(const ZOOMSDK::MeetingParameter *meeting_param) override;
    void onSuspendParticipantsActivities() override;
    void onAICompanionActiveChangeNotice(bool bActive) override;
    void onMeetingTopicChanged(const zchar_t *sTopic) override;
    void onMeetingFullToWatchLiveStream(const zchar_t *sLiveStreamUrl) override;
    void onUserNetworkStatusChanged(ZOOMSDK::MeetingComponentType type,
                                    ZOOMSDK::ConnectionQuality level,
                                    unsigned int userId, bool uplink) override;
#if defined(WIN32)
    void onAppSignalPanelUpdated(ZOOMSDK::IMeetingAppSignalHandler *pHandler) override;
#endif

private:
    ZoomMeeting() = default;
    bool join_impl(const std::string &meeting_id, const std::string &passcode,
                   const std::string &display_name);
    void schedule_reconnect();
    void do_reconnect();

    void fire_state_cb();

    MeetingState              m_state           = MeetingState::Idle;
    std::unordered_map<void *, StateCallback> m_state_cbs;
    ZOOMSDK::IMeetingService *m_meeting_service = nullptr;

    // reconnect state
    std::string m_last_meeting_id;
    std::string m_last_passcode;
    std::string m_last_display_name;
    int         m_reconnect_attempts = 0;
    bool        m_user_leaving       = false;
};
