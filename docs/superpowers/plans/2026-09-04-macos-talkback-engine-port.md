# macOS Talkback Engine Port — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the macOS engine the talkback feature that today exists only in the Windows engine, without forking the ladder logic into a second implementation.

**Architecture:** Introduce one CoreVideo-owned abstract seam, `TalkbackSdk`, at the boundary where `engine-talkback.cpp` calls Zoom. The ladder, pacing, nomination state machine and abort paths stay as the single implementation they are today; only the type they call changes. Two backends implement the seam: a Windows adapter wrapping `ZOOMSDK::IMeetingTalkbackController` (which hides the Begin/Add/Execute batch sequences entirely), and a macOS Objective-C++ adapter wrapping `ZoomSDKTalkbackController` (whose equivalents are single atomic calls). The existing fakes move from subclassing Zoom's Windows interfaces to implementing `TalkbackSdk`, which is what makes the test suite build and run on macOS.

**Tech Stack:** C++17, Objective-C++ (`.mm`) for the macOS adapter, Zoom Meeting SDK (Windows `IMeetingTalkbackController` / macOS `ZoomSDKTalkbackController`), CMake, the project's plain-executable test convention (no framework).

## Global Constraints

- **Zero behaviour change on Windows.** Windows talkback works and has passed its live gate. A task that changes Windows *behaviour* rather than the type it calls is a defect, not progress. **This is verified by CI, not locally** — the development machine is an Apple Silicon Mac with no Windows toolchain, so "Windows still green" means the branch's Windows x64 PR check, the same gate that verified the #229 merge. Do not ask an implementer to run a Windows suite; ask it to keep the change mechanical and let CI answer.
- **`CreateChannel` and every membership call are SDK-thread-only.** On Windows that thread is the command loop. **On macOS it is the main queue** — the IPC reader is a separate thread that `dispatch_async`es to the main queue (`engine/src/main-macos.mm:2243`). This is the single most important difference in the port and the most likely source of a defect.
- **Law 2 pacing is server behaviour, not API shape.** One membership call per ~600 ms, round-robin, with backoff on a too-frequent refusal. It ports unchanged. Only the *error code* differs: Windows `SDKERR_TOO_FREQUENT_CALL` (enum position 18) vs macOS `ZoomSDKError_TooFrequentCall` (`ZoomSDKErrors.h:241`). The adapter normalises; the ladder must never see a raw SDK code.
- **No live-meeting dependency before Task 5.** Tasks 1-4 must be completable and verifiable with no network and no Zoom meeting. This is a hard sequencing requirement, not a preference — see Task 5's note on the `initSDKWithParams` wedge.
- **Never assert a branch unreachable.** Standing policy in `CLAUDE.md`; two Majors in this feature have lived behind exactly that claim.
- **Tests pin invariants, not implementations.** Every new pin must be mutation-proved: break the thing, watch the test fail, revert.

---

## File Structure

**Create:**
- `src/talkback-sdk.h` — the abstract seam plus the normalised result type. Pure, no Zoom headers, no Qt, no OBS. This is what makes the ladder and its tests portable.
- `engine/src/engine-talkback-sdk-win.h` — Windows adapter. Owns the Begin/Add/Execute batch sequences so nothing above it knows they exist.
- `engine/src/engine-talkback-sdk-macos.h` / `.mm` — macOS adapter over `ZoomSDKTalkbackController`, plus the delegate object that forwards callbacks into `TalkbackSdkEvents`.
- `engine/src/engine-talkback-pump-macos.h` / `.mm` — the main-queue timer that drives `nomination_tick()`, replacing Windows' command-loop idle turn.

**Modify:**
- `engine/src/engine-talkback.h` / `.cpp` — hold a `TalkbackSdk*` instead of a `ZOOMSDK::IMeetingTalkbackController*`; call normalised methods. Delete the batch bookkeeping that moves into the Windows adapter.
- `engine/src/main.cpp` — construct the Windows adapter, pass it in. No logic change.
- `engine/src/main-macos.mm` — construct the macOS adapter, wire the seven talkback IPC commands, start the pump.
- `CMakeLists.txt` — add the macOS talkback sources to the macOS engine target; make `CoreVideoEngineTalkbackSelectTest` build on all platforms.
- `tests/engine-talkback-select-test.cpp` — fakes implement `TalkbackSdk` rather than Zoom's Windows interfaces.
- `src/zoom-talkback-panel.cpp` — flip `kTalkbackPlatformSupported` (Task 5).
- `CLAUDE.md` — the macOS talkback section, replacing the "does not exist on macOS" entry.

