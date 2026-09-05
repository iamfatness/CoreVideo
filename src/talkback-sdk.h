#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Every SDK call's outcome, normalised. The two platforms disagree on the
// numbers (Windows SDKERR_TOO_FREQUENT_CALL is enum position 18; macOS spells
// it ZoomSDKError_TooFrequentCall) and the ladder's backoff turns on this
// distinction, so the mapping belongs in the adapter and nowhere else.
enum class TalkbackResult {
    Ok,
    TooFrequent,     // Law 2: back off and retry the SAME item.
    AlreadyExists,   // Treated as confirmed presence, never retried.
    NoPermission,
    NotExist,
    Rejected,
    Timeout,
    Unknown,
};

// Callbacks, in the shape the ladder already consumes them.
class TalkbackSdkEvents {
public:
    virtual ~TalkbackSdkEvents() = default;
    virtual void on_create_channel_response(const std::string &channel_id,
                                            TalkbackResult result) = 0;
    virtual void on_destroy_channel_response(const std::string &channel_id,
                                             TalkbackResult result) = 0;
    virtual void on_channel_user_join_response(const std::string &channel_id,
                                               uint32_t user_id,
                                               TalkbackResult result) = 0;
    virtual void on_channel_user_leave_response(const std::string &channel_id,
                                                uint32_t user_id,
                                                TalkbackResult result) = 0;
};

// The operations the ladder needs, stated SEMANTICALLY. invite_users() and
// destroy_channels() take whole lists because that is what the operation means;
// whether the backend spells it as one atomic call (macOS) or a
// Begin/Add/Execute sequence (Windows) is the adapter's business. Hiding that
// is the point: the Begin/Add/Execute mutual-exclusion rules that produced the
// M2 Major have no analogue on macOS and should not be visible above this line.
class TalkbackSdk {
public:
    virtual ~TalkbackSdk() = default;
    virtual bool is_meeting_support_talkback() = 0;
    virtual TalkbackResult create_channel(uint32_t count) = 0;
    virtual TalkbackResult invite_users(const std::string &channel_id,
                                        const std::vector<uint32_t> &user_ids) = 0;
    virtual TalkbackResult destroy_channels(
        const std::vector<std::string> &channel_ids) = 0;
    virtual TalkbackResult send_audio(const std::string &channel_id,
                                      const char *data, uint32_t len,
                                      uint32_t sample_rate, bool stereo) = 0;
    virtual TalkbackResult set_background_volume(const std::string &channel_id,
                                                 float volume) = 0;
    virtual void set_events(TalkbackSdkEvents *events) = 0;

    // Fix round 1 (Findings 2 & 3). Neither of these is a ladder DECISION --
    // TalkbackResult is still the only thing the ladder's own logic compares
    // against, everywhere -- they are what the ladder's REPORT lines need.
    //
    // last_raw_code(): the platform's own error code from whichever seam
    // call (an operation above, or event registration below) most recently
    // returned. CLAUDE.md documents operators and post-mortems reading these
    // specific Windows SDKError numbers verbatim out of the E2P log (e.g.
    // "stage":"invite",...,"code":2 -- SDKERR_WRONG_USAGE, a talent in a
    // different breakout room -- and code 3, and code 18 for the rate
    // limit); reporting TalkbackResult's own numbering there instead would
    // make every one of those diagnostics silently wrong, even though the
    // ladder's retry/abort decisions (which DO use TalkbackResult) would
    // still be correct. So the two never collapse into one value: the
    // adapter maps its raw code to a TalkbackResult for the ladder AND keeps
    // the raw code available, separately, for the report line that follows.
    virtual int last_raw_code() const = 0;
    // Whether the most recent attempt to register this object's event sink
    // succeeded (SetEvent, on Windows). Registration itself is NOT one of
    // this seam's operations -- it is platform-specific in a way none of the
    // operations above are (see engine-talkback-sdk-win.h's own comment) --
    // but probe() must still be able to refuse when it fails, exactly as it
    // always could, so that one bit crosses here.
    virtual bool events_registered() const = 0;
};
