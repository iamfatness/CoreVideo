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
};