**Deliberately not touched:** `src/talkback-plan.h`, `src/talkback-nomination.h`, `src/talkback-nomination-dispatch.h`, `src/talkback-key.h`, `src/talkback-ring.h`, `src/talkback-dock-state.h`, `src/zoom-talkback-panel.cpp` (beyond the flag). These are already cross-platform and already compile on macOS. **The port is the engine, not the feature.**

---

### Task 1: The `TalkbackSdk` seam, and the suite running on macOS

Introduces the abstraction, retargets the ladder and the fakes onto it, and un-gates the test target so it builds everywhere. Windows behaviour is unchanged throughout; the existing mutation-proved suite is the safety net.

**These were drafted as two tasks and merged, because neither is testable alone.** `engine-talkback.cpp` compiles nowhere on macOS today — it is Windows-gated in `ENGINE_SOURCES` (`CMakeLists.txt:398`, which the macOS engine target does not use) and at `CMakeLists.txt:1236`. So the refactor cannot be compiled or run locally until the CMake gate moves; and the gate cannot move until the refactor frees the fakes from Zoom's Windows headers. One deliverable: **the ladder and its suite build and pass on macOS.**

**Files:**
- Create: `src/talkback-sdk.h`
- Create: `engine/src/engine-talkback-sdk-win.h`
- Modify: `engine/src/engine-talkback.h`, `engine/src/engine-talkback.cpp`, `engine/src/main.cpp`, `CMakeLists.txt`
- Test: `tests/engine-talkback-select-test.cpp`

**Interfaces:**
- Produces: `TalkbackSdk`, `TalkbackSdkEvents`, `TalkbackResult`, `TalkbackWinSdk`.
- Consumes: nothing from earlier tasks.

- [ ] **Step 1: Write `src/talkback-sdk.h`**

Normalised results first. The ladder must never compare a raw SDK integer — the two platforms use different numbers for the same condition, and Law 2's backoff keys on exactly one of them.

```cpp
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
```

- [ ] **Step 2: Write the failing test for result normalisation**

Add to `tests/engine-talkback-select-test.cpp`. This pins the one mapping the ladder's correctness depends on.

```cpp
{
    // Law 2's signal must survive the seam and reach the LADDER's retry
    // decision. Asserting the fake returns what it was told is a tautology;
    // what matters is that a too-frequent create is retried rather than
    // reported as a terminal failure, which is the behaviour the whole backoff
    // exists for.
    FakeTalkbackSdk sdk;
    sdk.script_create_results({TalkbackResult::TooFrequent,
                               TalkbackResult::TooFrequent,
                               TalkbackResult::Ok});
    EngineTalkback tb;
    tb.set_sdk(&sdk);
    tb.nominate_for_test({"Sarah"});
    for (int i = 0; i < 8; ++i) {
        tb.debug_expire_membership_floor_for_test();
        tb.nomination_tick();
    }
    check(sdk.create_calls() == 3,
          "a too-frequent create was not retried -- Law 2 backoff is not "
          "reaching the ladder through the seam");
    check(!tb.last_report_contains("create_rate_limited"),
          "a create that eventually succeeded was reported as rate-limited");
}
```

- [ ] **Step 3: Run it and watch it fail**

Run: `cmake --build build --target CoreVideoEngineTalkbackSelectTest && ./build/CoreVideoEngineTalkbackSelectTest`
Expected: FAIL to compile — `TalkbackResult` and `FakeTalkbackSdk` do not exist.

- [ ] **Step 4: Write `engine/src/engine-talkback-sdk-win.h`**

The adapter owns the batch sequences. Map every Zoom error the ladder acts on; map everything else to `Unknown` rather than guessing.

