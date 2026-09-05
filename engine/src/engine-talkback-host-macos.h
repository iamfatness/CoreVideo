#pragma once
#include "talkback-host.h"
#include <memory>

// macOS adapter for TalkbackHost (macOS talkback port Task 2b, 2026-09-05).
// Mirrors TalkbackMacSdk's own shape (engine-talkback-sdk-macos.h): opaque so
// C++ translation units can hold one without importing ZoomSDK, bound lazily
// via bind() rather than at construction.
//
// NOT WIRED INTO main-macos.mm BY THIS TASK. This task's deliverable is
// getting engine-talkback.cpp (and this file) to COMPILE into the macOS
// ZoomObsEngine target -- see the CMakeLists.txt comment this task replaces.
// EngineTalkback is not constructed anywhere in main-macos.mm yet, so there is
// nothing on macOS for this class to be injected INTO; wiring main-macos.mm's
// command loop to actually drive EngineTalkback (mirroring main.cpp's
// inject_talkback_sdk/inject_talkback_host lambdas and the probe/session/
// nominate call sites) is a later task's job, exactly as Task 2 left
// TalkbackMacSdk unwired for the same reason.
class TalkbackMacHost : public TalkbackHost {
public:
    TalkbackMacHost();
    ~TalkbackMacHost() override;

    // Rebound whenever the meeting service hands us one. `zoom_meeting_service`
    // is really a ZoomSDKMeetingService* smuggled through as void* so this
    // header never has to import ZoomSDK; the caller owns that lifetime, this
    // class does not retain it. Wraps the MEETING SERVICE itself, not the
    // controller directly -- unlike TalkbackMacSdk (which wraps
    // ZoomSDKTalkbackController directly, because every one of its operations
    // targets that one controller). Fix round (review, Minor): this used to
    // say the reason was needing TWO different controllers off the service,
    // mirroring TalkbackWinHost -- wrong, and contradicted by this class's own
    // .mm: the macOS SDK has no split at all, one ZoomSDKMeetingActionController
    // (fetched via getMeetingActionController(), see the .mm's own divergence
    // note) owns BOTH the roster/self lookup and the mute action. The real
    // reason to wrap the service rather than the controller is simpler --
    // getMeetingActionController() can itself return nil at different times
    // (e.g. between meetings), so resolving it lazily on every call, the same
    // way ctrl() does in the .mm, is what lets one TalkbackMacHost instance
    // outlive a single meeting the way TalkbackWinHost does.
    void bind(void *zoom_meeting_service);

    std::vector<TalkbackParticipant> roster() override;
    bool myself(TalkbackParticipant &out) override;
    bool is_self_muted() override;
    TalkbackResult set_self_muted(bool muted) override;
    int last_raw_code() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
