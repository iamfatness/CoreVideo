# Zoom Talkback — Milestone 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove, against a live Zoom meeting, that this account can create a talkback channel, invite a participant to it, and send audio only that participant hears — before any transport, UI, or Companion work is built on top.

**Architecture:** A new plugin→engine IPC command (`talkback_probe`) drives a thin ladder inside the engine: acquire `IMeetingTalkbackController`, report the meeting-level gate, create one channel, invite one participant resolved by name, send a generated sine tone for a few seconds, destroy the channel. Every rung reports its own `SDKError` and `TalkbackError` back over E2P, so a failure names the exact rung it fell off. No shared memory, no OBS audio tap, no UI — those are Milestone 2+.

**Tech Stack:** C++17, Zoom Windows Meeting SDK 7.1.5.43953, CMake + CTest, existing named-pipe line-JSON IPC.

**Spec:** `docs/superpowers/specs/2026-08-24-zoom-talkback-design.md`

## Global Constraints

- **This milestone is a hard gate.** If Task 6 fails, Milestones 2–8 do not start. Report the failure; do not work around it.
- Zoom Meeting SDK **7.1.5.43953**; talkback shipped in Meeting SDK **7.0.0**.
- Talkback caps, from the SDK headers: **max 16 channels**, **max 10 users per channel**.
- Audio to `SendAudioDataToChannel` must be **PCM, 16-bit**, mono or stereo, `dataLength` a **multiple of 2**. Sample rate **48000** for this milestone (32 kHz or 48 kHz recommended by the SDK).
- Participants are addressed **by name**, resolved to a meeting-scoped `userID` at use time. Never persist a raw Zoom user ID — they are meeting-scoped and get recycled.
- Tests are plain executables, no framework, one `check()`-style file per invariant cluster in `tests/`, registered in `CMakeLists.txt` with `add_executable` + `add_test`.
- Build and test from the worktree: `cmake --build build_x64 --config Release --parallel 8` then `cd build_x64 && ctest -C Release --output-on-failure` — must be N/N green.
- Comments state the constraint the code cannot show. When a change is motivated by a live failure, say what happened, with numbers.
- Never run a second OBS instance while one is testing (pipe/SDK singleton collision, crash loop). Send `{"cmd":"leave"}` before closing OBS.

---

### Task 1: Route the `talkback_probe` command

The engine identifies commands by exact match on the declared `cmd` field (`src/engine-command.h`), never by substring — a substring dispatch once routed every `unsubscribe` into the `subscribe` branch. A new command must be added to the token list, the enum, and the routing function together, and pinned by the existing routing test.

**Files:**
- Modify: `src/engine-ipc.h:19` (add token after `IPC_CMD_QUIT`)
- Modify: `src/engine-command.h:32-43` (enum), `src/engine-command.h:79-92` (routing)
- Test: `tests/engine-command-test.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `IPC_CMD_TALKBACK_PROBE` (string literal `"talkback_probe"`), `IpcCommand::TalkbackProbe`. Task 4 branches on the enum value; Task 5 emits the literal.

- [ ] **Step 1: Write the failing test**

Append inside `main()` in `tests/engine-command-test.cpp`, before the final `if (failures == 0)` block:

```cpp
    // --- Talkback probe routes exactly, and does not collide ---
    check(ipc_command_of(R"({"cmd":"talkback_probe","participant":"Sarah Muller"})") ==
              IpcCommand::TalkbackProbe,
          "talkback_probe did not route to IpcCommand::TalkbackProbe");
    // A display name or payload containing the token must not route.
    check(ipc_command_of(R"({"cmd":"join","display_name":"talkback_probe"})") ==
              IpcCommand::Join,
          "a payload containing 'talkback_probe' hijacked the join branch");
    // Guard the substring family the same way the existing commands are guarded.
    check(ipc_command_of(R"({"cmd":"talkback_probe_extra"})") == IpcCommand::Unknown,
          "a longer command starting with talkback_probe matched it");
```

- [ ] **Step 2: Run test to verify it fails**

```sh
cmake --build build_x64 --config Release --target CoreVideoEngineCommandTest --parallel 8
```

Expected: FAIL to compile with `'TalkbackProbe' is not a member of 'IpcCommand'`.

- [ ] **Step 3: Write minimal implementation**

In `src/engine-ipc.h`, after the `IPC_CMD_QUIT` line:

```c
#define IPC_CMD_TALKBACK_PROBE "talkback_probe"
```

In `src/engine-command.h`, add to the `IpcCommand` enum after `Unsubscribe,`:

```cpp
    TalkbackProbe,
```

In `ipc_command_of`, add before the final `return IpcCommand::Unknown;`:

```cpp
    if (cmd == IPC_CMD_TALKBACK_PROBE) return IpcCommand::TalkbackProbe;
```

- [ ] **Step 4: Run test to verify it passes**

```sh
cmake --build build_x64 --config Release --target CoreVideoEngineCommandTest --parallel 8
cd build_x64 && ctest -C Release -R CoreVideoEngineCommand --output-on-failure
```

Expected: PASS, `engine-command: all tests passed`.

- [ ] **Step 5: Commit**

```sh
git add src/engine-ipc.h src/engine-command.h tests/engine-command-test.cpp
git commit -m "feat(talkback): route the talkback_probe command"
```

---

### Task 2: Tone generator

The probe needs audio to send. A generated sine is better than a file: no asset, no format ambiguity, and a listener can confirm "I heard a steady tone" unambiguously. Phase must carry across calls — restarting phase every buffer produces a click at each boundary that a listener would report as broken audio when the transport was fine.

**Files:**
- Create: `src/talkback-tone.h`
- Create: `tests/talkback-tone-test.cpp`
- Modify: `CMakeLists.txt` (register the test after the `CoreVideoJoinWatchdogTest` block, `CMakeLists.txt:959-966`)

**Interfaces:**
- Consumes: nothing.
- Produces: `uint64_t talkback_tone_fill(int16_t *out, size_t count, uint64_t start_index, uint32_t sample_rate, double freq_hz, double amplitude)` — fills `count` mono samples, returns the next absolute sample index. Task 4 calls this.

- [ ] **Step 1: Write the failing test**

Create `tests/talkback-tone-test.cpp`:

```cpp
// tests/talkback-tone-test.cpp
// The probe's test tone. Pinned because a phase discontinuity between buffers
// is audible as a click, and a listener reporting "clicky audio" would be
// mistaken for a transport fault when the transport was fine.
#include "talkback-tone.h"

