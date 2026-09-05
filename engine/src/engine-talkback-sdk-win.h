#pragma once
#include "talkback-sdk.h"
#include "engine-json.h"     // zchar_to_utf8() -- Task 2b's event forwarding
#include "meeting_service_components/meeting_talkback_ctrl_interface.h"

// Fix round 2: WIN32 is a CMake-supplied compile definition (this project's
// own targets set it explicitly), not a compiler-defined one -- MSVC defines
// _WIN32. This header had only ever been reached with WIN32 already true
// because windows.h arrives transitively through engine-talkback.h's own
// include chain before this file is ever included; testing the wrong macro
// here was silently harmless, not silently correct.
#if defined(_WIN32)
#include <windows.h>
#endif

// SDKERR_TOO_FREQUENT_CALL is enum POSITION 18 in the Windows SDKError enum;
// referenced by name, never by the literal, because a header revision that
// inserts a value ahead of it would silently retarget the backoff.
inline TalkbackResult talkback_win_result(ZOOMSDK::SDKError e)
{
    switch (e) {
    case ZOOMSDK::SDKERR_SUCCESS:            return TalkbackResult::Ok;
    case ZOOMSDK::SDKERR_TOO_FREQUENT_CALL:  return TalkbackResult::TooFrequent;
    case ZOOMSDK::SDKERR_NO_PERMISSION:      return TalkbackResult::NoPermission;
    default:                                 return TalkbackResult::Unknown;
    }
}

// Task 2b (2026-09-05): the EVENT-side counterpart of talkback_win_result()
// above -- maps TalkbackError (meeting_talkback_ctrl_interface.h's async
// callback error type, NOT ZOOMSDK::SDKError) onto this seam's TalkbackResult.
//
// TalkbackError IS NESTED INSIDE ZOOMSDK::IMeetingTalkbackCtrlEvent, not at
// namespace or global scope (confirmed by tests/engine-talkback-select-test.cpp's
// own top-of-file comment: "TalkbackError is nested INSIDE
// IMeetingTalkbackCtrlEvent, which is why engine-talkback.h's overrides take
// it unqualified -- they are members of a subclass"). engine-talkback.cpp's
// pre-Task-2b code and this file's own onCreateChannelResponse()/etc overrides
// above could reference it bare for exactly that reason: unqualified lookup of
// a base class's nested type resolves inside a derived class's own member
// declarations. A FREE function has no such derived-class scope, so this one
// -- unlike the methods above -- must qualify it fully.
//
// DIVERGENCE FROM WHAT COULD BE VERIFIED HERE: only TALKBACK_ERROR_OK and
// TALKBACK_ERROR_ALREADY_EXIST are confirmed real (both appeared, qualified
// only by the enclosing class exactly as here, in the pre-Task-2b
// engine-talkback.cpp and in tests/engine-talkback-select-test.cpp, both of
// which shipped and passed Windows CI). This machine has no Windows Zoom SDK
// headers to check the remaining enumerators against
// (TALKBACK_ERROR_NOPERMISSION/REJECTED are named only in this codebase's own
// COMMENTS, never in code, and this task cannot build Windows to confirm the
// rest at all). The ladder does not need finer resolution than this to make
// its OWN decisions -- engine-talkback.cpp's event handlers only ever compare
// a TalkbackResult against Ok or AlreadyExists, everything else is "not that"
// -- so every other TalkbackError value maps to Unknown here, and the
// human-diagnostic value survives anyway through `raw_code` (threaded
// separately into every on_*_response() call), never through TalkbackResult's
// own numbering. If a future task narrows this further, verify the enumerator
// names against the real header first.
inline TalkbackResult talkback_win_tb_result(ZOOMSDK::IMeetingTalkbackCtrlEvent::TalkbackError e)
{
    switch (e) {
    case ZOOMSDK::IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_OK:
        return TalkbackResult::Ok;
    case ZOOMSDK::IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_ALREADY_EXIST:
        return TalkbackResult::AlreadyExists;
    default:
        return TalkbackResult::Unknown;
    }
}

