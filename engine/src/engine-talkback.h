#pragma once
//
// engine-talkback.h — the Zoom talkback probe (Milestone 1).
//
// Talkback is the first path in this codebase that SENDS audio to Zoom. Every
// other media path runs engine -> plugin. This class exists to answer one
// question before any of that is built: can this account open a talkback
// channel and put audio in it?
//
// Neither the SDK headers nor Zoom's documentation state what entitles
// talkback. The 7.0.0 changelog says only "Support talkback audio feature" and
// lists Permission denied among the error codes. Our working assumptions are
// host/co-host plus the Zoom Enhanced Media add-on, and this probe is how they
// get tested rather than believed.
//
// Every rung reports its own SDKError and TalkbackError over E2P, so a failure
// names the exact rung it fell off instead of surfacing as silence.
//
// engine-ipc.h must come before any Zoom SDK header: it pulls in <windows.h>
// (under WIN32), and zoom_sdk_def.h uses HWND without including it itself.
// Every other engine-*.h in this codebase follows the same order.
#include "../../src/engine-ipc.h"

#include "zoom_sdk.h"
#include "meeting_service_interface.h"
#include "meeting_service_components/meeting_talkback_ctrl_interface.h"
// meeting_service_interface.h only forward-declares IMeetingParticipantsController;
// resolve_participant() needs the full definition (GetParticipantsList,
// GetUserByUserID) and IUserInfo (GetUserName). meeting_participants_ctrl_interface.h
// uses AudioType without including its home header, so meeting_audio_interface.h
// must come first -- same order main.cpp already uses for this same pair.
#include "meeting_service_components/meeting_audio_interface.h"
#include "meeting_service_components/meeting_participants_ctrl_interface.h"

