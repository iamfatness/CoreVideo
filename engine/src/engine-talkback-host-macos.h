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
    // class does not retain it. Wraps the MEETING SERVICE itself, not a single
    // controller -- unlike TalkbackMacSdk (which wraps ZoomSDKTalkbackController
    // directly, because every one of its operations targets that one
    // controller), this class's methods need TWO different controllers off the
    // same service (getMeetingActionController() for the roster/self lookup
    // AND the mute action), mirroring TalkbackWinHost's own reason for
    // wrapping ZOOMSDK::IMeetingService* rather than one sub-controller.
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
