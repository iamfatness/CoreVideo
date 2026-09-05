#pragma once
#include "talkback-sdk.h"   // TalkbackResult
#include <cstdint>
#include <string>
#include <vector>

// The macOS-port seam's second half (Task 2b, 2026-09-05). TalkbackSdk
// (talkback-sdk.h) normalised the channel-management OPERATIONS
// (create/invite/destroy/send/volume/is_supported); this one normalises the
// remaining coupling task-2b-brief.md measured as four concerns once the
// operations were already gone -- the ZOOMSDK::IMeetingService* parameter and
// member, the roster walk (engine-talkback.cpp's resolve_participant()/
// current_roster()), and the mic control (ensure_mic_open()/
// restore_mic_state()). Without this seam too, EngineTalkback still directly
// implements ZOOMSDK::IMeetingTalkbackCtrlEvent-adjacent coupling through
// IMeetingService*, and the macOS engine target cannot compile it: there is
// no zoom_sdk.h, no IMeetingService, nothing under ZOOMSDK:: on that
// platform's pure-Objective-C SDK.

struct TalkbackParticipant {
    uint32_t    user_id = 0;
    std::string display_name;
    // Divergence from the brief's sketch: resolve_participant()
    // (engine-talkback.cpp) also needs the per-user IsSupportTalkback() gate
    // for its "participant_talkback_support" report line -- not a ladder
    // DECISION, but a report field the brief's roster() sketch had no room
    // for. Added here rather than a second host call, mirroring
    // TalkbackParticipant's own "no SDK type in sight" rule; the field is
    // read-only data about the participant, exactly like user_id/display_name.
    bool        supports_talkback = false;
};

// The meeting-side facts the talkback ladder needs, with no SDK type in sight.
// Deliberately NOT a general meeting abstraction -- only what engine-talkback.cpp
// actually reaches for today. Anything wider is speculation.
class TalkbackHost {
public:
    virtual ~TalkbackHost() = default;

    // Everyone currently in the meeting. Empty is a legitimate answer, and is
    // distinct from a failure to ask -- the ladder already treats an empty
    // roster as "nobody to invite", never as an error.
    virtual std::vector<TalkbackParticipant> roster() = 0;

    // This client. False when the SDK cannot answer, which the ladder must not
    // confuse with "muted" -- Law 1's whole point is that an unknown mic state
    // is not a safe one.
    virtual bool myself(TalkbackParticipant &out) = 0;

    // Law 1: IMeetingAudioController has no "am I muted" getter; the
    // authoritative state is GetMySelfUser()->IsAudioMuted(). Kept as its own
    // question so the adapter owns that quirk rather than the ladder.
    //
    // HAZARD (from this task's brief, stated so it stays checkable): an
    // unknown mic state must never read as "not muted" here. Every adapter
    // implementing this fails CLOSED -- returns true (muted) -- whenever the
    // participants controller or the self user cannot be resolved, so
    // ensure_mic_open() still attempts the unmute (and reports whatever THAT
    // fails with) instead of silently believing a mic it could not actually
    // read is already open.
    virtual bool is_self_muted() = 0;

    // Returns the normalised result; the raw SDK code stays available through
    // the same last_raw_code() convention TalkbackSdk uses, because the emitted
    // "code" fields on the wire must remain raw SDK numbers.
    virtual TalkbackResult set_self_muted(bool muted) = 0;

    // Divergence from the brief's sketch: the sketch's own comment on
    // set_self_muted() (above, carried forward verbatim) invokes "the same
    // last_raw_code() convention TalkbackSdk uses" without this interface
    // actually declaring one -- TalkbackSdk's version is a method on THAT
    // class, and nothing here exposed the equivalent. Added to make the
    // comment true: the platform's own raw error code from the most recent
    // set_self_muted() call, kept only for the ladder's report lines (never
    // its decisions, which stay on the TalkbackResult that call returned).
    // Mirrors TalkbackSdk::no_controller()'s convention too: an adapter that
    // cannot even attempt the call (no controller resolvable) returns
    // TalkbackResult::NotExist from set_self_muted() and stores the platform's
    // own "no genuine answer" sentinel here, so a caller can tell "there was
    // nothing to call" from "the call failed" without a second signal.
    virtual int last_raw_code() const = 0;
};