// talkback_pcm_rate_supported() -- F7 review-round fix uses this to validate
// the ring header's sample_rate engine-side, the same gate the plugin
// already applies before it ever creates the region.
#include "../../src/talkback-pcm.h"
#include "../../src/talkback-channel-owner.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class EngineTalkback : public ZOOMSDK::IMeetingTalkbackCtrlEvent {
public:
    // Starts the probe ladder. Reports and returns without blocking; the
    // asynchronous rungs continue through the callbacks below and tick().
    // Returns true if a new ladder actually started, false if the
    // re-entrancy guard refused (a ladder is already in flight). The caller
    // MUST use this to decide whether to spawn a tick()-driving thread --
    // spawning one unconditionally reintroduces the exact two-threads-call-
    // tick() hazard the invariants on m_chan_mtx / the batch-destroy API
    // above are built to rule out. See the caller in engine/src/main.cpp.
    bool probe(ZOOMSDK::IMeetingService *svc, const std::string &participant_name);

    // Called from the engine's main loop. Sends tone buffers while a send is
    // in progress, then destroys the channel.
    void tick();

    // ── Talkback audio path (Milestone 2) ──────────────────────────────────
    bool open_audio(const std::string &region_name, uint32_t sample_rate,
                    uint16_t channels);
    void drain_audio();
    void close_audio();

    // ── Persistent talkback session (Milestone 5) ──────────────────────────
    // Deliberately NOT part of the probe's Phase machine: that machine exists
    // to tear itself down after one tone, which is the opposite of what a key
    // held down needs. The session owns its OWN channel, so tick() -- which
    // destroys the PROBE's channel from a separate thread -- can never touch
    // it. That separation is the fix for the probe-thread race, and it is
    // structural rather than a lock.
    bool session_start(ZOOMSDK::IMeetingService *svc,
                       const std::string &participant_name);
    void session_stop();
    bool session_live() const;

    // True once the ladder is quiescent: Idle before the first probe() ever
    // runs, Done after one finishes (success, failure, or abandoned
    // destroy). Task 5's driving thread uses this to stop ticking as soon as
    // the probe settles instead of always spinning its full bound -- and
    // deliberately exposes only this bool, not m_phase itself, so callers
    // outside this file never take a dependency on the phase enum's shape.
    bool is_idle() const
    {
        const Phase p = m_phase.load(std::memory_order_acquire);
        return p == Phase::Idle || p == Phase::Done;
    }

    // True while the driving loop in main.cpp must keep calling tick():
    // either the ladder itself is not settled, OR a stray channel is queued
    // and still needs drain_stray_channels() (called only from tick()) to
    // run for it. is_idle() and has_pending_work() answer different
    // questions and must not be conflated: is_idle() answers "may a new
    // ladder start?" -- the refusal gate in main.cpp uses it, and a pending
    // stray must NOT make that gate refuse new probes indefinitely, because
    // a stray drain has nothing to do with whether a fresh ladder is safe to
    // start. has_pending_work() answers "must the driver keep running?" --
    // the driving loop uses it instead of is_idle() so it does not exit and
    // orphan a queued-but-undrained stray channel between the ladder
    // settling to Idle/Done and drain_stray_channels() next getting a
    // chance to run (see the F3 review-round finding: AwaitingChannel times
    // out at 10s -> Destroying -> Done -> driver exits -> a genuinely late
    // onCreateChannelResponse arrives after that and queues a real Zoom
    // channel that nothing then destroys).
    //
    // R1-round-3 review fix: session_start() gates on this function believing
    // "false" means the driving thread will not touch m_ctrl. That was wrong
    // for one specific window: drain_stray_channels() swaps m_stray_channels
    // into a local UNDER m_chan_mtx, releases the lock, and only THEN runs
    // its Begin/Add/ExecuteBatchDestroyChannels loop against m_ctrl. In that
    // window the member m_stray_channels already reads empty and m_phase can
    // independently already read Done, so the two checks below would both
    // pass and this function would report "nothing to wait for" while the
    // driving thread is still mid SDK-call. m_driving_thread_in_sdk_call is
    // what actually closes that: set before drain_stray_channels() risks
    // touching the SDK, cleared once it's done, checked here FIRST so it
    // dominates the other two checks. (tick()'s Destroying-phase SDK
    // sequence does not need the same treatment: unlike the stray path, it
    // never stores Phase::Done until strictly after its own Begin/Add/
    // Execute sequence finishes, so m_phase alone already reads "busy" for
    // that entire window -- traced, not assumed.)
    bool has_pending_work() const
    {
        if (m_driving_thread_in_sdk_call.load(std::memory_order_acquire))
            return true;
        const Phase p = m_phase.load(std::memory_order_acquire);
        if (p != Phase::Idle && p != Phase::Done) return true;
        // Takes m_chan_mtx only for this queue check -- never call this
        // function while already holding m_chan_mtx elsewhere, same
        // discipline as every other access to m_stray_channels.
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        return !m_stray_channels.empty();
    }

    // IMeetingTalkbackCtrlEvent
    void onCreateChannelResponse(const zchar_t *channelID, TalkbackError error) override;
    void onDestroyChannelResponse(const zchar_t *channelID, TalkbackError error) override;
    void onChannelUserJoinResponse(const zchar_t *channelID, unsigned int userID,
                                   TalkbackError error) override;
    void onChannelUserLeaveResponse(const zchar_t *channelID, unsigned int userID,
                                    TalkbackError error) override;
    void onJoinTalkbackChannel(unsigned int inviterID) override;
    void onLeaveTalkbackChannel(unsigned int inviterID) override;
    void onInviterAudioLevel(unsigned int inviterID, unsigned int audioLevel) override;

