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
#include <string>

class EngineTalkback : public ZOOMSDK::IMeetingTalkbackCtrlEvent {
public:
    // Starts the probe ladder. Reports and returns without blocking; the
    // asynchronous rungs continue through the callbacks below and tick().
    void probe(ZOOMSDK::IMeetingService *svc, const std::string &participant_name);

    // Called from the engine's main loop. Sends tone buffers while a send is
    // in progress, then destroys the channel.
    void tick();

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

    ZOOMSDK::IMeetingService          *m_svc  = nullptr;
    ZOOMSDK::IMeetingTalkbackController *m_ctrl = nullptr;

    // m_phase is written from SDK callback threads (onCreateChannelResponse,
    // onChannelUserJoinResponse) and, once Task 5 wires tick() to the engine
    // main loop, read/written from the engine thread too -- atomic with
    // explicit acquire/release is the cheap fix, done now while this file is
    // in front of us rather than left as a landmine for that task.
    //
    // No mutex needed for m_channel_id_z either: it is always fully written
    // in onCreateChannelResponse BEFORE m_phase is released to AwaitingInvite
    // (and later Sending), and every reader of m_channel_id_z first acquires
    // m_phase and checks it is at least AwaitingInvite/Sending. The
    // release-store of m_phase after the write, paired with the acquire-load
    // before every read, is the synchronizes-with edge that makes the plain
    // std::string write visible -- that ordering, not a lock, is what makes
    // this safe.
    std::atomic<Phase> m_phase{Phase::Idle};

    std::string  m_channel_id;      // UTF-8, REPORTING ONLY -- never pass to
                                     // the SDK, see m_channel_id_z below.
    // zchar_t is wchar_t on Windows (zoom_sdk_def.h) but char elsewhere, so
    // basic_string<zchar_t> is the only type that is simultaneously correct
    // on both platforms and round-trip-safe for an opaque SDK identifier
    // (no UTF-8 re-encoding). Every SDK call that takes a channel ID must
    // use m_channel_id_z.c_str(), never m_channel_id.c_str().
    std::basic_string<zchar_t> m_channel_id_z;
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
