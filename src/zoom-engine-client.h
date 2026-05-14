#pragma once

#include "engine-ipc.h"
#include "zoom-types.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class ZoomEngineClient {
public:
    struct SourceCallbacks {
        std::function<void(uint32_t width, uint32_t height)> on_frame;
        std::function<void(uint32_t byte_len)> on_audio;
    };

    static ZoomEngineClient &instance();

    bool start(const std::string &jwt_token);
    void stop();
    // Same as stop() but does not set the user-leaving flag and does not
    // cancel a pending recovery. Used by ZoomReconnectManager between retries.
    void stop_for_reconnect();

    bool join(const std::string &meeting_id, const std::string &passcode,
              const std::string &display_name);
    void leave();

    // Used by ZoomReconnectManager to drive state transitions.
    void set_state(MeetingState s) { m_state.store(s, std::memory_order_release); }
    void subscribe(const std::string &source_uuid, uint32_t participant_id);
    void unsubscribe(const std::string &source_uuid);

    MeetingState state() const { return m_state.load(std::memory_order_acquire); }
    uint32_t active_speaker_id() const;
    std::vector<ParticipantInfo> roster() const;

    void register_source(const std::string &source_uuid, SourceCallbacks callbacks);
    void unregister_source(const std::string &source_uuid);
    using RosterCallback = std::function<void()>;
    void add_roster_callback(void *key, RosterCallback cb);
    void remove_roster_callback(void *key);

private:
    ZoomEngineClient() = default;
    ~ZoomEngineClient();

    bool launch_engine();
    bool connect_ipc();
    void disconnect_ipc();
    void reader_loop();
    void monitor_loop();
    void handle_event(const std::string &line);
    void write_json(const std::string &json);

    mutable std::mutex m_mtx;
    IpcFd m_p2e = kIpcInvalidFd;
    IpcFd m_e2p = kIpcInvalidFd;
    std::thread m_reader;
    std::thread m_monitor;
    std::atomic<bool> m_running{false};
    std::atomic<MeetingState> m_state{MeetingState::Idle};
    uint32_t m_active_speaker_id = 0;
    std::vector<ParticipantInfo> m_roster;
    std::unordered_map<std::string, SourceCallbacks> m_sources;
    std::unordered_map<void *, RosterCallback> m_roster_callbacks;
    // Tracks whether the user deliberately requested a leave/stop (suppresses recovery).
    std::atomic<bool> m_user_leaving{false};
    std::string m_last_jwt; // stored so reconnect manager can access it

#if defined(WIN32)
    void *m_process = nullptr;
#else
    int m_pid = -1;
#endif
};
