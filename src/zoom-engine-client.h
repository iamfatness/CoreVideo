#pragma once

#include "engine-ipc.h"
#include "media-event-queue.h"
#include "media-failure-state.h"
#include "talkback-nomination.h"
#include "zoom-types.h"
#include <atomic>
#include <condition_variable>
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

    // F2 review-round fix (CRITICAL): the engine's own CONFIRMED state for
    // the persistent talkback session -- distinct from both the plugin's
    // local intent (TalkbackKeyState::open) and the tap's local liveness
    // signal (TalkbackTap::last_audio_ms(), which stays fresh even when the
    // Zoom channel never opened at all, because the tap keeps publishing
    // audio into the ring regardless of whether anything on the far end can
    // hear it). Written by handle_event()'s talkback_session branch from the
    // engine's {"cmd":"talkback_session","live":...,"reason":"..."} line
    // (engine/src/engine-talkback.cpp's report_session_state()); read by
    // TalkbackController::status_json()/evaluate(). live=false with a
    // non-empty reason means an explicit failure; live=false with an empty
    // reason means no session has ever reported in yet (the initial state,
    // or right after talkback_start() resets it -- see talkback_start()
    // below) -- the two must not be conflated, which is why evaluate()'s
    // grace period keys off reason being non-empty, not off live alone.
    struct TalkbackSessionStatus {
        bool live = false;
        std::string reason;

        // TALKBACK DELIVERY LAW 1 (2026-08-29): is this key live over a bot
        // whose meeting audio the engine could NOT open?
        //
        // Talkback delivers only while the engine's own client is unmuted --
        // muted, SendAudioDataToChannel is ACCEPTED and every member hears
        // silence, which is the one failure this feature cannot afford to show
        // as success. The engine reports `"mic":"open"|"blocked"` on the SAME
        // confirmed-state line as `live` (report_session_state()), and
        // re-emits it on a mid-key CHANGE, so a host muting the bot at second
        // 30 of a latched key moves this too.
        //
        // ABSENT MEANS false, and that is the mixed-version rule, not an
        // accident: an engine older than Law 1 sends no "mic" key at all, and
        // a DLL-only install is this project's canonical mistake. Reading a
        // missing field as "blocked" would put every such rig into a permanent
        // false alarm; reading it as "open" is the same thing that engine
        // already meant.
        bool mic_blocked = false;

        // Milestone 7 (the dock). The three fields below come from the OTHER
        // shape on this cmd -- report_session()'s stage lines -- not from
        // report_session_state()'s confirmed-state line above, so they are
        // written by a different branch of handle_event() and can lag or lead
        // `live` by one line. All three are cleared with the rest of this
        // struct at talkback_start(), so nothing here can describe a previous
        // key.
        //
        // members_present/members_total are the keyed target's membership AS
        // OF THE MOMENT THE KEY OPENED: the engine reports them once, in its
        // "session_live" stage line, and does not re-report them while the key
        // is held. A talent who rejoins mid-press is therefore not counted
        // until the next press. members_known distinguishes "0 of 0" from
        // "no session_live has arrived", which is not the same thing.
        bool members_known = false;
        uint32_t members_present = 0;
        uint32_t members_total = 0;
        // The engine's own recovery hint for a refusal, echoed rather than
        // inferred: session_start's refusal line carries
        // "recover":"re-nominate" for provisioning_incomplete, and nothing at
        // all for the other reasons. The plugin never invents a remedy the
        // engine did not name.
        std::string recover;
    };

    // Task 5: the plugin's own record of the last CONFIRMED talkback_nominate()
    // plan. Fix round 1 (F1/F2): this used to be written optimistically at
    // send time and never invalidated, which both falsely refused a key on a
    // still-standing channel after a refused re-nomination (F1) and kept
    // advertising a plan a Leave/engine-restart had already destroyed (F2).
    // It is now the pure TalkbackNominationPlan type from
    // src/talkback-nomination.h -- see that header's comment for the full
    // account and why it had to move out of Qt/OBS reach to be pinned by a
    // test. `requested`/`uncovered_private` are the ONLY fields
    // TalkbackController::key_on() may read (via
    // talkback_target_known_unprovisioned(), src/talkback-plan.h);
    // `last_attempt_ok`/`last_attempt_reason` are diagnostic only.
    using TalkbackNominationStatus = TalkbackNominationPlan;

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
    // Triggers the Milestone 1 talkback probe for one named participant. The
    // probe's progress reports arrive asynchronously as "talkback_probe"
    // lines and are logged verbatim in handle_event(); this call itself does
    // not block or return a result.
    void talkback_probe(const std::string &participant_name);
    // Latest talkback_probe stage line (raw compact JSON), for the dock's
    // status label. Written by handle_event()'s talkback_probe branch and
    // polled by the dock's existing 100ms refresh timer -- the same pattern
    // last_error() and roster() already use -- rather than a dedicated
    // signal/slot path, so this diagnostic doesn't need its own plumbing on
    // top of what every other dock readout already relies on.
    std::string talkback_probe_status() const;

    // F2 review-round fix: the engine-confirmed session state -- see the
    // TalkbackSessionStatus doc comment above. Same polled-not-signalled
    // pattern as talkback_probe_status() above, for the same reason.
    TalkbackSessionStatus talkback_session_status() const;

    // Task 5: forwards a nomination's name list to the engine's nominate()
    // (engine/src/engine-talkback.cpp). Fire-and-forget like talkback_probe --
    // the plan outcome (channel count, who is uncovered, who is unreachable)
    // arrives asynchronously as "cmd":"talkback_nominate" stage lines, logged
    // verbatim in handle_event() and summarised in
    // talkback_nomination_status() below. An empty list is a deliberate
    // denominate (see nominate()'s own doc comment), not a no-op.
    void talkback_nominate(const std::vector<std::string> &nominees);
    // Same polled-not-signalled pattern as talkback_probe_status() /
    // talkback_session_status() above -- see TalkbackNominationStatus's doc
    // comment for what each field means and where it comes from.
    TalkbackNominationStatus talkback_nomination_status() const;
    // Milestone 7 (the intercom grid): who the engine has actually confirmed
    // INTO a talkback channel, which the plan above cannot say. Assembled
    // from the same "cmd":"talkback_nominate" stage lines, by
    // talkback_channel_presence_apply_report()
    // (src/talkback-nomination-dispatch.h). Polled and copied under m_mtx
    // like every status above, and display-only: nothing in the keying path
    // reads it. See TalkbackChannelPresence in src/talkback-nomination.h for
    // the live defect it exists for and the two limits it carries.
    TalkbackChannelPresence talkback_channel_presence() const;

    // Milestone 5's live-talkback senders. All five follow talkback_probe's
    // shape exactly: guarded by m_running, fire-and-forget, no result
    // returned here -- the engine's dispatch/response, if any, arrives async
    // like every other command on this pipe.
    //
    // Task 5: `target` is "all" (kTalkbackAllTalentTarget) or a nominee's
    // name -- session_start() on the engine side SELECTS an
    // already-provisioned channel, it does not create one. Sent as the
    // "target" JSON field; the engine's "participant" fallback
    // (engine/src/main.cpp) is a compatibility shim for pre-Task-5 senders
    // only, not a second spelling this plugin uses.
    void talkback_start(const std::string &target);
    void talkback_stop();
    // Must be sent only after the caller's ring header is fully laid out
    // (talkback_ring_init has run): the engine validates slot_count/
    // slot_bytes from the header when it maps the region, and would reject
    // one it mapped before the header was written.
    void talkback_open(const std::string &region, uint32_t rate, uint16_t channels);
    void talkback_audio();
    void talkback_close();

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
    // True while Zoom has us in a waiting room, or the meeting has not started
    // yet (MEETING_STATUS_IN_WAITING_ROOM / _WAITINGFORHOST, reported by the
    // engine on every status change).
    //
    // Exposed for the same watchdog and for the same reason as
    // is_init_retry_pending() above: this wait happens *inside* the Joining
    // window and is open-ended, because it ends only when a host acts. Charged
    // against the join deadline it auto-left a live meeting after 114s in a
    // waiting room (2026-08-22); see src/join-watchdog.h. Point-in-time read,
    // safe from any thread.
    bool is_awaiting_admission() const {
        return m_awaiting_admission.load(std::memory_order_acquire);
    }
    bool is_media_active() const { return m_media_active.load(std::memory_order_acquire); }
    // Meeting error first, otherwise the terminal raw-media diagnostic. Internal
    // meeting/leave classification deliberately reads only m_last_error.
    std::string last_error() const;
    void clear_last_error();
    // Record-privilege notice or actionable terminal raw-media diagnostic.
    // Empty when neither is pending. See
    // src/zoom-privilege-notice.h for what this state means and
    // add_notice_callback() below for how it is pushed. Exposed as a getter
    // too, mirroring last_error(), so a dock can resync on its own poll tick
    // (update_state_indicator()'s 100ms timer) rather than depend solely on
    // catching the callback.
    std::string pending_privilege_notice() const;
    uint32_t active_speaker_id() const;
    uint32_t raw_active_speaker_id() const;
    std::vector<ParticipantInfo> roster() const;
    std::vector<DebugEvent> recent_debug_events() const;

    void register_source(const std::string &source_uuid, SourceCallbacks callbacks);
    void unregister_source(const std::string &source_uuid);
    // Capture before the shared-memory read, acknowledge only a successful
    // read. Reassignment during the read invalidates the ticket.
    uint64_t media_delivery_ticket(const std::string &uuid, uint32_t participant) const;
    void acknowledge_media_delivery(const std::string &uuid, uint32_t participant, uint64_t ticket);

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
    // A SEPARATE callback from ErrorCallback above, deliberately. Every
    // existing (and future) error-callback subscriber's contract is "this
    // means show the operator a failure" -- the dock's own registration pops
    // a QMessageBox unconditionally whenever the message is non-empty. The
    // record-privilege wait (src/zoom-privilege-notice.h) is not a failure,
    // so it never enters that list at all; the invariant "this can never
    // reach a QMessageBox" is enforced by which list a report goes to, not by
    // a severity flag every subscriber has to remember to check. Empty
    // message means "clear the notice", exactly like ErrorCallback's empty
    // message convention (see clear_last_error()).
    using NoticeCallback = std::function<void(const std::string &message)>;
    void add_notice_callback(void *key, NoticeCallback cb);
    void remove_notice_callback(void *key);