private:
    enum class Phase { Idle, AwaitingChannel, AwaitingInvite, Sending, Destroying, Done };

    // const: touches no member state, only formats a string and writes to
    // the pipe -- resolve_participant() (below, const) needs to call it to
    // report the per-user talkback gate (F2 review-round fix) without
    // losing its own const-ness.
    void report(const std::string &stage, const std::string &fields) const;
    unsigned int resolve_participant(const std::string &name) const;

    // Drains m_stray_channels and destroys each one. Called from tick() only
    // -- see the invariant comment at that call site.
    void drain_stray_channels();

    ZOOMSDK::IMeetingService          *m_svc  = nullptr;
    ZOOMSDK::IMeetingTalkbackController *m_ctrl = nullptr;

    // m_phase is written from SDK callback threads (onCreateChannelResponse,
    // onChannelUserJoinResponse) and, once Task 5 wires tick() to the engine
    // main loop, read/written from the engine thread too -- atomic with
    // explicit acquire/release is the cheap, correct fix for THIS field: a
    // trivially-copyable enum can be published as a unit with no lock. It
    // stays atomic rather than folding into m_chan_mtx below -- the two
    // mechanisms are complementary, not competing: the atomic is the fast
    // path every function checks first, the mutex is only for the string
    // fields it is unsafe to reason about from phase ordering alone.
    std::atomic<Phase> m_phase{Phase::Idle};

    // m_channel_id / m_channel_id_z / m_stray_channels are NOT safe under
    // acquire/release on m_phase alone and must go through m_chan_mtx for
    // EVERY access from here down, cross-thread or not -- no exceptions, so
    // nobody has to re-derive which phases are "safe".
    //
    // The tempting argument -- "m_channel_id_z is fully written in
    // onCreateChannelResponse before m_phase is released to AwaitingInvite,
    // so an acquire-load of m_phase >= AwaitingInvite is a synchronizes-with
    // edge" -- is real but incomplete: it only covers phases AT OR ABOVE
    // AwaitingInvite. It says nothing about Idle or Done. probe()'s
    // re-entrancy guard deliberately ALLOWS a fresh probe() to run whenever
    // phase is Idle/Done, and that fresh call clears/reassigns these members
    // from whatever thread called probe(). Meanwhile a late or duplicate
    // onCreateChannelResponse for the PREVIOUS probe can observe phase as
    // Idle/Done (via its own acquire-load) and read these same members on
    // the SDK callback thread at the same moment -- a heap-buffer read
    // racing a concurrent std::basic_string mutation: undefined behaviour,
    // not merely a stale value. This is on the expected path (it is exactly
    // the late-callback scenario the timeout machinery exists to handle),
    // found live in review round 3 after round 2's stray-channel fix added
    // the first cross-thread read of m_channel_id_z that could land in the
    // Idle/Done window.
    //
    // Discipline: copy the needed value out under the lock, release, THEN
    // call the SDK or build a report string with the copy. Never call the
    // SDK while holding m_chan_mtx.
    //
    // mutable: has_pending_work() (above, const) takes this lock purely to
    // read whether m_stray_channels is empty -- a read-only query from the
    // outside, so it is declared const like is_idle(), which requires the
    // mutex itself be lockable from a const method.
    mutable std::mutex m_chan_mtx;
    std::string  m_channel_id;      // UTF-8, REPORTING ONLY -- never pass to
                                     // the SDK, see m_channel_id_z below.
    // zchar_t is wchar_t on Windows (zoom_sdk_def.h) but char elsewhere, so
    // basic_string<zchar_t> is the only type that is simultaneously correct
    // on both platforms and round-trip-safe for an opaque SDK identifier
    // (no UTF-8 re-encoding). Every SDK call that takes a channel ID copies
    // this out under m_chan_mtx first; never call .c_str() on it directly.
    std::basic_string<zchar_t> m_channel_id_z;

    // BeginBatchDestroyChannels/AddChannelToDestroy/ExecuteBatchDestroyChannels
    // has exactly one caller: tick(), on whichever thread owns the engine
    // main loop -- see the invariant comment at the top of tick(). Callbacks
    // that discover a channel needing cleanup push its id here (under
    // m_chan_mtx) instead of calling the SDK; drain_stray_channels(), called
    // only from tick(), is the sole drainer.
    std::vector<std::basic_string<zchar_t>> m_stray_channels;

    // R1-round-3 review fix: true for exactly the window in which
    // drain_stray_channels() (driving thread) is between its m_chan_mtx-
    // protected swap of m_stray_channels and the end of its subsequent
    // Begin/Add/ExecuteBatchDestroyChannels loop against m_ctrl. Neither
    // m_phase nor m_stray_channels alone can express "busy" for that window
    // -- by the time the SDK loop runs, the swap has already emptied the
    // member queue, and m_phase can independently already read Done -- so
    // has_pending_work() needs this as a third, explicit signal. See its
    // doc comment above for why session_start()'s R1 mutual-exclusion gate
    // depends on has_pending_work() being right about this. atomic<bool>,
    // not m_chan_mtx: this flag is read by has_pending_work() while
    // m_chan_mtx may or may not be held by the caller (session_start() does
    // not hold it), and it is set/cleared around SDK calls that must never
    // run under that mutex -- folding this into m_chan_mtx would mean
    // either holding the mutex across the SDK loop (forbidden) or leaving a
    // gap between unlocking and setting/clearing this flag, which is the
    // exact class of gap this flag exists to close. No `mutable` needed:
    // std::atomic<bool>::load() is already const.
    std::atomic<bool> m_driving_thread_in_sdk_call{false};

    std::string  m_participant_name;
    unsigned int m_participant_id = 0;
    uint64_t     m_tone_index = 0;
    uint32_t     m_buffers_sent = 0;

    // Deadline for whichever of AwaitingChannel / AwaitingInvite is
    // currently active (only one is ever active at a time, so one field
    // suffices). An SDK call that returns SDKERR_SUCCESS is only a promise
    // that the call was accepted, not that the matching callback will ever
    // fire; without a deadline a swallowed callback hangs the probe forever
    // and reports nothing, which is silence -- the exact failure mode this
    // class exists to make visible instead of enduring.
    //
    // atomic (F5 review-round fix): this is genuinely cross-thread, and not
    // covered by m_phase's release/acquire the way it first looks. The
    // write in onCreateChannelResponse happens while phase still reads
    // AwaitingChannel -- BEFORE the release-store that advances it to
    // AwaitingInvite -- and tick(), running concurrently on the driving
    // thread, can read this same field for its AwaitingChannel timeout
    // check at that exact moment. That is a plain, unsynchronized
    // concurrent read/write of the same non-atomic memory from two threads:
    // undefined behaviour, not merely a stale value, and it is on the
    // expected path (a create_channel_response arriving while tick() is
    // mid-timeout-check is ordinary timing, not a rare interleaving).
    // Stored as the steady_clock rep (an integer) rather than the
    // time_point itself, since time_point is not trivially atomic-friendly
    // across implementations; reconstructed with
    // steady_clock::time_point(steady_clock::duration(rep)) at each read.
    // Deliberately not folded into m_chan_mtx: that mutex guards the
    // channel-id/stray-queue string state specifically, and this field has
    // nothing to do with it.
    std::atomic<std::chrono::steady_clock::rep> m_phase_deadline{0};

    // How many times BeginBatchDestroyChannels/AddChannelToDestroy/
    // ExecuteBatchDestroyChannels has been attempted for the current
    // channel. Reset to 0 at the start of every probe().
    uint32_t m_destroy_attempts = 0;

    // ── Talkback audio path (Milestone 2) ──────────────────────────────────
    // The plugin CREATES this region and writes it; we open it read-write
    // because a reader must be able to clear the notify flag. See
    // src/talkback-ring.h for why the roles are reversed here.
    ShmRegion   m_audio_region{};
    std::string m_audio_region_name;
    uint32_t    m_audio_read_index = 0;
    uint32_t    m_audio_rate       = 0;
    uint16_t    m_audio_channels   = 0;
    bool        m_audio_open       = false;

    // F8 review-round fix: counts audio_send report emissions in
    // drain_audio() so it can report the first occurrence and then only
    // periodically, never once per drain. Without this, a stale
    // m_channel_id_z (fixed elsewhere in this round -- see the destroy
    // paths in tick()) made every buffer fail and every drain_audio() call
    // report it: ~50-100 pipe lines/sec, the message-storm shape this
    // codebase already has a live incident about. Reset whenever a fresh
    // region is opened so each session gets its own "first occurrence".
    uint32_t    m_audio_send_fail_count = 0;

    // ── Persistent talkback session (Milestone 5) ──────────────────────────
    // Exactly one CreateChannel may be outstanding across the probe and the
    // session; see src/talkback-channel-owner.h for why.
    //
    // Guarded by m_chan_mtx -- NOT command-loop-thread-only, despite an
    // earlier version of this comment claiming otherwise. Every WRITER but
    // one is the command-loop thread: probe() and session_start() (both
    // claim it before CreateChannel), and onCreateChannelResponse (which
    // clears it) -- that callback is safe on the command-loop thread for the
    // same reason open_audio/drain_audio/close_audio are (see the THREADING
    // comment above the audio path below): on Windows this engine's main
    // loop is ALSO the SDK's message-pump thread, so every SDK callback,
    // this one included, runs there, not on some SDK-internal thread. The
    // exception is tick()'s AwaitingChannel-timeout clear (review-round R3
    // fix, see tick()): that one genuinely runs on the probe's OWN separate
    // driving thread, so this field needs the same cross-thread protection
    // as the channel-id strings below rather than being lock-free. Never
    // call the SDK while holding m_chan_mtx for this field either -- same
    // discipline as everywhere else in this class.
    TalkbackChannelOwner       m_pending_create = TalkbackChannelOwner::None;
    std::basic_string<zchar_t> m_session_channel_z;   // guarded by m_chan_mtx
    std::string                m_session_channel;     // UTF-8, reporting only
    std::string                m_session_participant; // by NAME, re-resolved
    unsigned int               m_session_user_id = 0;
    bool                       m_session_live    = false;
};