```cpp
#pragma once
#include "talkback-sdk.h"
#include "meeting_service_components/meeting_talkback_ctrl_interface.h"

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

class TalkbackWinSdk : public TalkbackSdk {
public:
    explicit TalkbackWinSdk(ZOOMSDK::IMeetingTalkbackController *ctrl)
        : m_ctrl(ctrl) {}

    bool is_meeting_support_talkback() override
    {
        return m_ctrl && m_ctrl->IsMeetingSupportTalkBack();
    }

    TalkbackResult create_channel(uint32_t count) override
    {
        if (!m_ctrl) return TalkbackResult::NotExist;
        return talkback_win_result(m_ctrl->CreateChannel(count));
    }

    // The Begin/Add/Execute sequence lives HERE and nowhere above. On this
    // platform an invite is three calls that must not interleave with another
    // batch; on macOS it is one call. The ladder is entitled to know neither.
    TalkbackResult invite_users(const std::string &channel_id,
                                const std::vector<uint32_t> &user_ids) override
    {
        if (!m_ctrl) return TalkbackResult::NotExist;
        m_ctrl->BeginBatchInviteUsers(channel_id.c_str());
        for (uint32_t id : user_ids)
            m_ctrl->AddUserToInvite(id);
        return talkback_win_result(m_ctrl->ExecuteBatchInviteUsers());
    }

    TalkbackResult destroy_channels(
        const std::vector<std::string> &channel_ids) override
    {
        if (!m_ctrl) return TalkbackResult::NotExist;
        m_ctrl->BeginBatchDestroyChannels();
        for (const auto &id : channel_ids)
            m_ctrl->AddChannelToDestroy(id.c_str());
        return talkback_win_result(m_ctrl->ExecuteBatchDestroyChannels());
    }

    TalkbackResult send_audio(const std::string &channel_id, const char *data,
                              uint32_t len, uint32_t sample_rate,
                              bool stereo) override
    {
        if (!m_ctrl) return TalkbackResult::NotExist;
        return talkback_win_result(m_ctrl->SendAudioDataToChannel(
            channel_id.c_str(), const_cast<char *>(data), len, sample_rate,
            stereo ? ZOOMSDK::ZoomSDKAudioChannel_Stereo
                   : ZOOMSDK::ZoomSDKAudioChannel_Mono));
    }

    TalkbackResult set_background_volume(const std::string &channel_id,
                                         float volume) override
    {
        if (!m_ctrl) return TalkbackResult::NotExist;
        return talkback_win_result(
            m_ctrl->SetChannelBackgroundVolume(channel_id.c_str(), volume));
    }

    void set_events(TalkbackSdkEvents *events) override { m_events = events; }

private:
    ZOOMSDK::IMeetingTalkbackController *m_ctrl = nullptr;
    TalkbackSdkEvents *m_events = nullptr;
};
```

- [ ] **Step 5: Retarget `EngineTalkback` onto the seam**

In `engine/src/engine-talkback.h`, replace the `ZOOMSDK::IMeetingTalkbackController *m_ctrl` member with `TalkbackSdk *m_sdk`, and add `void set_sdk(TalkbackSdk *sdk);`. In `engine-talkback.cpp`, replace each `m_ctrl->X(...)` with the corresponding `m_sdk->x(...)`, and delete the Begin/Add/Execute call sites now owned by the adapter. Replace raw `SDKERR_TOO_FREQUENT_CALL` comparisons with `TalkbackResult::TooFrequent`.

This is mechanical. Do not take the opportunity to restructure the ladder — the existing suite is the only thing making this safe, and it only protects behaviour that stays put.

- [ ] **Step 6: Retarget the fakes**

In `tests/engine-talkback-select-test.cpp`, replace `class FakeTalkbackController : public ZOOMSDK::IMeetingTalkbackController` with a `FakeTalkbackSdk : public TalkbackSdk`, carrying the same scripted behaviour plus a `next_create_result` field for Step 2's test. Keep every existing assertion.

- [ ] **Step 7: Un-gate the test target so this is verifiable at all**

In `CMakeLists.txt`, `CoreVideoEngineTalkbackSelectTest` is gated inside an `if(WIN32)` block (around `:1228`) under a comment stating the engine is Windows-only. That is now false for this target: it needs `engine-talkback.cpp` plus `TalkbackSdk`, and no Zoom headers. Move its `add_executable` and `add_test` out of the Windows block, and update the stale comment to say why it is portable now.

