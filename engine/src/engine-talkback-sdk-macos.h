#pragma once
#include "talkback-sdk.h"
#include <memory>

// Opaque so C++ translation units can hold one without importing ZoomSDK --
// only engine-talkback-sdk-macos.mm (this class's own implementation file)
// ever sees a real ZoomSDKTalkbackController*.
class TalkbackMacSdk : public TalkbackSdk {
public:
    TalkbackMacSdk();
    ~TalkbackMacSdk() override;

    // Rebound whenever the meeting service hands us a controller. Mirrors the
    // Windows engine's guard: never reassign while a session is live, because a
    // roster event can fire mid-press and a stray nil would null the controller
    // for the rest of that press. `zoom_talkback_controller` is really a
    // ZoomSDKTalkbackController* smuggled through as void* so this header never
    // has to import ZoomSDK; the caller (a future task's macOS main.cpp) owns
    // that lifetime, this class does not retain it.
    void bind(void *zoom_talkback_controller);

    bool is_meeting_support_talkback() override;
    TalkbackResult create_channel(uint32_t count) override;
    TalkbackResult invite_users(const std::string &channel_id,
                                const std::vector<uint32_t> &user_ids) override;
    TalkbackResult destroy_channels(
        const std::vector<std::string> &channel_ids) override;
    TalkbackResult send_audio(const std::string &channel_id, const char *data,
                              uint32_t len, uint32_t sample_rate,
                              bool stereo) override;
    TalkbackResult set_background_volume(const std::string &channel_id,
                                         float volume) override;
    void set_events(TalkbackSdkEvents *events) override;

    // Task 1's real TalkbackSdk (src/talkback-sdk.h) is not the brief's Step 1
    // draft for this class -- it also declares these two pure virtuals (raw
    // platform error code + event-registration status, both for the ladder's
    // REPORT lines, never its decisions -- see the base class's own doc
    // comment). They are implemented here so this class is concrete at all;
    // see the .mm file for what "registered" means on a platform where
    // registering the delegate has no failure-carrying return code.
    int last_raw_code() const override;
    bool events_registered() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
