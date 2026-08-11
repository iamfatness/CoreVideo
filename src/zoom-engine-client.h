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
        // "A NEW ZoomObsEngine PROCESS is about to serve you — drop every
        // shared-memory read mapping you hold." Invoked once per source, from
        // start(), BEFORE the replacement process is launched and therefore
        // before anything — including a subscribe that was already in flight
        // when the old engine died — can reach it.
        //
        // Why this exists. The engine's SHM generation counter
        // (src/shm-generation.h) is process-wide, which is what makes a resize
        // land on a name nothing can still be mapping — but only within one
        // engine process. A restarted engine starts from an empty table, so its
        // FIRST create for any region asks for generation 1, and generation 1
        // is the legacy unsuffixed name (shm_region_name() in engine-ipc.h).
        // Every mapping we carried across the restart is therefore standing on
        // a name the new engine is about to ask for, and on Windows a live
        // mapping makes that create fail (ERROR_ACCESS_DENIED) whenever the new
        // region needs to be larger. See src/shm-resubscribe.h.
        //
        // This is a precondition of the new engine's first create, not cleanup.
        // The registry it is dispatched from IS the set of mapping holders:
        // a region name is only ever learned from a frame/audio event, so
        // anything holding a mapping registered here to receive them.
        //
        // Called on the thread that called start() (a Qt worker, the control
        // server, the OSC server, or the reconnect thread) with this client's
        // m_mtx RELEASED, for the same reason roster callbacks are — the
        // callback takes the source's own lock and may re-enter this client.
        // It must not talk to the engine; unmapping is all it may do.
        std::function<void()> on_new_engine_process;
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
    // video_only suppresses the engine-side audio target for this source. It
    // defaults to false so every existing caller keeps receiving audio; only a
    // source that will never register an on_audio callback should set it (the
    // CoreVideo Tiles wall). It is NOT the same as isolate_audio, which claims
    // the participant's one-way stream and would starve an audience-audio
    // source pointed at the same person.
    void subscribe(const std::string &source_uuid,
                   uint32_t participant_id,
                   bool isolate_audio,
                   bool audience_audio = false,
                   VideoResolution video_resolution = VideoResolution::P720,
                   bool video_only = false);
    // Returns true only if the command was handed to a running engine over a
    // live pipe. False means it was dropped — no engine, or the link broke —
    // and the caller must NOT record the source as subscribed: the dedicated
    // CoreVideo audio sources have no sweep equivalent to
    // ZoomOutputManager::resubscribe_all() to correct a flag that says
    // "subscribed" about a command the engine never saw, so a false claim there
    // is silent for the rest of the session.
    bool subscribe_audio(const std::string &source_uuid,
                         uint32_t participant_id,
                         bool isolate_audio,
                         bool audience_audio);
    void unsubscribe(const std::string &source_uuid);

    MeetingState state() const { return m_state.load(std::memory_order_acquire); }
    bool is_running() const { return m_running.load(std::memory_order_acquire); }
    bool is_authenticated() const { return m_authenticated.load(std::memory_order_acquire); }
    // True while a scheduled SDKERR_OTHER_SDK_INSTANCE_RUNNING init retry is
    // still waiting to be replayed (see m_init_retry_due_ms below).
    //
    // Exposed for the dock's join watchdog. That watchdog measures how long the
    // state has been Joining, and the init retry runs *inside* that window
    // (on_join_clicked() sets Joining before start(), and every start() is
    // immediately followed by join()), so a long wait for another SDK instance
    // would otherwise be charged against the join's deadline. This is a
    // point-in-time read, safe from any thread; it can go false at any moment
    // when the monitor thread replays the init.
    bool is_init_retry_pending() const {
        return m_init_retry_due_ms.load(std::memory_order_acquire) != 0;
    }
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
    // Fires every registered source's on_new_engine_process callback. Called
    // from start() only, and before launch_engine() — see the definition, and
    // the call site, for why it has to run before the replacement process
    // exists rather than merely before its first create.
    void release_source_mappings_for_new_engine();
    bool connect_ipc();
    void disconnect_ipc();
    void set_last_error(const std::string &message);
    // Records `message` as the last error and hands it to every registered
    // error callback with m_mtx RELEASED, for the same reason
    // update_roster_state_and_notify() releases it: a callback may re-enter
    // this client (the dock reads last_error() from one) and m_mtx is not
    // recursive.
    void set_error_and_notify(const std::string &message);
    // Runs on the monitor thread ONLY (see m_init_teardown_pending). Stops the
    // engine process, then surfaces the operator-facing failure.
    void fail_after_init_retries_exhausted();
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
    // force a future site to take it. The error-callback side of the same
    // snapshot-then-dispatch shape now lives in set_error_and_notify(); use
    // that rather than open-coding the pattern again. clear_last_error() is the
    // one remaining hand-rolled copy, because it clears rather than sets.
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

    // --- Zoom SDK init retry (SDKERR_OTHER_SDK_INSTANCE_RUNNING) ------------
    // An orphaned ZoomObsEngine from a previous OBS session still holds the
    // Zoom SDK when the freshly launched engine calls InitSDK, so the FIRST
    // engine request of a session fails and the operator has to ask twice. The
    // orphan exits on its own, so we replay the init command a bounded number
    // of times instead of failing. The rules live in zoom-sdk-init-retry.h.
    //
    // The wait deliberately runs on the monitor thread. start() is called from
    // a Qt worker (and from the control/OSC server threads), the auth_fail
    // arrives on the reader thread, and neither may sleep: the reader also
    // dispatches every source's frames and keeps m_last_rx_ms fresh, so a
    // sleeping reader would stall video AND let monitor_loop() decide the
    // engine had gone silent. monitor_loop() already ticks about once a second
    // and exits as soon as m_running clears, so stop() cancels a pending retry
    // for free and never blocks on it.
    //
    // 0 = nothing scheduled; otherwise the wall-clock ms (os_gettime_ns()/1e6)
    // at which monitor_loop() should replay m_init_payload.
    std::atomic<uint64_t> m_init_retry_due_ms{0};
    // Set by the reader thread when the retry bound is spent; the monitor
    // thread notices it, tears the engine process down and only then surfaces
    // the failure. The teardown cannot happen where the failure is detected:
    // that is the reader thread, and stop() joins the reader thread (see the
    // roster-callback note above — the same self-join hazard). It also cannot
    // be skipped: an engine process that is alive but unauthenticated makes
    // start() early-return on m_running, so "request the engine again" would
    // silently do nothing, which is what the operator already hit in the field
    // log when their second click sent no init at all.
    std::atomic<bool> m_init_teardown_pending{false};
    // Written by the reader thread (retry accounting) and by start() /
    // stop_for_reconnect() / fail_after_init_retries_exhausted() when they
    // reset it. Those resets are safe unlocked because start() and
    // stop_for_reconnect() only touch them with the reader thread joined, and
    // the monitor thread reads them only after acquiring
    // m_init_teardown_pending, which the reader releases after its last write.
    int m_init_retry_attempts = 0;
    uint64_t m_init_retry_waited_ms = 0;
    // The exact init command to replay. Guarded by m_mtx; holds an SDK
    // credential, so it is cleared on teardown.
    std::string m_init_payload;

#if defined(WIN32)
    void *m_process = nullptr;
#else
    int m_pid = -1;
#endif
};