- [ ] **Step 8: Build and run the suite on macOS**

Run: `cmake --build build --config Release --parallel 8 --target CoreVideoEngineTalkbackSelectTest && ./build/CoreVideoEngineTalkbackSelectTest`
Expected: compiles and PASSES, on a machine with no Zoom SDK and no meeting. A Zoom-header include error means a Step 5 call site was missed — fix it there, never with a macOS `#ifdef`.

- [ ] **Step 9: Run the full suite on macOS**

Run: `cd build && ctest -C Release --output-on-failure`
Expected: PASS, with the previous count plus the new normalisation pin and the newly-portable talkback target.

- [ ] **Step 10: Mutation-prove the new pin**

Change `talkback_win_result`'s `SDKERR_TOO_FREQUENT_CALL` case to return `TalkbackResult::Unknown`. Rebuild and run. Expected: FAIL. Revert and confirm green.

Note this mutation is on Windows-only code, so it is proved by inspection of the mapping plus the macOS-side fake test; the fake's `script_create_results` path is what actually exercises the ladder's retry here.

- [ ] **Step 11: Commit**

```bash
git add src/talkback-sdk.h engine/src/engine-talkback-sdk-win.h \
        engine/src/engine-talkback.h engine/src/engine-talkback.cpp \
        engine/src/main.cpp tests/engine-talkback-select-test.cpp CMakeLists.txt
git commit -m "refactor(talkback): normalised SDK seam, suite portable to macOS"
```

**Windows verification:** none of the above runs a Windows suite — this machine has no Windows toolchain. Push the branch and let the PR's Windows x64 check confirm no behavioural change, per Global Constraints.

---

### Task 2: The macOS adapter

**Files:**
- Create: `engine/src/engine-talkback-sdk-macos.h`, `engine/src/engine-talkback-sdk-macos.mm`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `TalkbackSdk`, `TalkbackSdkEvents`, `TalkbackResult` (Task 1).
- Produces: `TalkbackMacSdk`, constructed from a `ZoomSDKTalkbackController *`.

- [ ] **Step 1: Write the header**

```objc
#pragma once
#include "talkback-sdk.h"
#include <memory>

// Opaque so C++ translation units can hold one without importing ZoomSDK.
class TalkbackMacSdk : public TalkbackSdk {
public:
    TalkbackMacSdk();
    ~TalkbackMacSdk() override;

    // Rebound whenever the meeting service hands us a controller. Mirrors the
    // Windows engine's guard: never reassign while a session is live, because a
    // roster event can fire mid-press and a stray nil would null the controller
    // for the rest of that press.
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

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
```

- [ ] **Step 2: Write the implementation**

Two mappings and a delegate. Note `invite_users` and `destroy_channels` are single calls here — the whole reason the Windows batch machinery does not cross this line.