#include <cmath>
#include <iostream>
#include <vector>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

int main()
{
    constexpr uint32_t kRate = 48000;
    constexpr double kFreq = 440.0;
    constexpr double kAmp = 0.5;

    // --- Returns the next absolute index, so callers can chain ---
    std::vector<int16_t> a(480);
    const uint64_t next = talkback_tone_fill(a.data(), a.size(), 0, kRate, kFreq, kAmp);
    check(next == 480, "talkback_tone_fill did not return start_index + count");

    // --- Phase is continuous across a buffer boundary ---
    // Filling [0,960) in one call must equal filling [0,480) then [480,960).
    std::vector<int16_t> whole(960);
    talkback_tone_fill(whole.data(), whole.size(), 0, kRate, kFreq, kAmp);
    std::vector<int16_t> second(480);
    talkback_tone_fill(second.data(), second.size(), next, kRate, kFreq, kAmp);
    bool contiguous = true;
    for (size_t i = 0; i < 480; ++i) {
        if (whole[i] != a[i] || whole[480 + i] != second[i]) { contiguous = false; break; }
    }
    check(contiguous,
          "phase restarted at the buffer boundary -- a chained fill did not match "
          "one continuous fill, which is an audible click every buffer");

    // --- Amplitude is respected and never clips ---
    int16_t peak = 0;
    for (int16_t s : whole) {
        const int16_t mag = static_cast<int16_t>(s < 0 ? -s : s);
        if (mag > peak) peak = mag;
    }
    check(peak <= 16384 + 64, "tone exceeded the requested 0.5 amplitude");
    check(peak > 16384 - 512, "tone was far quieter than the requested amplitude");

    // --- Starts at zero, so keying in does not begin with a step ---
    check(whole[0] == 0, "the tone did not start at zero amplitude");

    // --- One full period lands back near zero (440Hz at 48kHz ~ 109.09 samples) ---
    std::vector<int16_t> one_sec(kRate);
    talkback_tone_fill(one_sec.data(), one_sec.size(), 0, kRate, kFreq, kAmp);
    int zero_crossings = 0;
    for (size_t i = 1; i < one_sec.size(); ++i) {
        if ((one_sec[i - 1] < 0) != (one_sec[i] < 0)) ++zero_crossings;
    }
    // 440Hz => 880 sign changes per second, allow a couple either side.
    check(zero_crossings >= 878 && zero_crossings <= 882,
          "the tone was not 440Hz -- zero-crossing count was wrong");

    // --- Zero count is a no-op, not a crash ---
    const uint64_t same = talkback_tone_fill(nullptr, 0, 1234, kRate, kFreq, kAmp);
    check(same == 1234, "a zero-length fill did not return start_index unchanged");

    if (failures == 0)
        std::cout << "talkback-tone: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Add to `CMakeLists.txt` immediately after the `add_test(NAME CoreVideoJoinWatchdog ...)` block:

```cmake
    # The talkback probe's test tone. Phase must carry across buffers: a restart
    # every buffer is an audible click that would be misread as a transport
    # fault. See src/talkback-tone.h.
    add_executable(CoreVideoTalkbackToneTest
        tests/talkback-tone-test.cpp
    )
    target_include_directories(CoreVideoTalkbackToneTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTalkbackTone
             COMMAND CoreVideoTalkbackToneTest)
```

Then:

```sh
cmake -S . -B build_x64 && cmake --build build_x64 --config Release --target CoreVideoTalkbackToneTest --parallel 8
```

Expected: FAIL — `Cannot open include file: 'talkback-tone.h'`.

- [ ] **Step 3: Write minimal implementation**

Create `src/talkback-tone.h`:

```cpp
#pragma once
//
// talkback-tone.h — the generated tone the talkback probe sends.
//
// A generated sine rather than an audio asset: no file to ship, no format to
// get wrong, and a listener can confirm "steady tone" or "no tone" without
// ambiguity. That matters because the probe's whole job is to answer a yes/no
// question about entitlement, and an ambiguous result answers nothing.
//
// Phase is an ABSOLUTE sample index supplied by the caller, not internal
// state. Buffers are sent one after another; restarting phase at each buffer
// boundary steps the waveform discontinuously, which is audible as a click at
// the buffer rate (~100/sec). A listener would report that as broken audio and
// we would be debugging a transport that was working correctly.
//
// Free of Qt / OBS / Zoom SDK dependencies so it can be pinned by a test with
// no engine and no meeting.
//
#include <cmath>
#include <cstddef>
#include <cstdint>

// Fills `out` with `count` mono 16-bit samples of a sine at `freq_hz`,
// continuing from absolute sample index `start_index`. Returns the next
// absolute index, so the caller chains successive buffers by feeding the
// return value back in.
//
// `amplitude` is 0.0-1.0 of full scale. Kept below 1.0 by callers so the
// int16 conversion cannot wrap on rounding.
inline uint64_t talkback_tone_fill(int16_t *out,
                                   std::size_t count,
                                   uint64_t start_index,
                                   uint32_t sample_rate,
                                   double freq_hz,
                                   double amplitude)
{
    if (out == nullptr || count == 0 || sample_rate == 0)
        return start_index;

    constexpr double kTwoPi = 6.283185307179586476925286766559;
    const double step = kTwoPi * freq_hz / static_cast<double>(sample_rate);

    for (std::size_t i = 0; i < count; ++i) {
        // Phase from the absolute index, never from a running accumulator:
        // an accumulator drifts, and more importantly it would have to be
        // stored somewhere, which is what makes chained calls discontinuous.
        const double phase = step * static_cast<double>(start_index + i);
        const double v = std::sin(phase) * amplitude * 32767.0;
        out[i] = static_cast<int16_t>(v < 0.0 ? v - 0.5 : v + 0.5);
    }
    return start_index + count;
}
```

- [ ] **Step 4: Run test to verify it passes**

```sh
cmake --build build_x64 --config Release --target CoreVideoTalkbackToneTest --parallel 8
cd build_x64 && ctest -C Release -R CoreVideoTalkbackTone --output-on-failure
```

Expected: PASS, `talkback-tone: all tests passed`.

- [ ] **Step 5: Commit**

```sh
git add src/talkback-tone.h tests/talkback-tone-test.cpp CMakeLists.txt
git commit -m "feat(talkback): generated test tone with continuous phase"
```

---

### Task 3: Engine talkback probe — controller and the meeting gate

The first two rungs of the ladder, and the two that answer the entitlement question. `GetMeetingTalkbackController()` returning null, or `IsMeetingSupportTalkBack()` returning false, is the whole gate — everything after it is mechanics.

**Files:**
- Create: `engine/src/engine-talkback.h`, `engine/src/engine-talkback.cpp`, `engine/src/engine-json.h`
- Modify: `engine/src/main.cpp:268,300,391` — move the three `static` JSON helpers into `engine-json.h`
- Modify: `CMakeLists.txt` — add `engine/src/engine-talkback.cpp` to `ENGINE_SOURCES`

**Interfaces:**
- Consumes: `talkback_tone_fill` from Task 2.
- Produces: `class EngineTalkback` with `void probe(ZOOMSDK::IMeetingService *svc, const std::string &participant_name)` and `void tick()`. Task 4 fills in the ladder; Task 5 triggers `probe()` from the command branch.

- [ ] **Step 1: Write the failing test**

There is no unit test for this task — it is SDK-bound and its verification is Task 6's live pass. The compile *is* the check at this stage. Write the header first so the shape is committed before the implementation:

Create `engine/src/engine-talkback.h`:

```cpp
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
#include "zoom_sdk.h"
#include "meeting_service_interface.h"
#include "meeting_service_components/meeting_talkback_ctrl_interface.h"

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
    Phase        m_phase = Phase::Idle;
    std::string  m_channel_id;
    std::string  m_participant_name;
    unsigned int m_participant_id = 0;
    uint64_t     m_tone_index = 0;
    uint32_t     m_buffers_sent = 0;
};
```

- [ ] **Step 2: Run the build to verify it fails**

Add `engine/src/engine-talkback.cpp` to the `ENGINE_SOURCES` list in `CMakeLists.txt`, then:

```sh
cmake -S . -B build_x64 && cmake --build build_x64 --config Release --target ZoomObsEngine --parallel 8
```

Expected: FAIL — `engine-talkback.cpp` does not exist.

- [ ] **Step 3: Write minimal implementation**

Create `engine/src/engine-talkback.cpp` with the first two rungs only:

```cpp
#include "engine-talkback.h"
#include "engine-writer.h"   // EngineIpc::write -- an inline fn in a namespace,
                             // so it must be INCLUDED, never forward-declared
#include "talkback-tone.h"
#include "engine-json.h"     // zchar_to_utf8 / json_escape / json_str (Step 3a)

#include <string>

void EngineTalkback::report(const std::string &stage, const std::string &fields)
{
    std::string line = R"({"cmd":"talkback_probe","stage":")" + stage + "\"";
    if (!fields.empty()) line += "," + fields;
    line += "}";
    EngineIpc::write(line);
}

void EngineTalkback::probe(ZOOMSDK::IMeetingService *svc,
                           const std::string &participant_name)
{
    m_svc = svc;
    m_participant_name = participant_name;
    m_phase = Phase::Idle;
    m_channel_id.clear();
    m_participant_id = 0;
    m_tone_index = 0;
    m_buffers_sent = 0;

    if (!m_svc) {
        report("controller", R"("ok":false,"reason":"no_meeting_service")");
        m_phase = Phase::Done;
        return;
    }

    // RUNG 1: does the controller exist at all on this SDK/account?
    m_ctrl = m_svc->GetMeetingTalkbackController();
    report("controller", std::string(R"("ok":)") + (m_ctrl ? "true" : "false"));
    if (!m_ctrl) {
        m_phase = Phase::Done;
        return;
    }

    // RUNG 2: the meeting-level gate. This is the one we expect Enhanced Media
    // to satisfy, and the one that decides whether the feature is viable.
    const bool supported = m_ctrl->IsMeetingSupportTalkBack();
    report("meeting_supported",
           std::string(R"("supported":)") + (supported ? "true" : "false"));
    if (!supported) {
        m_phase = Phase::Done;
        return;
    }

    const ZOOMSDK::SDKError set_err = m_ctrl->SetEvent(this);
    report("set_event", R"("code":)" + std::to_string(static_cast<int>(set_err)));
    if (set_err != ZOOMSDK::SDKERR_SUCCESS) {
        m_phase = Phase::Done;
        return;
    }

    // Rungs 3-6 land in Task 4.
    report("done", R"("reached":"gate_passed")");
    m_phase = Phase::Done;
}

void EngineTalkback::tick() {}

unsigned int EngineTalkback::resolve_participant(const std::string &) const { return 0; }

void EngineTalkback::onCreateChannelResponse(const zchar_t *, TalkbackError) {}
void EngineTalkback::onDestroyChannelResponse(const zchar_t *, TalkbackError) {}
void EngineTalkback::onChannelUserJoinResponse(const zchar_t *, unsigned int, TalkbackError) {}
void EngineTalkback::onChannelUserLeaveResponse(const zchar_t *, unsigned int, TalkbackError) {}
void EngineTalkback::onJoinTalkbackChannel(unsigned int) {}
void EngineTalkback::onLeaveTalkbackChannel(unsigned int) {}
void EngineTalkback::onInviterAudioLevel(unsigned int, unsigned int) {}
```

- [ ] **Step 3a: Extract the JSON helpers before using them**

`json_str` (`engine/src/main.cpp:268`), `json_escape` (`:300`) and `zchar_to_utf8` (`:391`) are all `static` in `main.cpp`, so `engine-talkback.cpp` cannot see them. **Move all three verbatim** into a new `engine/src/engine-json.h` as `inline` functions and include it from `main.cpp`. Do not copy them — two divergent copies of a JSON escaper is exactly the bug that outlives the person who wrote it. Confirm `main.cpp` still compiles before continuing.

- [ ] **Step 4: Run the build to verify it passes**

```sh
cmake --build build_x64 --config Release --parallel 8
cd build_x64 && ctest -C Release --output-on-failure
```

Expected: engine links; full suite still green (55/55 — 54 existing plus `CoreVideoTalkbackTone`).

- [ ] **Step 5: Commit**

```sh
git add engine/src/engine-talkback.h engine/src/engine-talkback.cpp CMakeLists.txt
git commit -m "feat(talkback): probe the controller and the meeting-level gate"
```

---

### Task 4: Engine talkback probe — channel, invite, send, destroy

The remaining four rungs. All of them are asynchronous and confirmed by callback, which is exactly why the design pre-provisions channels rather than creating them at key time — this task is where that latency becomes visible.

**Files:**
- Modify: `engine/src/engine-talkback.cpp` (replace the stub bodies from Task 3)

**Interfaces:**
- Consumes: `EngineTalkback` from Task 3, `talkback_tone_fill` from Task 2.
- Produces: no new symbols. The engine main loop calls `tick()` (wired in Task 5).

- [ ] **Step 1: Write the failing check**

No unit test — SDK-bound. The check is that the probe advances past `gate_passed` to `sent` and `destroyed` on a live meeting, which Task 6 exercises. Before implementing, add the assertion to the runbook so the expected output is written down before the code exists. Append to `docs/superpowers/plans/2026-08-24-zoom-talkback-milestone-1.md` under Task 6's expected output — it is already there; confirm it reads `stage":"sent"` and `stage":"destroyed"`.

- [ ] **Step 2: Replace the `probe()` tail and the stubs**

In `engine/src/engine-talkback.cpp`, replace the two lines:

```cpp
    // Rungs 3-6 land in Task 4.
    report("done", R"("reached":"gate_passed")");
    m_phase = Phase::Done;
```

with:

```cpp
    // RUNG 3: create exactly one channel. Max 16 exist; we make one and
    // destroy it, so a failed probe cannot leak channel budget into the
    // meeting.
    const ZOOMSDK::SDKError create_err = m_ctrl->CreateChannel(1);
    report("create_channel", R"("code":)" +
           std::to_string(static_cast<int>(create_err)));
    if (create_err != ZOOMSDK::SDKERR_SUCCESS) {
        m_phase = Phase::Done;
        return;
    }
    m_phase = Phase::AwaitingChannel;   // continues in onCreateChannelResponse
```

Replace `resolve_participant` with a real lookup. Names, never stored IDs — Zoom user IDs are meeting-scoped:

```cpp
unsigned int EngineTalkback::resolve_participant(const std::string &name) const
{
    if (!m_svc) return 0;
    auto *part = m_svc->GetMeetingParticipantsController();
    if (!part) return 0;
    ZOOMSDK::IList<unsigned int> *ids = part->GetParticipantsList();
    if (!ids) return 0;
    for (int i = 0; i < ids->GetCount(); ++i) {
        const unsigned int uid = ids->GetItem(i);
        ZOOMSDK::IUserInfo *u = part->GetUserByUserID(uid);
        if (!u) continue;
        if (zchar_to_utf8(u->GetUserName()) == name) return uid;
    }
    return 0;
}
```

Replace the three callbacks that carry the ladder:

```cpp
void EngineTalkback::onCreateChannelResponse(const zchar_t *channelID, TalkbackError error)
{
    const std::string id = zchar_to_utf8(channelID);
    report("create_channel_response",
           R"("channel":")" + json_escape(id) + R"(","error":)" +
           std::to_string(static_cast<int>(error)));
    if (error != TALKBACK_ERROR_OK || m_phase != Phase::AwaitingChannel) {
        m_phase = Phase::Done;
        return;
    }
    m_channel_id = id;

    // RUNG 4: invite one participant, resolved from a NAME. A raw id would
    // point at nobody after a rejoin and at the wrong person once ids are
    // recycled -- the defect the Companion module fixed in v0.1.44.
    m_participant_id = resolve_participant(m_participant_name);
    if (m_participant_id == 0) {
        report("invite", R"("error":"no_participant_named","name":")" +
               json_escape(m_participant_name) + "\"");
        m_phase = Phase::Destroying;
        return;
    }

    ZOOMSDK::SDKError e = m_ctrl->BeginBatchInviteUsers(channelID);
    if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->AddUserToInvite(m_participant_id);
    if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->ExecuteBatchInviteUsers();
    report("invite", R"("user_id":)" + std::to_string(m_participant_id) +
           R"(,"code":)" + std::to_string(static_cast<int>(e)));
    if (e != ZOOMSDK::SDKERR_SUCCESS) {
        m_phase = Phase::Destroying;
        return;
    }
    m_phase = Phase::AwaitingInvite;
}

void EngineTalkback::onChannelUserJoinResponse(const zchar_t *channelID,
                                               unsigned int userID, TalkbackError error)
{
    report("invite_response",
           R"("channel":")" + json_escape(zchar_to_utf8(channelID)) +
           R"(","user_id":)" + std::to_string(userID) +
           R"(,"error":)" + std::to_string(static_cast<int>(error)));
    if (m_phase != Phase::AwaitingInvite) return;
    if (error != TALKBACK_ERROR_OK) {
        m_phase = Phase::Destroying;
        return;
    }

    // Duck the main meeting for the person being spoken to, so the tone is
    // unambiguous rather than competing with meeting audio.
    const ZOOMSDK::SDKError vol =
        m_ctrl->SetChannelBackgroundVolume(channelID, 0.3f);
    report("background_volume", R"("code":)" +
           std::to_string(static_cast<int>(vol)));

    m_tone_index = 0;
    m_buffers_sent = 0;
    m_phase = Phase::Sending;   // tick() takes it from here
}
```

Replace `tick()`:

```cpp
void EngineTalkback::tick()
{
    if (m_phase == Phase::Sending) {
        // 10ms of mono 48kHz, matching the buffer size Zoom itself uses on the
        // receive side. dataLength must be a multiple of 2 (it is: 480 * 2).
        constexpr uint32_t kRate    = 48000;
        constexpr std::size_t kCount = 480;
        constexpr uint32_t kBuffers  = 300;   // ~3 seconds

        int16_t pcm[kCount];
        m_tone_index = talkback_tone_fill(pcm, kCount, m_tone_index, kRate, 440.0, 0.5);

        const ZOOMSDK::SDKError e = m_ctrl->SendAudioDataToChannel(
            m_channel_id.c_str(), reinterpret_cast<const char *>(pcm),
            static_cast<unsigned int>(kCount * sizeof(int16_t)), kRate,
            ZOOMSDK::ZoomSDKAudioChannel_Mono);

        // Report the first send and any failure, never all 300 -- 300 pipe
        // lines is the message-storm shape this codebase already has a live
        // incident about.
        if (m_buffers_sent == 0 || e != ZOOMSDK::SDKERR_SUCCESS) {
            report("send", R"("buffer":)" + std::to_string(m_buffers_sent) +
                   R"(,"code":)" + std::to_string(static_cast<int>(e)));
        }
        if (e != ZOOMSDK::SDKERR_SUCCESS) {
            m_phase = Phase::Destroying;
            return;
        }
        if (++m_buffers_sent >= kBuffers) {
            report("sent", R"("buffers":)" + std::to_string(m_buffers_sent));
            m_phase = Phase::Destroying;
        }
        return;
    }

    if (m_phase == Phase::Destroying) {
        // Always destroy, on every exit path: a leaked channel consumes one of
        // the meeting's 16 for as long as the meeting lasts.
        ZOOMSDK::SDKError e = m_ctrl->BeginBatchDestroyChannels();
        if (e == ZOOMSDK::SDKERR_SUCCESS && !m_channel_id.empty())
            e = m_ctrl->AddChannelToDestroy(m_channel_id.c_str());
        if (e == ZOOMSDK::SDKERR_SUCCESS)
            e = m_ctrl->ExecuteBatchDestroyChannels();
        report("destroy", R"("code":)" + std::to_string(static_cast<int>(e)));
        m_phase = Phase::Done;
    }
}

void EngineTalkback::onDestroyChannelResponse(const zchar_t *channelID, TalkbackError error)
{
    report("destroyed",
           R"("channel":")" + json_escape(zchar_to_utf8(channelID)) +
           R"(","error":)" + std::to_string(static_cast<int>(error)));
}
```

Note `tick()` must be called at roughly 10 ms intervals for the tone to play at real speed. Task 5 wires that.

- [ ] **Step 3: Build**

```sh
cmake --build build_x64 --config Release --parallel 8
```

Expected: engine links clean.

- [ ] **Step 4: Run the full suite**

```sh
cd build_x64 && ctest -C Release --output-on-failure
```

Expected: 55/55 green — this task adds no tests but must break none.

- [ ] **Step 5: Commit**

```sh
git add engine/src/engine-talkback.cpp
git commit -m "feat(talkback): create, invite, send and destroy in the probe ladder"
```

---

### Task 5: Wire the probe to the control API

The probe needs a trigger. The control API already drives full live cycles (`join`, `start_engine`, `assign_output`, `leave`), so it is the natural place — and it means the probe can be run without touching the dock.

**Files:**
- Modify: `engine/src/main.cpp` — instantiate `EngineTalkback`, add the `IpcCommand::TalkbackProbe` branch, call `tick()`
- Modify: `src/zoom-engine-client.h`, `src/zoom-engine-client.cpp` — add `talkback_probe(const std::string &participant_name)`
- Modify: `src/zoom-control-server.cpp` — accept `{"cmd":"talkback_probe","participant":"..."}`

**Interfaces:**
- Consumes: `IpcCommand::TalkbackProbe` (Task 1), `EngineTalkback::probe`/`tick` (Tasks 3–4).
- Produces: control API command `talkback_probe`; `ZoomEngineClient::talkback_probe(const std::string &)`.

- [ ] **Step 1: Engine — instantiate and dispatch**

In `engine/src/main.cpp`, near the other long-lived engine objects, add:

```cpp
    EngineTalkback talkback;
```

and `#include "engine-talkback.h"` with the other engine includes.

In the command loop (`engine/src/main.cpp:1261` onward), add a branch alongside the existing ones:

```cpp
        } else if (command == IpcCommand::TalkbackProbe) {
            const std::string who = json_str(line, "participant");
            if (!meeting_svc) {
                EngineIpc::write(
                    R"({"cmd":"talkback_probe","stage":"controller","ok":false,)"
                    R"("reason":"not_in_meeting"})");
            } else {
                talkback.probe(meeting_svc, who);
            }
```

Use whatever the surrounding code already calls the meeting service pointer in that scope; do not introduce a second name for it.

- [ ] **Step 2: Engine — drive `tick()` at ~10 ms**

The read loop blocks on `ipc_read_line_with_message_pump`, so `tick()` cannot live there. Start a dedicated thread when the probe begins, in the `TalkbackProbe` branch, after `talkback.probe(...)`:

```cpp
                std::thread([&talkback]() {
                    // ~3s of tone plus destroy; bounded so the thread cannot
                    // outlive the probe if a callback never arrives.
                    for (int i = 0; i < 1200; ++i) {
                        talkback.tick();
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }).detach();
```

- [ ] **Step 3: Plugin — add the client method**

In `src/zoom-engine-client.h`, declare beside `start_media()`:

```cpp
    void talkback_probe(const std::string &participant_name);
```

In `src/zoom-engine-client.cpp`, beside `start_media()` (`src/zoom-engine-client.cpp:838`):

```cpp
void ZoomEngineClient::talkback_probe(const std::string &participant_name)
{
    if (!m_running.load(std::memory_order_acquire)) return;
    write_json(R"({"cmd":"talkback_probe","participant":")" +
               json_escape(participant_name) + "\"}");
}
```

- [ ] **Step 4: Plugin — accept the control API command**

In `src/zoom-control-server.cpp`, alongside the existing `cmd == "..."` branches, add:

```cpp
    if (cmd == "talkback_probe") {
        const std::string who = json_str(body, "participant");
        if (who.empty())
            return R"({"ok":false,"error":"participant name required"})";
        ZoomEngineClient::instance().talkback_probe(who);
        return R"({"ok":true,"note":"probe started; watch the OBS log for talkback_probe stages"})";
    }
```

Match the surrounding code's exact parameter names and response-building style rather than the sketch above.

Ensure the engine's `talkback_probe` lines reach the OBS log. In `ZoomEngineClient::handle_event` (`src/zoom-engine-client.cpp:1096`), add a branch that `blog(LOG_INFO, ...)`s any line whose `cmd` is `talkback_probe`, verbatim. Every rung must be visible; this is the entire output of the milestone.

- [ ] **Step 5: Build and run the full suite**

```sh
cmake --build build_x64 --config Release --parallel 8
cd build_x64 && ctest -C Release --output-on-failure
```

Expected: 55/55 green.

- [ ] **Step 6: Commit**

```sh
git add engine/src/main.cpp src/zoom-engine-client.h src/zoom-engine-client.cpp src/zoom-control-server.cpp
git commit -m "feat(talkback): trigger the probe from the control API"
```

---

### Task 6: Live verification — THE GATE

Nothing before this proves anything. The compile proves the API exists; only this proves the account can use it.

**Files:**
- Create: `docs/superpowers/notes/2026-08-24-talkback-probe-results.md` (record the actual output)

**Interfaces:**
- Consumes: everything above.
- Produces: the go/no-go decision for Milestones 2–8.

- [ ] **Step 1: Install the matched pair**

Both binaries, always — half the fixes in any release are engine-side and a DLL-only copy silently half-applies. Close OBS, back up the installed pair, copy `obs-zoom-plugin.dll` **and** `zoom-runtime\ZoomObsEngine.exe`, verify with SHA256.

- [ ] **Step 2: Set up the meeting**

- A Zoom meeting hosted by the Enhanced Media entitled account.
- CoreVideo joined and **host or co-host** — this is the assumption under test.
- At least two other participants: one to be invited (the "talent"), one **not** invited (the control).
- Ask the talent to report what they hear; ask the control participant the same.

- [ ] **Step 2a: Safety notes -- read before running anything**

- **Use a dedicated TEST meeting. Never a live show.** The driving thread
  spawned to call `tick()` is the first thread in this engine that has ever
  called Zoom SDK APIs off the message-pumping thread. If the SDK's
  controller turns out to be thread-affine in some way that hasn't shown up
  before, the failure mode is unknown and it lands in the same process
  carrying the show. Milestone 1's whole purpose is finding out things like
  this safely, which requires a meeting where finding them out safely is
  possible.
- **`SetChannelBackgroundVolume(channel, 0.3f)` ducks the INVITED
  participant's main-meeting audio to 30%, and only destroying the channel
  undoes it.** If `destroy` is abandoned (`destroy_abandoned`) or the engine
  dies mid-probe, that person is stuck hearing the meeting at 30% volume
  until they leave and rejoin. Warn the invitee before you start that this
  will happen, and ask them to turn their own volume **down** first -- a 0.5
  full-scale 440Hz tone for 3 seconds straight into headphones is loud.
- **Use a participant display name with no double quotes or backslashes.**
  The JSON parser (`json_str`) skips escape sequences rather than
  unescaping them, so a name containing `"` or `\` will not resolve to the
  person you meant. This fails diagnosably -- you'll see `invite` with
  `error=no_participant_named` -- but there's no reason to spend a live
  meeting finding that out; pick a plain name up front.

- [ ] **Step 3: Run the probe**

```sh
printf '{"cmd":"talkback_probe","participant":"<exact display name>"}\n' | nc 127.0.0.1 19870
```

- [ ] **Step 4: Read every rung**

Expected in the OBS log on the happy path, in order:

```
talkback_probe stage=controller                    ok=true
talkback_probe stage=meeting_supported              supported=true
talkback_probe stage=set_event                      code=0
talkback_probe stage=create_channel                 code=0
talkback_probe stage=create_channel_response        channel=<id> error=0
talkback_probe stage=participant_talkback_support   name=<name> user_id=<n> supported=<bool>
talkback_probe stage=invite                         user_id=<n> code=0
talkback_probe stage=invite_response                channel=<id> user_id=<n> error=0
talkback_probe stage=background_volume              code=0
talkback_probe stage=send                           buffer=0 code=0
talkback_probe stage=sent                           buffers=300
talkback_probe stage=destroy                        code=0 attempt=1
talkback_probe stage=destroyed                      channel=<id> error=0
```

`participant_talkback_support` (added by the final-review F2 fix) fires from
inside `resolve_participant()`, which `onCreateChannelResponse` calls right
after it records the new channel id and before it builds the invite -- so it
lands between `create_channel_response` and `invite`, as shown above. Its
`supported` field is `IUserInfo::IsSupportTalkback()`, a PER-USER gate
distinct from the meeting-level `meeting_supported` above. **We do not
refuse to proceed when it is `false`** -- the send is attempted anyway, on
purpose: the contrast between `supported=false` here and what the talent
actually reports hearing in Step 5 is itself the data this milestone exists
to produce. If you see `supported=false` and the talent still hears the
tone, or `supported=true` and they hear nothing, both are meaningful
results -- write them down, don't discard them as noise.

That is the happy path only. The probe is a ladder with several ways to fall
off it, and reading the wrong-outcome logs correctly matters as much as
reading the green ones -- every stage the code can actually emit:

| stage | when |
|---|---|
| `controller` | RUNG 1/RUNG 2 report from `probe()`. `ok=false` with `reason=not_in_meeting` or `reason=no_meeting_service` means the probe never started; `ok=true` means the controller exists. |
| `meeting_supported` | RUNG 2, the meeting-level gate. `supported=false` is a hard stop -- Enhanced Media is not active for this meeting. |
| `set_event` | Registering this object for callbacks. Non-zero `code` is a hard stop. |
| `create_channel` | RUNG 3, `CreateChannel(1)` accepted (or not) synchronously. Non-zero `code` is a hard stop; zero only means the SDK accepted the request, not that a channel exists yet -- see `create_channel_response`. |
| `create_channel_response` | The async confirmation. `error=0` continues the ladder; any other `error` (see the `TalkbackError` table below) ends it. |
| `create_channel_response_null_channel` | **New in the final review (F1).** The SDK handed back a null `channelID` on this callback -- reachable on any `error` code. Terminal for the rung if we were waiting on it; otherwise a no-op (nothing to queue for cleanup with no id). |
| `create_channel_response_duplicate` | A redelivered callback for the channel the ladder already moved past `AwaitingChannel` with (e.g. mid-invite or mid-send already). No action taken; the live channel is untouched. |
| `create_channel_response_stray` | A genuinely different, untracked channel now exists (e.g. arrived after `timeout` already gave up on this rung). Queued for cleanup -- expect a later `stray_destroy` / `stray_destroy_abandoned` pair for the same channel id. |
| `participant_talkback_support` | **New in the final review (F2).** See above. |
| `invite` | RUNG 4. Either the success form (`user_id`, `code`) or the failure form `error=no_participant_named` when `resolve_participant()` found nobody by that exact display name -- see the display-name note below. |
| `invite_response` | The async confirmation. Always logged first, before the correlation check below runs. |
| `invite_response_null_channel` | **New in the final review (F1).** The SDK handed back a null `channelID` on this callback -- reachable on any `error` code, including `TALKBACK_ERROR_NOPERMISSION`. This is the exact failure this probe exists to be able to see rather than crash on. Terminal for the rung. |
| `invite_response_mismatch` | The callback's channel/user pair doesn't match what we're waiting on (a stray/duplicate/late response). No action taken. |
| `background_volume` | Ducking the invited participant's main-meeting audio to 30% once their invite is confirmed. See the safety note below -- this is not free to leave in place. |
| `send` | First buffer only (buffer=0), and any buffer that fails. 300 identical success lines would be the same message-storm shape as a prior live incident in this codebase, so mid-stream successes are not logged individually. |
| `sent` | All 300 buffers (~3s of tone) sent successfully. |
| `destroy` | Each attempt (up to 5) of the synchronous Begin/Add/Execute destroy chain for the main channel. Carries `attempt`. |
| `destroy_abandoned` | All 5 destroy attempts for the main channel failed. **The channel is now stranded for the rest of the meeting** -- one of the account's 16 gone for good, and the background-volume duck (if it was applied) is not undone. Treat this as a result to report, not something to retry manually mid-meeting. |
| `destroyed` | `onDestroyChannelResponse` -- the async confirmation that a channel (main or a drained stray) was actually torn down. Can appear more than once if a stray channel also existed. |
| `stray_destroy` | `drain_stray_channels()` (driven by `tick()`) attempting to destroy a queued stray. Carries `channel`, `code`, `attempts`. |
| `stray_destroy_abandoned` | A stray channel's destroy chain also exhausted its retries. Same "stranded for the rest of the meeting" consequence as `destroy_abandoned`, for a channel this probe didn't even mean to create. |
| `timeout` | `tick()` gave up waiting on `AwaitingChannel` or `AwaitingInvite` after 10s with no callback. Carries a numeric `phase`: **`1` = AwaitingChannel, `2` = AwaitingInvite** (verified against the `Phase` enum in `engine/src/engine-talkback.h`: `0` Idle, `1` AwaitingChannel, `2` AwaitingInvite, `3` Sending, `4` Destroying, `5` Done -- only 1 and 2 are reachable here, since those are the only phases `tick()` applies the deadline to). A hang IS silence and looks identical to a permission failure from the outside; this is what tells them apart. |
| `busy` | A second `talkback_probe` command arrived while a ladder was already in flight and was refused outright -- no new channel, no new thread. `reason` explains why; this is the normal, expected response to sending the command twice, not a fault. |

`TalkbackError` values (the `error`/`code` fields above): `0` OK, `1`
NOPERMISSION, `2` ALREADY_EXIST, `3` COUNT_OVERFLOW, `4` NOT_EXIST, `5`
REJECTED, `6` TIMEOUT, `7` UNKNOWN.

- [ ] **Step 5: Confirm with humans — the part no log can answer**

- The invited participant heard the ~3s tone **present**, as opposed to
  **absent**. That is the judgment to ask for -- not "was it clean". The
  driving thread sleeps 10ms per 10ms tone buffer, and Windows timer
  granularity (~15.6ms) means those sleeps routinely overrun, so the tone is
  delivered at roughly 64% of real time. **Choppiness or gappiness is
  expected in Milestone 1 and is not evidence of a fault** -- it is a
  consequence of the driving thread's plain `sleep_for`, not of the phase
  continuity Task 2 pins (that continuity guarantees the *waveform* has no
  discontinuity where two buffers join; it says nothing about how evenly
  those buffers get handed to the SDK in wall-clock time). Deadline-anchored
  pacing that would fix the choppiness is deferred to Milestone 2 -- do not
  read a choppy tone as a rung failing.
- The **non-invited** participant heard nothing at all. This is the privacy claim, and it is the single most important observation in the milestone.
- Nobody reports the tone on program audio.

- [ ] **Step 6: Repeat once without co-host**

Demote CoreVideo to plain participant and run the probe again. Record which rung fails and with what error. This tells Milestones 2–8 whether to gate the UI on host/co-host, and turns the spec's inference into a fact either way.

- [ ] **Step 7: Record the results and decide**

Write the actual log output, both human confirmations, and the without-co-host result into `docs/superpowers/notes/2026-08-24-talkback-probe-results.md`. State the verdict explicitly:

- **All rungs green + talent heard it + control heard nothing** → gate passed, write the Milestone 2–8 plan.
- **Any rung red** → gate failed. Report which rung and which error. **Do not work around it and do not start Milestone 2.** A NOPERMISSION at `create_channel` means the role assumption was wrong; a `supported=false` at `meeting_supported` means the entitlement assumption was wrong. Either changes the whole feature's viability, which is exactly what this milestone exists to discover.

- [ ] **Step 8: Commit**

```sh
git add docs/superpowers/notes/2026-08-24-talkback-probe-results.md
git commit -m "docs: talkback probe results — Milestone 1 gate"
```

---

## Self-Review

**Spec coverage for Milestone 1.** The spec's Milestone 1 reads: *"Engine only: get the controller, `IsMeetingSupportTalkBack()`, create one channel, invite one participant, send a generated tone, destroy."* Task 3 covers controller + gate; Task 4 covers create, invite, send, destroy; Task 2 covers the generated tone; Tasks 1 and 5 are the plumbing that makes it triggerable; Task 6 is the gate itself. The spec's requirement that participants be addressed **by name** is honoured in Task 4's `resolve_participant`. The spec's "surface gating, never swallow it" is honoured by every rung reporting its own error code.

**Deliberately out of scope**, deferred to the Milestone 2–8 plan: the talkback SHM ring, the OBS audio tap, the keying state machine and dead-man switch, the channel planner and the 16/10 caps arithmetic, all control surfaces beyond the probe trigger, the dock, the program-track warning, and the five test files listed in the spec's Testing section. None of them are worth writing until the gate passes.

**Placeholder scan:** none. Every code step contains the actual code. Task 3's "no unit test" and Task 4's "no unit test" are explicit statements about SDK-bound code with a named verification route (Task 6), not deferred work.

**Type consistency:** `talkback_tone_fill` has one signature, defined in Task 2 and called in Task 4 with matching argument order and types. `EngineTalkback::probe(IMeetingService*, const std::string&)` and `EngineTalkback::tick()` are declared in Task 3 and called in Task 5 with those exact signatures. `IPC_CMD_TALKBACK_PROBE` / `IpcCommand::TalkbackProbe` are defined in Task 1 and consumed in Tasks 4 and 5. `Phase` values are declared in Task 3's header and every value used in Task 4 (`AwaitingChannel`, `AwaitingInvite`, `Sending`, `Destroying`, `Done`) appears in that enum.

**One known soft spot, flagged rather than hidden:** Task 5's `tick()` thread is a detached bounded loop, which is acceptable for a probe but is *not* the shape the real feature should use — Milestone 4's dead-man switch replaces it. Do not carry the detached thread forward.