// The reverse of engine-json.h's zchar_to_utf8(): every SDK call on
// IMeetingTalkbackController takes a channel id as `const zchar_t*`
// (wchar_t* on Windows), but the seam's currency is UTF-8 (src/talkback-sdk.h
// -- picked so the interface itself needs no Zoom type). Channel ids are
// Zoom-generated opaque tokens, never operator-entered text, so this only
// ever needs to round-trip ASCII in practice, but it goes through the real
// Windows conversion API rather than assuming that.
inline std::basic_string<zchar_t> talkback_utf8_to_zchar(const std::string &s)
{
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::basic_string<zchar_t> out(static_cast<size_t>(len - 1), L'\0');
    if (!out.empty())
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], len);
    return out;
}

// Task 2b (2026-09-05): this class now ALSO implements
// ZOOMSDK::IMeetingTalkbackCtrlEvent directly -- EngineTalkback used to (it
// implemented the real SDK interface and received Windows callbacks
// natively), but Task 2b retargeted it onto TalkbackSdkEvents (talkback-sdk.h)
// so the macOS engine target could compile it too, and something concrete
// still has to receive the SDK's native callbacks on Windows. This adapter is
// the natural place: it already holds the real
// ZOOMSDK::IMeetingTalkbackController to register against, and its job is
// already translating this seam's currency (UTF-8 strings, TalkbackResult) to
// and from the raw SDK types for every OPERATION -- the events are the same
// translation, in the other direction.
class TalkbackWinSdk : public TalkbackSdk, public ZOOMSDK::IMeetingTalkbackCtrlEvent {
public:
    explicit TalkbackWinSdk(ZOOMSDK::IMeetingTalkbackController *ctrl)
        : m_ctrl(ctrl) {}

    // Fix round (review, Important 2): the ONLY way to change which
    // controller this adapter wraps, once constructed. main.cpp used to
    // copy-assign a whole fresh `TalkbackWinSdk(ctrl)` over the static
    // instance on every injection -- which, now that this class also IS the
    // SDK's registered ZOOMSDK::IMeetingTalkbackCtrlEvent (Task 2b), is a
    // live object the SDK can be calling back into at that exact moment.
    // Copy-assignment nulled m_events for the instant between the assignment
    // and the following set_events() call (a callback landing there hit
    // `if (m_events)` and was silently dropped -- and a dropped
    // onCreateChannelResponse for an untracked channel never reaches
    // m_stray_channels, leaking a Zoom channel against the 16-channel budget
    // with no diagnostic, precisely in the Idle/Done injection window the
    // stray-channel machinery exists to handle) and was an unsynchronised
    // whole-object overwrite of m_ctrl/m_events/m_last_raw_code racing
    // callback threads reading those same fields. rebind() only ever touches
    // m_ctrl and m_events_registered -- m_events is set once (main.cpp calls
    // set_events() a single time, not on every injection) and is never
    // touched here, so there is no window where it reads null while a
    // callback could still be in flight for the object whose address never
    // moves. What remains is a single pointer store, the same shape the
    // pre-Task-2b code had for m_ctrl before this class had any events of its
    // own to protect.
    void rebind(ZOOMSDK::IMeetingTalkbackController *ctrl)
    {
        m_ctrl = ctrl;
        m_events_registered = false;
    }

    bool is_meeting_support_talkback() override
    {
        return m_ctrl && m_ctrl->IsMeetingSupportTalkBack();
    }

    TalkbackResult create_channel(uint32_t count) override
    {
        if (!m_ctrl) return no_controller();
        return map(m_ctrl->CreateChannel(count));
    }