```objc
#import "engine-talkback-sdk-macos.h"
#import <ZoomSDK/ZoomSDK.h>

static TalkbackResult mac_result(ZoomSDKError e)
{
    switch (e) {
    case ZoomSDKError_Success:         return TalkbackResult::Ok;
    case ZoomSDKError_TooFrequentCall: return TalkbackResult::TooFrequent;
    case ZoomSDKError_NoPermission:    return TalkbackResult::NoPermission;
    default:                           return TalkbackResult::Unknown;
    }
}

static TalkbackResult mac_tb_result(ZoomSDKTalkbackError e)
{
    switch (e) {
    case ZoomSDKTalkbackError_OK:           return TalkbackResult::Ok;
    case ZoomSDKTalkbackError_AlreadyExist: return TalkbackResult::AlreadyExists;
    case ZoomSDKTalkbackError_NoPermission: return TalkbackResult::NoPermission;
    case ZoomSDKTalkbackError_NotExist:     return TalkbackResult::NotExist;
    case ZoomSDKTalkbackError_Rejected:     return TalkbackResult::Rejected;
    case ZoomSDKTalkbackError_Timeout:      return TalkbackResult::Timeout;
    default:                                return TalkbackResult::Unknown;
    }
}

// EVERY delegate method is implemented. The port's own history: omitting a
// @required method on this SDK is an unrecognized-selector CRASH, not a
// compile error, so the unused receive-side callbacks are stubbed rather
// than left out.
@interface CVTalkbackDelegate : NSObject <ZoomSDKTalkbackControllerDelegate>
@property (nonatomic, assign) TalkbackSdkEvents *events;
@end

@implementation CVTalkbackDelegate
- (void)onCreateChannelResponse:(NSString *)channelID
                          error:(ZoomSDKTalkbackError)error {
    if (_events) _events->on_create_channel_response(
        channelID.UTF8String ?: "", mac_tb_result(error));
}
- (void)onDestroyChannelResponse:(NSString *)channelID
                           error:(ZoomSDKTalkbackError)error {
    if (_events) _events->on_destroy_channel_response(
        channelID.UTF8String ?: "", mac_tb_result(error));
}
- (void)onChannelUserJoinResponse:(NSString *)channelID
                           userID:(unsigned int)userID
                            error:(ZoomSDKTalkbackError)error {
    if (_events) _events->on_channel_user_join_response(
        channelID.UTF8String ?: "", userID, mac_tb_result(error));
}
- (void)onChannelUserLeaveResponse:(NSString *)channelID
                            userID:(unsigned int)userID
                             error:(ZoomSDKTalkbackError)error {
    if (_events) _events->on_channel_user_leave_response(
        channelID.UTF8String ?: "", userID, mac_tb_result(error));
}
// Receive-side: this engine is the DIRECTOR, never talent. Stubbed because
// @required, not because they are expected.
- (void)onJoinTalkbackChannel:(unsigned int)inviterID {}
- (void)onLeaveTalkbackChannel:(unsigned int)inviterID {}
- (void)onInviterAudioLevel:(unsigned int)inviterID
                 audioLevel:(unsigned int)audioLevel {}
@end

// `delegate` on ZoomSDKTalkbackController is declared `assign`, i.e. UNSAFE
// UNRETAINED -- the SDK does not keep our delegate alive. Impl holds the only
// strong reference, so the delegate's lifetime is exactly the adapter's. Let
// this go out of scope while a session is live and the SDK calls back through
// a dangling pointer, which is a crash with no diagnostic pointing here.
struct TalkbackMacSdk::Impl {
    ZoomSDKTalkbackController *ctrl = nil;   // not owned
    CVTalkbackDelegate *delegate = nil;      // OWNED -- see above
};

TalkbackMacSdk::TalkbackMacSdk() : m_impl(new Impl)
{
    m_impl->delegate = [[CVTalkbackDelegate alloc] init];
}
TalkbackMacSdk::~TalkbackMacSdk() = default;

void TalkbackMacSdk::bind(void *controller)
{
    m_impl->ctrl = (__bridge ZoomSDKTalkbackController *)controller;
    [m_impl->ctrl setDelegate:m_impl->delegate];
}

bool TalkbackMacSdk::is_meeting_support_talkback()
{
    return m_impl->ctrl && [m_impl->ctrl isMeetingSupportTalkBack];
}

TalkbackResult TalkbackMacSdk::create_channel(uint32_t count)
{
    if (!m_impl->ctrl) return TalkbackResult::NotExist;
    return mac_result([m_impl->ctrl createChannel:count]);
}

TalkbackResult TalkbackMacSdk::invite_users(
    const std::string &channel_id, const std::vector<uint32_t> &user_ids)
{
    if (!m_impl->ctrl) return TalkbackResult::NotExist;
    NSMutableArray<NSNumber *> *ids = [NSMutableArray array];
    for (uint32_t id : user_ids) [ids addObject:@(id)];
    return mac_result([m_impl->ctrl
        inviteUsersToChannel:@(channel_id.c_str()) userIDList:ids]);
}

TalkbackResult TalkbackMacSdk::destroy_channels(
    const std::vector<std::string> &channel_ids)
{
    if (!m_impl->ctrl) return TalkbackResult::NotExist;
    NSMutableArray<NSString *> *ids = [NSMutableArray array];
    for (const auto &id : channel_ids) [ids addObject:@(id.c_str())];
    return mac_result([m_impl->ctrl destroyChannels:ids]);
}

TalkbackResult TalkbackMacSdk::send_audio(const std::string &channel_id,
                                          const char *data, uint32_t len,
                                          uint32_t sample_rate, bool stereo)
{
    if (!m_impl->ctrl) return TalkbackResult::NotExist;
    return mac_result([m_impl->ctrl
        sendAudioDataToChannel:@(channel_id.c_str())
                     audioData:const_cast<char *>(data)
                    dataLength:len
                    sampleRate:sample_rate
                       channel:stereo ? ZoomSDKAudioChannel_Stereo
                                      : ZoomSDKAudioChannel_Mono]);
}

TalkbackResult TalkbackMacSdk::set_background_volume(
    const std::string &channel_id, float volume)
{
    if (!m_impl->ctrl) return TalkbackResult::NotExist;
    return mac_result([m_impl->ctrl
        setChannelBackgroundVolume:@(channel_id.c_str())
                  backgroundVolume:volume]);
}

void TalkbackMacSdk::set_events(TalkbackSdkEvents *events)
{
    m_impl->delegate.events = events;
}
```

