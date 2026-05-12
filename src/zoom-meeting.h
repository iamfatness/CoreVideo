#pragma once
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <meeting_service_interface.h>

enum class MeetingState { Idle, Joining, InMeeting, Leaving, Failed };

static constexpr int kMaxReconnectAttempts = 5;
static constexpr int kReconnectBaseMs      = 2000; // 2 s, 4 s, 8 s, 16 s, 32 s

class ZoomMeeting : public ZOOMSDK::IMeetingServiceEvent {
public:
    static ZoomMeeting &instance();

    bool join(const std::string &meeting_id, const std::string &passcode = "",
              const std::string &display_name = "OBS");
    void leave();
    MeetingState state() const {
        return m_state.load(std::memory_order_acquire);
    }

    ZOOMSDK::IMeetingService *service() const { return m_meeting_service; }

    using StateCallback = std::function<void(MeetingState)>;
    // Register a state-change callback keyed by an opaque pointer.
    // Callbacks always fire on the OBS main thread — safe to call OBS/SDK APIs.
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

    // m_state is written on the SDK thread and read from any thread.
    std::atomic<MeetingState> m_state{MeetingState::Idle};
    // m_state_cbs is only accessed on the OBS main thread.
    std::unordered_map<void *, StateCallback> m_state_cbs;
    ZOOMSDK::IMeetingService *m_meeting_service = nullptr;

    // reconnect state
    std::string m_last_meeting_id;
    std::string m_last_passcode;
    std::string m_last_display_name;
    int         m_reconnect_attempts = 0;
    bool        m_user_leaving       = false;

    // Kept alive across the async Join() call (JoinParam holds raw pointers).
#if defined(WIN32)
    std::wstring m_wide_name;
    std::wstring m_wide_passcode;
#else
    std::string  m_wide_name;
    std::string  m_wide_passcode;
#endif
};