    // The Begin/Add/Execute sequence lives HERE and nowhere above. On this
    // platform an invite is three calls that must not interleave with another
    // batch; on macOS it is one call. The ladder is entitled to know neither.
    //
    // Fix round 2 (Important 3): chained exactly as every pre-Task-1 call
    // site did (d7d41f2 engine-talkback.cpp:1528-1530, 2623-2625) -- each
    // call runs ONLY if the previous one succeeded, and the reported code is
    // whichever call FIRST failed. Two deltas the un-chained version had
    // introduced: a failed Begin no longer aborted the sequence (Add and
    // Execute still ran, driving the batch API into a state the old code
    // never produced -- and a failing Begin is the M2 Major's own subject);
    // and only Execute's code ever reached the ladder's report, when
    // AddUserToInvite is the only call that carries the user id -- the only
    // one that can mean "this specific talent is in a different breakout
    // room" (CLAUDE.md's own documented `"stage":"invite",...,"code":2`).
    TalkbackResult invite_users(const std::string &channel_id,
                                const std::vector<uint32_t> &user_ids) override
    {
        if (!m_ctrl) return no_controller();
        const std::basic_string<zchar_t> id_z = talkback_utf8_to_zchar(channel_id);
        ZOOMSDK::SDKError e = m_ctrl->BeginBatchInviteUsers(id_z.c_str());
        for (uint32_t id : user_ids)
            if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->AddUserToInvite(id);
        if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->ExecuteBatchInviteUsers();
        return map(e);
    }

    // Fix round 2 (Important 3): same chaining as invite_users() above, and
    // for the same reason -- see that method's comment. Preserves tick()'s
    // empty-list case bit for bit: `channel_ids` empty means the loop below
    // never runs, so Begin and (if it succeeded) Execute are still issued
    // with nothing added between them, exactly as the old
    // "e == SDKERR_SUCCESS && !channel_copy.empty()" guard around
    // AddChannelToDestroy alone produced.
    TalkbackResult destroy_channels(
        const std::vector<std::string> &channel_ids) override
    {
        if (!m_ctrl) return no_controller();
        // Every id's zchar_t form must outlive AddChannelToDestroy() but need
        // not outlive this whole function -- collected up front only so the
        // loop below is not re-deriving the same conversion issue site by
        // site.
        std::vector<std::basic_string<zchar_t> > ids_z;
        ids_z.reserve(channel_ids.size());
        for (const auto &id : channel_ids) ids_z.push_back(talkback_utf8_to_zchar(id));
        ZOOMSDK::SDKError e = m_ctrl->BeginBatchDestroyChannels();
        for (const auto &id_z : ids_z)
            if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->AddChannelToDestroy(id_z.c_str());
        if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->ExecuteBatchDestroyChannels();
        return map(e);
    }

    TalkbackResult send_audio(const std::string &channel_id, const char *data,
                              uint32_t len, uint32_t sample_rate,
                              bool stereo) override
    {
        if (!m_ctrl) return no_controller();
        const std::basic_string<zchar_t> id_z = talkback_utf8_to_zchar(channel_id);
        return map(m_ctrl->SendAudioDataToChannel(
            id_z.c_str(), data, len, sample_rate,
            stereo ? ZOOMSDK::ZoomSDKAudioChannel_Stereo
                   : ZOOMSDK::ZoomSDKAudioChannel_Mono));
    }

    TalkbackResult set_background_volume(const std::string &channel_id,
                                         float volume) override
    {
        if (!m_ctrl) return no_controller();
        const std::basic_string<zchar_t> id_z = talkback_utf8_to_zchar(channel_id);
        return map(m_ctrl->SetChannelBackgroundVolume(id_z.c_str(), volume));
    }

    void set_events(TalkbackSdkEvents *events) override { m_events = events; }

    // Fix round 1 (Finding 3): every operation above stores its own raw
    // ZOOMSDK::SDKError here (via map()), for the ladder's REPORT lines --
    // never for its decisions, which stay on the TalkbackResult each
    // operation returns. See the doc comment on the base class declaration.
    int last_raw_code() const override { return m_last_raw_code; }
    bool events_registered() const override { return m_events_registered; }