- [ ] **Step 3: Add to the macOS engine target**

In `CMakeLists.txt`, add `engine/src/engine-talkback-sdk-macos.mm` and `engine/src/engine-talkback.cpp` to the macOS `ZoomObsEngine` target (currently `main-macos.mm` alone, `:459`).

- [ ] **Step 4: Configure a build that actually has the engine target**

The default `build/` directory on this machine has **no `BUILD_ZOOM_ENGINE`** in its cache, so `ZoomObsEngine` does not exist as a target and Step 5 would fail with "no rule to make target" rather than a real compile error.

```bash
cmake -S . -B build-engine -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_ZOOM_ENGINE=ON \
  -DZOOM_SDK_DIR="$HOME/Developer/zoom-sdk-macos" \
  -DCMAKE_PREFIX_PATH="/tmp/obs-sdk;$HOME/Qt/6.8.3/macos" \
  -DLibObs_DIR=/tmp/obs-sdk/cmake/LibObs \
  -Dobs-frontend-api_DIR=/tmp/obs-sdk/cmake/obs-frontend-api
```

If `/tmp/obs-sdk` is missing (it is cleared on reboot), recreate it from the "Get OBS headers" step in `.github/workflows/build.yml` first.

- [ ] **Step 5: Compile**

Run: `cmake --build build-engine --config Release --target ZoomObsEngine`
Expected: compiles and links against the real framework. **This step is the offline substitute for a spike** — it proves every signature, delegate protocol and enum value exists as used. A typo in a selector is an error here, not a runtime surprise in a meeting.

- [ ] **Step 6: Commit**

```bash
git add engine/src/engine-talkback-sdk-macos.h \
        engine/src/engine-talkback-sdk-macos.mm CMakeLists.txt
git commit -m "feat(talkback): macOS adapter over ZoomSDKTalkbackController"
```

---

### Task 3: The main-queue pacing pump and command wiring

The most defect-prone task, because the threading model differs from Windows and the difference is invisible until something races.

**Files:**
- Create: `engine/src/engine-talkback-pump-macos.h`, `engine/src/engine-talkback-pump-macos.mm`
- Modify: `engine/src/main-macos.mm`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `TalkbackMacSdk` (Task 2), `EngineTalkback::nomination_tick()` / `mic_tick()` / `drain_audio()`.
- Produces: `talkback_pump_start(EngineTalkback *)`, `talkback_pump_stop()`.

- [ ] **Step 1: Write the pump**

On Windows the ladder rides the command loop's 50 ms idle turn. macOS has no such turn — the reader thread must not make SDK calls at all. A main-queue timer is the equivalent, and it is the *only* correct home.

