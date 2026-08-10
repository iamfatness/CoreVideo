#pragma once

#include "engine-ipc.h"
#include "zoom-types.h"
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Verbose logging gate. Per-frame / per-roster-tick telemetry (frame
// received, audio target churn, resolution noops) produced ~27 MB of OBS
// log in a 90-minute meeting, bloating support bundles and adding hot-path
// I/O. Those lines are suppressed from the OBS log by default and only
// emitted when CV_ZOOM_VERBOSE_LOG is set; they are still captured in the
// in-memory diagnostics ring buffer regardless. Read once, cached.
bool cv_zoom_verbose_logging();

class ZoomEngineClient {
public:
    struct DebugEvent {
        uint64_t timestamp_ms = 0;
        std::string stage;
        std::string source_uuid;
        uint32_t participant_id = 0;
        std::string message;
    };

    struct SourceCallbacks {
        // shm_generation: engine-side generation of the SHM region backing the
        // frame (increments each time the region is (re)created). 0 when the
        // engine did not report one (older engine binary).
        std::function<void(uint32_t width, uint32_t height,
                           uint32_t participant_id,
                           uint32_t shm_generation)> on_frame;
        std::function<void(uint32_t byte_len,
                           uint32_t participant_id,
                           uint32_t shm_generation)> on_audio;
    };

    static ZoomEngineClient &instance();

    bool start(const std::string &jwt_token,
               const std::string &public_app_key = {});
    void stop();
    // Same as stop() but does not set the user-leaving flag and does not
    // cancel a pending recovery. Used by ZoomReconnectManager between retries.
    void stop_for_reconnect();

    bool join(const std::string &meeting_id, const std::string &passcode,
              const std::string &display_name,
              MeetingKind kind = MeetingKind::Meeting,
              const ZoomJoinAuthTokens &tokens = {});
    void leave();
    void start_media();
    void stop_media();

    // Subscribe a source to a "spotlight slot" (1-based) instead of a fixed
    // participant. The engine resolves which participant owns that slot.
    void subscribe_spotlight(const std::string &source_uuid, uint32_t slot);
    // Subscribe a source to the active screen-share feed.
    void subscribe_screenshare(const std::string &source_uuid);

    // Used by ZoomReconnectManager to drive state transitions.
    void set_state(MeetingState s) { m_state.store(s, std::memory_order_release); }
    void subscribe(const std::string &source_uuid,
                   uint32_t participant_id,
                   bool isolate_audio,
                   bool audience_audio = false,
                   VideoResolution video_resolution = VideoResolution::P720);
    void subscribe_audio(const std::string &source_uuid,
                         uint32_t participant_id,
                         bool isolate_audio,
                         bool audience_audio);
    void unsubscribe(const std::string &source_uuid);

    MeetingState state() const { return m_state.load(std::memory_order_acquire); }
    bool is_running() const { return m_running.load(std::memory_order_acquire); }
    bool is_authenticated() const { return m_authenticated.load(std::memory_order_acquire); }
    bool is_media_active() const { return m_media_active.load(std::memory_order_acquire); }
    std::string last_error() const;
    void clear_last_error();
    uint32_t active_speaker_id() const;
    uint32_t raw_active_speaker_id() const;
    std::vector<ParticipantInfo> roster() const;
    std::vector<DebugEvent> recent_debug_events() const;

    void register_source(const std::string &source_uuid, SourceCallbacks callbacks);
    void unregister_source(const std::string &source_uuid);
    using RosterCallback = std::function<void()>;
    // Roster callbacks are invoked on the engine reader thread with this
    // client's internal lock RELEASED, so a callback may call back into the
    // client to read state (roster(), raw_active_speaker_id(), last_error())
    // or to change subscriptions (subscribe(), subscribe_spotlight(),
    // unsubscribe()). Those re-enter m_mtx and are safe precisely because the
    // lock is not held during dispatch.
    //
    // It may NOT call start(), stop() or stop_for_reconnect(). All three join
    // the reader thread, and the callback is running ON that thread, so the
    // join is a self-join and throws std::system_error
    // (resource_deadlock_would_occur). Anything that stops or restarts the
    // engine must be marshalled off this thread first.
    //
    // A callback must also not assume it runs on the Qt main thread — marshal
    // there yourself if you touch UI — and it should return promptly, since it
    // blocks the reader thread that also dispatches frame and audio events for
    // every source.
    void add_roster_callback(void *key, RosterCallback cb);
    void remove_roster_callback(void *key);
    using ErrorCallback = std::function<void(const std::string &message)>;
    void add_error_callback(void *key, ErrorCallback cb);
    void remove_error_callback(void *key);

private:
    ZoomEngineClient() = default;
    ~ZoomEngineClient();

    bool launch_engine();
    bool connect_ipc();
    void disconnect_ipc();
    void set_last_error(const std::string &message);
    void reader_loop();
    void monitor_loop();
    void handle_event(const std::string &line);
    void send_join_locked();
    // Returns false if the command could not be delivered to the engine.
    bool write_json(const std::string &json);

    // Applies `mutate` to the roster state under m_mtx, then invokes every
    // registered roster callback with the lock RELEASED.
    //
    // Releasing before dispatch is not an optimization, it is required for
    // correctness: callbacks re-enter this client (ZoomSource::on_roster_changed
    // and the CoreVideo Tiles source both call roster() from inside one) and
    // m_mtx is not recursive. Invoking under the lock self-deadlocks the engine
    // reader thread, which stops frame and audio dispatch for every source in
    // the plugin until the heartbeat monitor kills the engine.
    //
    // Every roster-state change that NOTIFIES callbacks goes through here, so
    // that this reasoning lives in one place. One site deliberately does not:
    // the cmd == "left" handler in handle_event() clears m_roster, zeroes
    // m_active_speaker_id and calls SpeakerDirector::reset() under m_mtx
    // without notifying anyone. That is intentional — routing it through here
    // would add a roster-callback dispatch on every leave, which is a behavior
    // change. Leave it alone.
    //
    // Being private makes the correct path the shortest one, but it cannot
    // force a future site to take it. The same snapshot-then-dispatch shape is
    // still hand-rolled for *error* callbacks, in the "error"/"auth_fail"
    // handling in handle_event() and in clear_last_error(), so that copy-paste
    // template remains in this file. If you extend those, factor them into a
    // sibling helper rather than open-coding the pattern a fourth time.
    //
    // `mutate` runs with m_mtx held: it may touch m_roster /
    // m_active_speaker_id freely, but it must not call any public method of
    // this client.
    template <typename Mutate>
    void update_roster_state_and_notify(Mutate &&mutate)
    {
        std::vector<RosterCallback> callbacks;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            mutate();
            for (const auto &entry : m_roster_callbacks)
                if (entry.second) callbacks.push_back(entry.second);
        }
        // m_mtx is released here — see the note above before moving this loop.
        for (const auto &cb : callbacks) cb();
    }

    mutable std::mutex m_mtx;
    IpcFd m_p2e = kIpcInvalidFd;
    IpcFd m_e2p = kIpcInvalidFd;
    std::thread m_reader;
    std::thread m_monitor;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_authenticated{false};
    std::atomic<bool> m_media_active{false};
    std::atomic<MeetingState> m_state{MeetingState::Idle};
    // Wall-clock ms (os_gettime_ns()/1e6) of the last line received from the
    // engine. Used by monitor_loop() to detect a hung-but-alive engine.
    std::atomic<uint64_t> m_last_rx_ms{0};
    uint32_t m_active_speaker_id = 0;
    std::vector<ParticipantInfo> m_roster;
    std::unordered_map<std::string, SourceCallbacks> m_sources;
    std::unordered_map<void *, RosterCallback> m_roster_callbacks;
    std::unordered_map<void *, ErrorCallback> m_error_callbacks;
    std::string m_last_error;
    std::deque<DebugEvent> m_debug_events;
    // Tracks whether the user deliberately requested a leave/stop (suppresses recovery).
    std::atomic<bool> m_user_leaving{false};
    std::string m_last_jwt; // stored so reconnect manager can access it
    bool m_join_pending = false;
    std::string m_pending_meeting_id;
    std::string m_pending_passcode;
    std::string m_pending_display_name;
    ZoomJoinAuthTokens m_pending_tokens;
    MeetingKind m_pending_kind = MeetingKind::Meeting;

#if defined(WIN32)
    void *m_process = nullptr;
#else
    int m_pid = -1;
#endif
};
