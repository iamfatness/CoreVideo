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
    void probe(ZOOMSDK::IMeetingService *svc, const std::string &participant_name);

    // Called from the engine's main loop. Sends tone buffers while a send is
    // in progress, then destroys the channel.
    void tick();

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

    void report(const std::string &stage, const std::string &fields);
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
    std::mutex m_chan_mtx;
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
    std::chrono::steady_clock::time_point m_phase_deadline{};

    // How many times BeginBatchDestroyChannels/AddChannelToDestroy/
    // ExecuteBatchDestroyChannels has been attempted for the current
    // channel. Reset to 0 at the start of every probe().
    uint32_t m_destroy_attempts = 0;
};