```objc
#import "engine-talkback-pump-macos.h"
#import "engine-talkback.h"
#import <dispatch/dispatch.h>

static dispatch_source_t g_pump = nil;

// 50 ms to match the Windows idle turn. The interval is NOT the pacing: Law 2's
// ~600 ms floor is enforced inside nomination_tick(), which spends at most one
// membership call per turn. Ticking faster than the floor is harmless; ticking
// slower would silently stretch a 24-invite ladder.
void talkback_pump_start(EngineTalkback *tb)
{
    if (g_pump) return;
    g_pump = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                    dispatch_get_main_queue());
    dispatch_source_set_timer(g_pump, dispatch_time(DISPATCH_TIME_NOW, 0),
                              50ull * NSEC_PER_MSEC, 10ull * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(g_pump, ^{
        tb->nomination_tick();
        tb->mic_tick();
    });
    dispatch_resume(g_pump);
}

void talkback_pump_stop(void)
{
    if (!g_pump) return;
    dispatch_source_cancel(g_pump);
    g_pump = nil;
}
```

- [ ] **Step 2: Wire the seven IPC commands**

In `main-macos.mm`'s reader loop (around line 2256), add branches for the talkback commands. **Every one dispatches to the main queue** — the reader thread must never touch the SDK.

Order matters and is the same trap the existing loop documents for `unsubscribe`/`subscribe`: these are substring matches, so `talkback_stop` must be checked before `talkback_start` would match a prefix, and the longer names before their shorter relatives.

```objc
} else if (msg.find(IPC_CMD_TALKBACK_NOMINATE) != std::string::npos) {
    dispatch_async(dispatch_get_main_queue(), ^{ handle_talkback_nominate(msg); });
} else if (msg.find(IPC_CMD_TALKBACK_PROBE) != std::string::npos) {
    dispatch_async(dispatch_get_main_queue(), ^{ handle_talkback_probe(msg); });
} else if (msg.find(IPC_CMD_TALKBACK_AUDIO) != std::string::npos) {
    dispatch_async(dispatch_get_main_queue(), ^{ g_talkback.drain_audio(); });
} else if (msg.find(IPC_CMD_TALKBACK_CLOSE) != std::string::npos) {
    dispatch_async(dispatch_get_main_queue(), ^{ g_talkback.close_audio(); });
} else if (msg.find(IPC_CMD_TALKBACK_OPEN) != std::string::npos) {
    dispatch_async(dispatch_get_main_queue(), ^{ handle_talkback_open(msg); });
} else if (msg.find(IPC_CMD_TALKBACK_STOP) != std::string::npos) {
    dispatch_async(dispatch_get_main_queue(), ^{ g_talkback.session_stop(); });
} else if (msg.find(IPC_CMD_TALKBACK_START) != std::string::npos) {
    dispatch_async(dispatch_get_main_queue(), ^{ handle_talkback_start(msg); });
}
```

- [ ] **Step 3: Write the ordering test**

The substring-matching trap has already caused one real defect on this loop. Pin it.

```cpp
{
    // "talkback_start" contains no other command as a substring, but
    // "talkback_stop" and "talkback_start" share the "talkback_st" prefix and
    // both contain "talkback_". Dispatch must land each on its own handler.
    check(talkback_command_for("{\"cmd\":\"talkback_stop\"}") == TalkbackCmd::Stop,
          "talkback_stop was routed somewhere else");
    check(talkback_command_for("{\"cmd\":\"talkback_start\"}") == TalkbackCmd::Start,
          "talkback_start was routed somewhere else");
    check(talkback_command_for("{\"cmd\":\"talkback_close\"}") == TalkbackCmd::Close,
          "talkback_close was routed somewhere else");
}
```

Extract the match into a pure `talkback_command_for(const std::string &)` in `src/talkback-command.h` so a host test can reach it — inline in the reader loop it is untestable, which is the shape of both Majors this feature already shipped.

- [ ] **Step 4: Run it, watch it fail, implement, watch it pass**

Run: `cmake --build build --target CoreVideoEngineTalkbackSelectTest && ./build/CoreVideoEngineTalkbackSelectTest`
Expected: FAIL (no `talkback_command_for`), then PASS after implementing.

- [ ] **Step 5: Mutation-prove the ordering**

Swap the `talkback_start` branch above `talkback_stop`. Expected: the ordering test FAILS. Revert, confirm green.

- [ ] **Step 6: Commit**

```bash
git add engine/src/engine-talkback-pump-macos.h \
        engine/src/engine-talkback-pump-macos.mm \
        engine/src/main-macos.mm src/talkback-command.h \
        tests/engine-talkback-select-test.cpp CMakeLists.txt
git commit -m "feat(talkback): main-queue pump and command wiring for macOS"
```