    // Fix round 1 (Finding 2): NOT one of TalkbackSdk's operations -- a
    // portable `TalkbackSdk*` cannot be handed this call at all, only code
    // that already holds a concrete TalkbackWinSdk can make it, which is
    // main.cpp, at the same point it constructs this adapter. Records both
    // last_raw_code() and events_registered() exactly as every other call
    // here does, so probe() can refuse on a failed registration precisely as
    // it did before this seam existed.
    //
    // Task 2b (2026-09-05): registers ITSELF (`this`), not an external event
    // object -- see the class comment for why. set_events() (TalkbackSdk's
    // own interface, above) is the separate call that wires the FORWARDING
    // target; main.cpp calls both, in either order, before the first
    // probe()/nominate()/session_start().
    void register_event()
    {
        if (!m_ctrl) { no_controller(); m_events_registered = false; return; }
        const ZOOMSDK::SDKError e = m_ctrl->SetEvent(this);
        m_last_raw_code = static_cast<int>(e);
        m_events_registered = (e == ZOOMSDK::SDKERR_SUCCESS);
    }

    // ZOOMSDK::IMeetingTalkbackCtrlEvent (Task 2b): forwards every callback
    // into whichever TalkbackSdkEvents* set_events() last set, translating
    // the raw wchar_t* channel id and native TalkbackError the same way every
    // OPERATION above already translates its own arguments/return value --
    // UTF-8 in, TalkbackResult out, with the platform's own raw code
    // threaded through separately (`raw_code`) so the ladder's report lines
    // keep carrying real SDK numbers even though TalkbackResult itself never
    // does. `m_events` null-checked exactly like every other optional
    // forwarding target in this codebase (e.g. CVTalkbackDelegate's own
    // `if (_events)` guard on macOS) -- a callback that arrives before
    // set_events() or after teardown is a no-op, not a crash.
    void onCreateChannelResponse(const zchar_t *channelID, TalkbackError error) override
    {
        if (m_events) m_events->on_create_channel_response(
            zchar_to_utf8(channelID), talkback_win_tb_result(error),
            static_cast<int>(error));
    }
    void onDestroyChannelResponse(const zchar_t *channelID, TalkbackError error) override
    {
        if (m_events) m_events->on_destroy_channel_response(
            zchar_to_utf8(channelID), talkback_win_tb_result(error),
            static_cast<int>(error));
    }
    void onChannelUserJoinResponse(const zchar_t *channelID, unsigned int userID,
                                   TalkbackError error) override
    {
        if (m_events) m_events->on_channel_user_join_response(
            zchar_to_utf8(channelID), userID, talkback_win_tb_result(error),
            static_cast<int>(error));
    }
    void onChannelUserLeaveResponse(const zchar_t *channelID, unsigned int userID,
                                    TalkbackError error) override
    {
        if (m_events) m_events->on_channel_user_leave_response(
            zchar_to_utf8(channelID), userID, talkback_win_tb_result(error),
            static_cast<int>(error));
    }
    // Receive-side: this engine is the DIRECTOR, never talent -- stubbed for
    // the same reason CVTalkbackDelegate's macOS equivalents are (that file's
    // own comment: the safe superset costs nothing). TalkbackSdkEvents has no
    // equivalent of these three at all (they were never part of the ladder's
    // own logic; EngineTalkback's pre-Task-2b versions were empty bodies
    // too), so there is nothing to forward them to.
    void onJoinTalkbackChannel(unsigned int) override {}
    void onLeaveTalkbackChannel(unsigned int) override {}
    void onInviterAudioLevel(unsigned int, unsigned int) override {}

private:
    TalkbackResult map(ZOOMSDK::SDKError e)
    {
        m_last_raw_code = static_cast<int>(e);
        return talkback_win_result(e);
    }
    // No real controller to call at all -- stores a raw code for the report
    // line the caller still emits (SDKERR_UNKNOWN: no genuine SDK answer
    // exists for a call that was never made) and returns the one
    // TalkbackResult that already means "there is nothing here."
    TalkbackResult no_controller()
    {
        m_last_raw_code = static_cast<int>(ZOOMSDK::SDKERR_UNKNOWN);
        return TalkbackResult::NotExist;
    }

    ZOOMSDK::IMeetingTalkbackController *m_ctrl = nullptr;
    TalkbackSdkEvents *m_events = nullptr;
    int m_last_raw_code = static_cast<int>(ZOOMSDK::SDKERR_UNKNOWN);
    bool m_events_registered = false;
};