private:
    // Starts the two media dispatch lanes; see MediaDispatchLane below.
    ZoomEngineClient();
    ~ZoomEngineClient();

    // Runs on a lane thread: resolves the uuid to its registered callbacks
    // under m_mtx (the same lookup handle_event used to do inline) and
    // invokes the frame or audio callback.
    void dispatch_media_event(bool is_frame, const std::string &uuid,
                              const MediaEvent &event);

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
    // Same snapshot-then-dispatch shape as set_error_and_notify(), over
    // m_privilege_notice / m_notice_callbacks instead of m_last_error /
    // m_error_callbacks -- see NoticeCallback's doc comment above for why
    // these are separate lists rather than one.
    void set_privilege_notice_and_notify(const std::string &message);
    // Clears m_privilege_notice and dispatches an empty message, but only if
    // a notice was actually pending -- called from the "raw_media_ready"
    // debug stage on every media start, most of which never had a notice to
    // clear.
    void clear_privilege_notice_and_notify();
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
    std::atomic<bool> m_awaiting_admission{false};
    std::atomic<MeetingState> m_state{MeetingState::Idle};
    // Wall-clock ms (os_gettime_ns()/1e6) of the last line received from the
    // engine. Used by monitor_loop() to detect a hung-but-alive engine.
    std::atomic<uint64_t> m_last_rx_ms{0};
    uint32_t m_active_speaker_id = 0;
    std::vector<ParticipantInfo> m_roster;
    std::unordered_map<std::string, SourceCallbacks> m_sources;
    std::unordered_map<void *, RosterCallback> m_roster_callbacks;
    std::unordered_map<void *, ErrorCallback> m_error_callbacks;
    std::unordered_map<void *, NoticeCallback> m_notice_callbacks;
    std::string m_last_error;
    // Terminal raw-media diagnostic, surfaced by last_error() only as fallback.
    // Never consulted by meeting/reconnect classification. Guarded by m_mtx.
    std::string m_raw_media_error;
    MediaFailureState m_media_failures;
    // Empty when no record-privilege notice is pending. Deliberately NEVER
    // written to/from m_last_error -- see pending_privilege_notice()'s doc
    // comment and NoticeCallback's above. Guarded by m_mtx like m_last_error.
    std::string m_privilege_notice;
    // Raw compact JSON of the most recent talkback_probe stage line; see
    // talkback_probe_status() above.
    std::string m_talkback_probe_status;
    // F2 review-round fix: the engine-confirmed session state; see
    // TalkbackSessionStatus and talkback_session_status() above. Guarded by
    // m_mtx, same as m_talkback_probe_status.
    TalkbackSessionStatus m_talkback_session_status;
    // Task 5: guarded by m_mtx, same as the two statuses above. This is the
    // CONFIRMED plan only -- see TalkbackNominationStatus's doc comment and
    // src/talkback-nomination.h. `m_talkback_nomination_pending` stages an
    // in-flight attempt's stage reports until they either commit into this
    // field (talkback_nomination_commit(), on "nominate_done") or are
    // discarded on a refusal (talkback_nomination_note_refused(), which
    // leaves this field untouched).
    TalkbackNominationStatus m_talkback_nomination_status;
    TalkbackNominationPending m_talkback_nomination_pending;
    // Milestone 7: the per-person channel presence view, guarded by m_mtx
    // like everything above it. Cleared at exactly the points
    // talkback_nomination_reset() is called (a Leave, an engine restart) and
    // additionally at the send of a new nominate -- a nomination replaces
    // the standing channel set, so every observation in here is about
    // channels the engine is destroying.
    TalkbackChannelPresence m_talkback_channel_presence;
    // C1 (CRITICAL, final whole-branch review 2026-08-26): the identity
    // stamped into each talkback_nominate request and echoed back in that
    // attempt's terminal reports, so a report can be matched to the staging
    // slot it belongs to instead of to whatever happens to be staged when it
    // arrives. Guarded by m_mtx, like the two records above.
    //
    // PROCESS-WIDE MONOTONIC, deliberately NOT reset by any world-reset
    // (Leave, engine restart) that resets the two records above: an id that
    // can be re-used is an id that can make a report from before the reset
    // match a staging slot from after it, which is the same class of bug the
    // id exists to close. Pre-incremented, so live ids are always >= 1 and 0
    // reads unambiguously as "nothing staged".
    uint32_t m_talkback_nominate_attempt = 0;
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

    // One media dispatch lane: a coalescing queue plus the thread that drains
    // it. Two exist -- video and audio -- so a burst of 1080p frame copies
    // can never delay an audio drain by more than the single handler already
    // in flight (the per-source callback gate serialises same-source work; see
    // the constructor for the full rationale and the measured defect).
    //
    // Lanes are constructed with the client singleton and joined in its
    // destructor -- they deliberately do NOT follow engine-session lifecycle.
    // A pending entry that outlives its engine or its source dispatches into
    // a lookup miss or a generation guard and does nothing, which is the same
    // tolerance those handlers already need for events that were in the pipe
    // when the engine died.
    struct MediaDispatchLane {
        MediaEventQueue queue;
        std::mutex mtx;
        std::condition_variable cv;
        bool stop = false;
        std::thread thread;

        void push(const std::string &uuid, const MediaEvent &event)
        {
            if (queue.push(uuid, event)) {
                std::lock_guard<std::mutex> lk(mtx);
                cv.notify_one();
            }
        }
        void run(const std::function<void(const std::string &,
                                          const MediaEvent &)> &dispatch)
        {
            for (;;) {
                {
                    std::unique_lock<std::mutex> lk(mtx);
                    cv.wait(lk, [this] { return stop || !queue.empty(); });
                    if (stop) return;
                }
                for (const auto &entry : queue.drain())
                    dispatch(entry.first, entry.second);
            }
        }
        void shutdown()
        {
            {
                std::lock_guard<std::mutex> lk(mtx);
                stop = true;
                cv.notify_one();
            }
            if (thread.joinable()) thread.join();
        }
    };
    MediaDispatchLane m_video_lane;
    MediaDispatchLane m_audio_lane;

    // Serialises start() bodies. The m_running early-return only filters
    // callers that arrive AFTER a start finished; two callers arriving
    // together both read m_running == false and both launch — observed live
    // 2026-08-17 as two "New ZoomObsEngine process" launches 9 ms apart, the
    // first of which becomes an orphaned ghost writer (see
    // terminate_stale_engine_processes() in the .cpp for what that ghost then
    // does to the audio rings). The second caller now waits, re-checks
    // m_running under the lock, and returns the first caller's outcome
    // instead of launching a rival.
    std::mutex m_start_mtx;

#if defined(WIN32)
    void *m_process = nullptr;
#else
    int m_pid = -1;
#endif
};