---

### Task 4: Turn the dock on, and correct the docs

**Files:**
- Modify: `src/zoom-talkback-panel.cpp`, `tests/talkback-dock-state-test.cpp`, `CLAUDE.md`

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: Flip the constant**

In `src/zoom-talkback-panel.cpp`, delete the `#if defined(__APPLE__)` block and set `kTalkbackPlatformSupported = true` unconditionally. The gate's machinery stays — it is now the switch it was designed to be, and PR #242's tests keep pinning that a default context is supported.

- [ ] **Step 2: Keep the gate's own tests honest**

The macOS-specific assertions in `tests/talkback-dock-state-test.cpp` still pass — they drive `platform_supported = false` explicitly and pin the banner and refusal wording for it. **Do not delete them.** They now pin the mechanism rather than the platform, and something has to keep `BannerState::Unavailable` reachable.

- [ ] **Step 3: Rewrite the CLAUDE.md macOS talkback section**

Replace the "Talkback does not exist on macOS" entry with what is now true: the seam, the two adapters, the main-queue pump and *why* it is not the reader thread, and the fact that the batch machinery is Windows-adapter-internal. State plainly which parts are live-verified and which are not.

- [ ] **Step 4: Full build and suite, both platforms**

Run: `cmake --build build --config Release --parallel 8 && cd build && ctest -C Release --output-on-failure`
Expected: PASS on macOS and on Windows CI.

- [ ] **Step 5: Commit**

```bash
git add src/zoom-talkback-panel.cpp CLAUDE.md
git commit -m "feat(talkback): enable the dock on macOS"
```

---

### Task 5: Live verification — one session, batched

**Everything above is offline. This task is the only one that needs a meeting, and it is deliberately last.**

Batch it with the two standing macOS unknowns. All three want the same scarce thing — a good connection and a real meeting — and there is a specific reason not to iterate: the `initSDKWithParams` wedge is documented as *escalating with repeated engine kills* (15s → 103s → 4.5min → never). Repeated live attempts actively degrade the machine's ability to test. Prepare fully, run once.

- [ ] **Step 1: Before joining, write down the expected outcome of each check.** A live session where the pass criteria are decided afterwards proves nothing.

- [ ] **Step 2: Settle the two standing unknowns first**, because they gate everything else: does one-way audio deliver on macOS (mono vs stereo control), and does `initSDKWithParams` still wedge. Capture the audio verdict before touching talkback.

- [ ] **Step 3: `talkback_probe` against a real meeting.** Expected: the probe ladder reaches `send` and `destroy` with no `create_rate_limited`.

- [ ] **Step 4: Nominate a list large enough to exercise Law 2** — at least 3 channels, so more than one membership call is paced. Expected: every create and invite confirmed, no terminal `create_rate_limited`, ladder reports `nominate_done`.

- [ ] **Step 5: Key a target and confirm audio is heard**, by a human on the other end. Confirm the first syllable is not lost — the claim the Windows gate measured.

- [ ] **Step 6: Confirm membership is acoustically neutral until keyed** — the 2026-08-29 production defect where talent were ducked on assignment. Ask an assigned, unkeyed participant whether meeting audio is attenuated.

- [ ] **Step 7: Record the outcomes in `docs/superpowers/notes/YYYY-MM-DD-macos-talkback-live-gate.md`** and update CLAUDE.md's verified/unverified split to match what actually happened.

---

## Open Questions

These do not block Tasks 1–5 and should be answered in Task 5 rather than guessed at now:

1. **Does macOS rate-limit membership calls identically?** Law 2 was measured on Windows and on ZComms. The pacing is implemented regardless — it is harmless if unnecessary and essential if not — but the ~600 ms floor may want re-measuring.
2. **Does the macOS SDK duck channel members at creation**, as Zoom does on Windows? The provision-time `setChannelBackgroundVolume:` neutral write is ported unconditionally for the same reason.
3. **Is `sendAudioDataToChannel:` safe off the main queue?** The plan keeps every SDK call on the main queue, which is safe by construction. If audio latency proves unacceptable, that is the first thing to measure — not to assume.
