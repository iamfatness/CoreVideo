# Zoom Talkback — Wiring It Up (Milestone 5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make talkback actually work — key on from the control API, and a nominated participant hears the director's voice from an OBS source.

**Architecture:** Talkback gets its **own persistent channel**, separate from the Milestone 1 probe's throwaway one. A plugin-side `TalkbackController` owns the OBS tap and the keying state, evaluates `talkback-key.h` on a timer, and drives the engine over the pipe. Channel creation stays on the engine's command-loop thread — the only thread that has ever created one — so the probe and talkback serialize naturally and a small ownership arbiter routes each `onCreateChannelResponse` to whoever asked for it.

**Tech Stack:** C++17, Qt (plugin-side timer), libobs, Zoom Windows Meeting SDK 7.1.5.43953, CMake + CTest, existing named-pipe line-JSON IPC + shared memory.

**Spec:** `docs/superpowers/specs/2026-08-24-zoom-talkback-design.md`

**Prior plans:** `2026-08-24-zoom-talkback-milestone-1.md` (the probe; gate PASSED live) and `2026-08-25-talkback-audio-path.md` (the audio path; built, reviewed, **unreachable** — this plan is what makes it reachable).

## Global Constraints

- **The audio path already exists and is reviewed. Do not redesign it.** `src/talkback-pcm.h`, `src/talkback-ring.h`, `src/talkback-key.h`, `src/talkback-tap.{h,cpp}`, and `EngineTalkback::open_audio/drain_audio/close_audio` are done. This plan wires them.
- **Talkback MUST NOT share the probe's channel.** The probe's channel is created, used for a 3-second tone, and destroyed by `tick()` on a separate thread. Sharing it is the race that this design exists to remove: `tick()` could destroy the channel `drain_audio` is mid-send on.
- **`CreateChannel` is called only from the engine's command-loop thread.** That is true today (`engine-talkback.cpp:100`, called from `main.cpp:1441`) and this plan keeps it true. Do not create a channel from `tick()` or from an SDK callback.
- Protected invariants, unchanged: `m_phase` stays `std::atomic<Phase>` and belongs to **the probe only**; `m_chan_mtx` guards channel-id state and the SDK is never called while holding it; `tick()` remains the sole caller of the batch-destroy API; the seqlock copy-inside-the-window in `talkback_ring_drain`; the six forbidden routing APIs in `talkback-tap.cpp`.
- **Fail closed everywhere.** A latch does not survive a reconnect. Every failure path closes the key.
- Participants are addressed **by name**, resolved to a meeting-scoped `userID` at use time. Never persist a raw Zoom user ID.
- Tests are plain executables, no framework, one `check()`-style file per invariant cluster in `tests/`, box-drawing section headers, registered with `add_executable` + `target_include_directories(... PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")` + `add_test`.
- Build: `cmake --build build_x64 --config Release --parallel 8`; test: `cd build_x64 && ctest -C Release --output-on-failure`. **`build_x64` is already configured — never delete or reconfigure it.**
- **Baseline is 59 tests.** Each task states the expected count.
- **OBS may be running.** Never kill it, install binaries, or touch Program Files.
- Comments state the constraint the code cannot show.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/talkback-channel-owner.h` (new) | Pure arbiter: which subsystem a `CreateChannel` response belongs to. No SDK. |
| `engine/src/engine-talkback.{h,cpp}` (modify) | Gains a persistent talkback session alongside the probe: its own channel id, its own lifecycle, no `Phase` involvement. |
| `engine/src/main.cpp` (modify) | Routes `talkback_start` / `talkback_stop`. |
| `src/talkback-controller.{h,cpp}` (new) | Plugin-side owner: the tap, the keying state, the timer, and the pipe commands. |
| `src/talkback-tap.cpp` (modify) | Sends `talkback_audio` on the notify edge — closes the `(void)notify;` gap. |
| `src/zoom-engine-client.{h,cpp}` (modify) | `talkback_start/open/audio/close/stop` senders. |
| `src/zoom-control-server.cpp` (modify) | `talkback_key`, `talkback_renew`, `talkback_status`. |
| `src/plugin-main.cpp` (modify) | Constructs the controller at `OBS_FRONTEND_EVENT_FINISHED_LOADING`; destroys it on unload. |

---

### Task 1: The channel-ownership arbiter

Two subsystems now ask the SDK for channels: the probe and talkback. `CreateChannel(1)` does not return an id — the id arrives later in `onCreateChannelResponse`. So the response carries no clue about who asked. Getting this wrong means the probe adopts talkback's channel (and destroys it three seconds later, mid-sentence) or talkback adopts the probe's.

The whole decision is one pure function, so it gets pinned before either caller exists.

**Files:**
- Create: `src/talkback-channel-owner.h`
- Create: `tests/talkback-channel-owner-test.cpp`
- Modify: `CMakeLists.txt` (register after the `CoreVideoTalkbackIsolation` block)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum class TalkbackChannelOwner { None, Probe, Session };`
  - `TalkbackChannelOwner talkback_claim_create(TalkbackChannelOwner pending);`
  - `bool talkback_may_request_create(TalkbackChannelOwner pending);`

- [ ] **Step 1: Write the failing test**

Create `tests/talkback-channel-owner-test.cpp`:

```cpp
// tests/talkback-channel-owner-test.cpp
// Who a CreateChannel response belongs to.
//
// Two subsystems now ask the Zoom SDK for talkback channels: the Milestone 1
// probe (creates one, sends a 3s tone, destroys it) and the talkback session
// (creates one and holds it open while a key is down). CreateChannel(1) does
// not return the id -- it arrives later in onCreateChannelResponse, which
// carries no indication of who asked.
//
// Get this wrong in one direction and the probe adopts the session's channel
// and destroys it three seconds later, cutting the director off mid-sentence.
// Get it wrong in the other and the session adopts the probe's channel, which
// tick() then destroys underneath it. Both are silent failures on a live show,
// so the decision is a pure function pinned here.
//
// The rule is deliberately strict: exactly ONE create may be outstanding. Both
// callers run on the engine's single command-loop thread, so serialising is
// free -- and a queue would only add a way for the two to interleave.
#include "talkback-channel-owner.h"

#include <iostream>

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
    // ── With nothing outstanding, either subsystem may ask ─────────────────
    check(talkback_may_request_create(TalkbackChannelOwner::None),
          "a create was refused while nothing was outstanding");

    // ── While one is outstanding, nobody else may ask ──────────────────────
    check(!talkback_may_request_create(TalkbackChannelOwner::Probe),
          "a second create was allowed while the probe's was outstanding");
    check(!talkback_may_request_create(TalkbackChannelOwner::Session),
          "a second create was allowed while the session's was outstanding");

    // ── The response goes to whoever is outstanding, and clears it ─────────
    check(talkback_claim_create(TalkbackChannelOwner::Probe) ==
              TalkbackChannelOwner::Probe,
          "the probe's create response was not routed to the probe");
    check(talkback_claim_create(TalkbackChannelOwner::Session) ==
              TalkbackChannelOwner::Session,
          "the session's create response was not routed to the session");

    // ── An UNEXPECTED response belongs to nobody ───────────────────────────
    // A late or duplicate response with nothing outstanding must NOT be
    // adopted. Adopting it would hand one subsystem a channel the other is
    // about to destroy -- and the SDK genuinely can redeliver.
    check(talkback_claim_create(TalkbackChannelOwner::None) ==
              TalkbackChannelOwner::None,
          "a create response arriving with nothing outstanding was adopted by "
          "somebody -- it must belong to nobody and be destroyed as a stray");

    if (failures == 0)
        std::cout << "talkback-channel-owner: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register it and confirm it fails**

In `CMakeLists.txt`, after the `add_test(NAME CoreVideoTalkbackIsolation ...)` block:

```cmake
    # Which subsystem a CreateChannel response belongs to. Two now ask (the
    # probe and the talkback session) and the response carries no clue; routing
    # it wrong cuts the director off mid-sentence or hands the session a
    # channel tick() is about to destroy. See src/talkback-channel-owner.h.
    add_executable(CoreVideoTalkbackChannelOwnerTest
        tests/talkback-channel-owner-test.cpp
    )
    target_include_directories(CoreVideoTalkbackChannelOwnerTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTalkbackChannelOwner
             COMMAND CoreVideoTalkbackChannelOwnerTest)
```

Run: `cmake --build build_x64 --config Release --target CoreVideoTalkbackChannelOwnerTest --parallel 8`
Expected: FAIL — `Cannot open include file: 'talkback-channel-owner.h'`.

- [ ] **Step 3: Implement**

Create `src/talkback-channel-owner.h`:

```cpp
#pragma once
//
// talkback-channel-owner.h — who a CreateChannel response belongs to.
//
// Two subsystems ask the Zoom SDK for talkback channels:
//
//   * the Milestone 1 PROBE, which creates one, sends a 3s tone, and destroys
//     it from tick() on its own driving thread;
//   * the talkback SESSION, which creates one and holds it open for as long
//     as a key is down.
//
// CreateChannel(1) does not return the channel id. It arrives later in
// onCreateChannelResponse, which says nothing about who asked. Route it wrong
// and either the probe adopts the session's channel (and destroys it three
// seconds later, mid-sentence) or the session adopts the probe's (which tick()
// then destroys underneath it). Both are silent on a live show.
//
// THE RULE: exactly one create may be outstanding at a time. That costs
// nothing, because both callers run on the engine's single command-loop
// thread -- CreateChannel has only ever been called from there
// (engine-talkback.cpp's probe(), reached from main.cpp's command loop), and
// this plan keeps it that way. A queue would buy nothing and would add a way
// for the two to interleave.
//
// Free of Qt / OBS / Zoom SDK dependencies so the routing can be pinned by a
// test with no engine and no meeting.
//
enum class TalkbackChannelOwner {
    // Nothing outstanding.
    None,
    // The Milestone 1 probe ladder.
    Probe,
    // The persistent talkback session.
    Session,
};

// May a subsystem issue CreateChannel right now?
inline bool talkback_may_request_create(TalkbackChannelOwner pending)
{
    return pending == TalkbackChannelOwner::None;
}

// Who owns the create response that just arrived, given what was outstanding.
//
// Returns None when nothing was outstanding: a late or duplicate response must
// be adopted by NOBODY and handled as a stray. The SDK can redeliver, and
// adopting a redelivered response would hand one subsystem a channel the other
// already owns.
//
// The caller clears its own pending state; this function does not mutate, so
// it stays a pure decision the test can drive exhaustively.
inline TalkbackChannelOwner talkback_claim_create(TalkbackChannelOwner pending)
{
    return pending;
}
```

- [ ] **Step 4: Confirm it passes**

```sh
cmake --build build_x64 --config Release --parallel 8
cd build_x64 && ctest -C Release --output-on-failure
```
Expected: **60/60** green.

- [ ] **Step 5: Commit**

```sh
git add src/talkback-channel-owner.h tests/talkback-channel-owner-test.cpp CMakeLists.txt
git commit -m "feat(talkback): arbiter for which subsystem owns a channel create"
```

---

### Task 2: The engine's persistent talkback session

A channel that lives while a key is down. Deliberately **not** part of the probe's `Phase` machine — that machine is built to tear itself down, which is the opposite of what this needs.

**Files:**
- Modify: `engine/src/engine-talkback.h`, `engine/src/engine-talkback.cpp`
- Modify: `src/engine-ipc.h` (two tokens), `src/engine-command.h` (routing)
- Modify: `tests/engine-command-test.cpp` (routing cases)

**Interfaces:**
- Consumes: `TalkbackChannelOwner` (Task 1).
- Produces:
  - `bool EngineTalkback::session_start(ZOOMSDK::IMeetingService *svc, const std::string &participant_name)`
  - `void EngineTalkback::session_stop()`
  - `bool EngineTalkback::session_live() const`
  - IPC tokens `IPC_CMD_TALKBACK_START` (`"talkback_start"`), `IPC_CMD_TALKBACK_STOP` (`"talkback_stop"`), and `IpcCommand::TalkbackStart` / `TalkbackStop`.

- [ ] **Step 1: Add the routing, TDD**

Append inside `main()` in `tests/engine-command-test.cpp`, before the final `if (failures == 0)` block:

```cpp
    // ── Talkback session commands route exactly ─────────────────────────────
    check(ipc_command_of(R"({"cmd":"talkback_start","participant":"Sarah Muller"})") ==
              IpcCommand::TalkbackStart,
          "talkback_start did not route to IpcCommand::TalkbackStart");
    check(ipc_command_of(R"({"cmd":"talkback_stop"})") == IpcCommand::TalkbackStop,
          "talkback_stop did not route to IpcCommand::TalkbackStop");
    // The talkback_* family now has six members sharing a prefix. Exact match
    // must keep every one of them apart.
    check(ipc_command_of(R"({"cmd":"talkback_start_extra"})") == IpcCommand::Unknown,
          "a longer command starting with talkback_start matched it");
    check(ipc_command_of(R"({"cmd":"talkback_stop_extra"})") == IpcCommand::Unknown,
          "a longer command starting with talkback_stop matched it");
```

Run the routing test; expected FAIL on the missing enum values. Then add to `src/engine-ipc.h`:

```c
#define IPC_CMD_TALKBACK_START "talkback_start"
#define IPC_CMD_TALKBACK_STOP  "talkback_stop"
```

to `src/engine-command.h`'s enum:

```cpp
    TalkbackStart,
    TalkbackStop,
```

and to `ipc_command_of`:

```cpp
    if (cmd == IPC_CMD_TALKBACK_START) return IpcCommand::TalkbackStart;
    if (cmd == IPC_CMD_TALKBACK_STOP)  return IpcCommand::TalkbackStop;
```

Re-run; expected PASS.

- [ ] **Step 2: Add the session state to the header**

In `engine/src/engine-talkback.h`, public:

```cpp
    // ── Persistent talkback session (Milestone 5) ──────────────────────────
    // Deliberately NOT part of the probe's Phase machine: that machine exists
    // to tear itself down after one tone, which is the opposite of what a key
    // held down needs. The session owns its OWN channel, so tick() -- which
    // destroys the PROBE's channel from a separate thread -- can never touch
    // it. That separation is the fix for the probe-thread race, and it is
    // structural rather than a lock.
    bool session_start(ZOOMSDK::IMeetingService *svc,
                       const std::string &participant_name);
    void session_stop();
    bool session_live() const;
```

private:

```cpp
    // Exactly one CreateChannel may be outstanding across the probe and the
    // session; see src/talkback-channel-owner.h for why. Command-loop thread
    // only -- both callers live there -- so it needs no synchronisation, and
    // that is stated here so nobody "helpfully" makes it atomic and hides the
    // threading contract.
    TalkbackChannelOwner       m_pending_create = TalkbackChannelOwner::None;
    std::basic_string<zchar_t> m_session_channel_z;   // guarded by m_chan_mtx
    std::string                m_session_channel;     // UTF-8, reporting only
    std::string                m_session_participant; // by NAME, re-resolved
    unsigned int               m_session_user_id = 0;
    bool                       m_session_live    = false;
```

Add `#include "../../src/talkback-channel-owner.h"` alongside the existing includes.

- [ ] **Step 3: Implement `session_start` / `session_stop` / `session_live`**

In `engine/src/engine-talkback.cpp`:

```cpp
bool EngineTalkback::session_live() const { return m_session_live; }

bool EngineTalkback::session_start(ZOOMSDK::IMeetingService *svc,
                                   const std::string &participant_name)
{
    if (m_session_live) {
        report("session_start", R"("ok":false,"reason":"already_live")");
        return false;
    }
    if (!svc) {
        report("session_start", R"("ok":false,"reason":"not_in_meeting")");
        return false;
    }
    m_svc  = svc;
    m_ctrl = m_svc->GetMeetingTalkbackController();
    if (!m_ctrl) {
        report("session_start", R"("ok":false,"reason":"no_controller")");
        return false;
    }
    if (!m_ctrl->IsMeetingSupportTalkBack()) {
        report("session_start", R"("ok":false,"reason":"not_supported")");
        return false;
    }
    m_ctrl->SetEvent(this);

    if (!talkback_may_request_create(m_pending_create)) {
        // The probe is mid-create. Refuse rather than queue: a queued create
        // would arrive with the other subsystem's response still in flight,
        // which is exactly the ambiguity the arbiter exists to remove.
        report("session_start", R"("ok":false,"reason":"create_busy")");
        return false;
    }

    m_session_participant = participant_name;
    const ZOOMSDK::SDKError e = m_ctrl->CreateChannel(1);
    report("session_start", R"("code":)" + std::to_string(static_cast<int>(e)) +
           R"(,"participant":")" + json_escape(participant_name) + "\"");
    if (e != ZOOMSDK::SDKERR_SUCCESS) return false;

    m_pending_create = TalkbackChannelOwner::Session;
    return true;
}

void EngineTalkback::session_stop()
{
    if (!m_session_live && m_session_channel_z.empty()) {
        // Nothing to tear down. Still clear the pending create so a refused
        // start cannot wedge the arbiter.
        if (m_pending_create == TalkbackChannelOwner::Session)
            m_pending_create = TalkbackChannelOwner::None;
        return;
    }

    std::basic_string<zchar_t> channel_copy;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        channel_copy = m_session_channel_z;
        m_session_channel_z.clear();
        m_session_channel.clear();
    }
    m_session_live    = false;
    m_session_user_id = 0;
    if (m_pending_create == TalkbackChannelOwner::Session)
        m_pending_create = TalkbackChannelOwner::None;

    if (channel_copy.empty() || !m_ctrl) {
        report("session_stop", R"("ok":true,"reason":"no_channel")");
        return;
    }

    // The session destroys its OWN channel here, on the command-loop thread.
    // This is NOT the batch-destroy path tick() owns for the probe's stray
    // queue -- keeping them separate is what keeps "tick() is the sole caller
    // of the batch-destroy API" true.
    ZOOMSDK::SDKError e = m_ctrl->BeginBatchDestroyChannels();
    if (e == ZOOMSDK::SDKERR_SUCCESS)
        e = m_ctrl->AddChannelToDestroy(channel_copy.c_str());
    if (e == ZOOMSDK::SDKERR_SUCCESS)
        e = m_ctrl->ExecuteBatchDestroyChannels();
    report("session_stop", R"("code":)" + std::to_string(static_cast<int>(e)));
}
```

- [ ] **Step 4: Route the create response through the arbiter**

In `onCreateChannelResponse`, BEFORE the existing probe handling, insert:

```cpp
    // Route by who asked. See src/talkback-channel-owner.h: the response
    // carries no indication of its requester, so the arbiter is the only
    // thing standing between the probe and the session adopting each other's
    // channels.
    const TalkbackChannelOwner owner = talkback_claim_create(m_pending_create);
    if (owner == TalkbackChannelOwner::Session) {
        m_pending_create = TalkbackChannelOwner::None;
        if (error != TALKBACK_ERROR_OK || channelID == nullptr) {
            report("session_channel", R"("ok":false,"error":)" +
                   std::to_string(static_cast<int>(error)));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            m_session_channel_z.assign(channelID);
            m_session_channel = zchar_to_utf8(channelID);
        }
        // Invite by NAME, resolved now: Zoom user ids are meeting-scoped, so
        // a stored id points at nobody after a rejoin and at the wrong face
        // once ids are recycled.
        m_session_user_id = resolve_participant(m_session_participant);
        if (m_session_user_id == 0) {
            report("session_invite", R"("ok":false,"reason":"no_participant_named",)"
                   R"("name":")" + json_escape(m_session_participant) + "\"");
            session_stop();
            return;
        }
        std::basic_string<zchar_t> channel_copy;
        {
            std::lock_guard<std::mutex> lock(m_chan_mtx);
            channel_copy = m_session_channel_z;
        }
        ZOOMSDK::SDKError e = m_ctrl->BeginBatchInviteUsers(channel_copy.c_str());
        if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->AddUserToInvite(m_session_user_id);
        if (e == ZOOMSDK::SDKERR_SUCCESS) e = m_ctrl->ExecuteBatchInviteUsers();
        report("session_invite", R"("user_id":)" + std::to_string(m_session_user_id) +
               R"(,"code":)" + std::to_string(static_cast<int>(e)));
        if (e != ZOOMSDK::SDKERR_SUCCESS) { session_stop(); return; }
        m_ctrl->SetChannelBackgroundVolume(channel_copy.c_str(), 0.3f);
        m_session_live = true;
        report("session_live", R"("channel":")" + json_escape(m_session_channel) + "\"");
        return;
    }
    if (owner == TalkbackChannelOwner::Probe)
        m_pending_create = TalkbackChannelOwner::None;
```

Then, in `probe()`, set `m_pending_create = TalkbackChannelOwner::Probe;` immediately after its successful `CreateChannel`, and gate that `CreateChannel` behind `talkback_may_request_create(m_pending_create)` exactly as `session_start` does — refusing with a `busy` report rather than queueing.

- [ ] **Step 5: Send audio to the SESSION's channel, not the probe's**

In `drain_audio`, replace the `m_channel_id_z` copy with `m_session_channel_z`, under the same lock:

```cpp
    std::basic_string<zchar_t> channel_copy;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        channel_copy = m_session_channel_z;
    }
```

Update the surrounding comment: talkback now has its own channel, so the probe's `tick()` can never destroy the one being sent on — that was the race, and it is now structurally impossible rather than merely unlikely.

- [ ] **Step 6: Route the commands in `main.cpp`**

Alongside the existing talkback branches:

```cpp
        } else if (command == IpcCommand::TalkbackStart) {
            talkback.session_start(meeting_svc, json_str(line, "participant"));

        } else if (command == IpcCommand::TalkbackStop) {
            talkback.session_stop();
```

Add `talkback.session_stop();` to the `Leave` and quit paths, beside the existing `close_audio()` calls.

- [ ] **Step 7: Build and test**

```sh
cmake --build build_x64 --config Release --parallel 8
cd build_x64 && ctest -C Release --output-on-failure
```
Expected: **60/60** green.

- [ ] **Step 8: Commit**

```sh
git add engine/src/engine-talkback.h engine/src/engine-talkback.cpp engine/src/main.cpp src/engine-ipc.h src/engine-command.h tests/engine-command-test.cpp
git commit -m "feat(talkback): a persistent session channel, separate from the probe"
```

---

### Task 3: Close the notify gap — the tap sends its pipe events

`src/talkback-tap.cpp` computes `notify` and throws it away. This is the line that makes the whole audio path unreachable.

**Files:**
- Modify: `src/talkback-tap.cpp`, `src/talkback-tap.h`
- Modify: `src/zoom-engine-client.h`, `src/zoom-engine-client.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `ZoomEngineClient::talkback_open(const std::string &region, uint32_t rate, uint16_t channels)`, `::talkback_audio()`, `::talkback_close()`, `::talkback_start(const std::string &participant)`, `::talkback_stop()`.

- [ ] **Step 1: Add the senders**

In `src/zoom-engine-client.h`, beside `talkback_probe`:

```cpp
    void talkback_start(const std::string &participant_name);
    void talkback_stop();
    void talkback_open(const std::string &region, uint32_t rate, uint16_t channels);
    void talkback_audio();
    void talkback_close();
```

In `src/zoom-engine-client.cpp`, following `talkback_probe`'s shape exactly (including the `m_running` guard and `json_escape`):

```cpp
void ZoomEngineClient::talkback_start(const std::string &participant_name)
{
    if (!m_running.load(std::memory_order_acquire)) return;
    write_json(R"({"cmd":"talkback_start","participant":")" +
               json_escape(participant_name) + "\"}");
}

void ZoomEngineClient::talkback_stop()
{
    if (!m_running.load(std::memory_order_acquire)) return;
    write_json(R"({"cmd":"talkback_stop"})");
}

void ZoomEngineClient::talkback_open(const std::string &region, uint32_t rate,
                                     uint16_t channels)
{
    if (!m_running.load(std::memory_order_acquire)) return;
    write_json(R"({"cmd":"talkback_open","region":")" + json_escape(region) +
               R"(","rate":)" + std::to_string(rate) +
               R"(,"channels":)" + std::to_string(channels) + "}");
}

void ZoomEngineClient::talkback_audio()
{
    if (!m_running.load(std::memory_order_acquire)) return;
    write_json(R"({"cmd":"talkback_audio"})");
}

void ZoomEngineClient::talkback_close()
{
    if (!m_running.load(std::memory_order_acquire)) return;
    write_json(R"({"cmd":"talkback_close"})");
}
```

- [ ] **Step 2: Send the notify edge from the tap**

In `src/talkback-tap.cpp`'s `on_audio`, replace the `(void)notify;` block with:

```cpp
    // THE EDGE, at last. talkback_ring_publish returns true exactly when this
    // publish crossed empty -> non-empty and one event must be sent. Sending
    // one per BUFFER instead would be ~100 pipe lines/sec -- the message-storm
    // shape this codebase has a live incident about, and the reason the ring
    // is edge-triggered at all.
    //
    // This runs on the OBS capture thread, so it must not block: write_json
    // is a non-blocking pipe write that drops on a broken link, and a dropped
    // edge is recovered by the dead-man switch closing the key rather than by
    // retrying here.
    if (notify) ZoomEngineClient::instance().talkback_audio();
```

Add `#include "zoom-engine-client.h"`.

In `TalkbackTap::open()`, after the region is created and `talkback_ring_init` has run, send the open:

```cpp
    ZoomEngineClient::instance().talkback_open(m_region_name, rate, chans);
```

and in `close()`, after the callback is removed and before the region is destroyed:

```cpp
    ZoomEngineClient::instance().talkback_close();
```

**Ordering matters and must be commented:** `talkback_open` is sent only after `talkback_ring_init` has laid out the header, because the engine validates `slot_count`/`slot_bytes` from it and would reject a region it mapped too early.

- [ ] **Step 3: Build and test**

Expected: **60/60** green (no new tests; this is wiring).

- [ ] **Step 4: Commit**

```sh
git add src/talkback-tap.cpp src/talkback-tap.h src/zoom-engine-client.h src/zoom-engine-client.cpp
git commit -m "feat(talkback): the tap sends its pipe events -- audio path reachable"
```

---

### Task 4: The plugin-side controller

Owns the tap, the keying state, and the timer that evaluates `talkback-key.h`. This is the first caller `talkback-key.h` has ever had.

**Files:**
- Create: `src/talkback-controller.h`, `src/talkback-controller.cpp`
- Modify: `CMakeLists.txt` (add to the plugin sources beside `src/talkback-tap.cpp`)
- Modify: `src/plugin-main.cpp` (construct at `OBS_FRONTEND_EVENT_FINISHED_LOADING`, destroy on unload)

**Interfaces:**
- Consumes: `TalkbackTap` (Task 3's senders), `talkback_key_evaluate` and `TalkbackKeyState` from `src/talkback-key.h`.
- Produces: `TalkbackController::instance()`, `bool key_on(const std::string &participant, const std::string &source, TalkbackKeyMode mode, bool needs_renewal, std::string &error_out)`, `void key_off()`, `void renew()`, `std::string status_json() const`.

- [ ] **Step 1: Write the header**

Create `src/talkback-controller.h`:

```cpp
#pragma once
//
// talkback-controller.h — the plugin's talkback owner.
//
// Holds the three things that must agree for a key to be open: the OBS tap
// (audio is flowing), the engine session (a channel exists and someone is
// invited), and the keying state (the operator still wants it). Every surface
// -- control API today, Companion and a hotkey later -- goes through here, so
// there is exactly one place that can open or close a key.
//
// The dead-man switch lives in src/talkback-key.h and is evaluated on a timer
// here. Audio arriving IS the liveness signal: while a key is open the tap
// publishes continuously (including silence, because an active OBS source
// calls back whether or not anyone is talking), so a gap means the path is
// gone and the key closes with nothing having to NOTICE the failure.
//
#include "talkback-key.h"
#include "talkback-tap.h"

#include <QObject>
#include <QTimer>

#include <mutex>
#include <string>

class TalkbackController : public QObject {
    Q_OBJECT
public:
    static TalkbackController &instance();

    // Opens a key. Returns false with a human-readable reason -- an operator
    // whose key did not open needs to know WHICH thing failed.
    bool key_on(const std::string &participant, const std::string &source,
                TalkbackKeyMode mode, bool needs_renewal,
                std::string &error_out);
    void key_off();
    // Re-assert an open key. The lost-release backstop: a key opened over the
    // control API stays open only while the controller keeps saying it is
    // still wanted. See src/talkback-key.h.
    void renew();

    std::string status_json() const;

private slots:
    void evaluate();

private:
    TalkbackController();

    mutable std::mutex m_mtx;
    TalkbackTap        m_tap;
    TalkbackKeyState   m_key{};
    std::string        m_participant;
    std::string        m_source;
    QTimer            *m_timer = nullptr;
};
```

- [ ] **Step 2: Write the implementation**

Create `src/talkback-controller.cpp`:

```cpp
#include "talkback-controller.h"
#include "zoom-engine-client.h"

#include <obs-module.h>
#include <util/platform.h>

static uint64_t now_ms() { return os_gettime_ns() / 1000000ULL; }

TalkbackController &TalkbackController::instance()
{
    static TalkbackController c;
    return c;
}

TalkbackController::TalkbackController()
{
    m_timer = new QTimer(this);
    // A tenth of the audio-gap window, so a dead path is noticed within a
    // couple of ticks rather than a couple of gaps.
    m_timer->setInterval(static_cast<int>(kTalkbackAudioGapMs / 10));
    connect(m_timer, &QTimer::timeout, this, &TalkbackController::evaluate);
    m_timer->start();
}

bool TalkbackController::key_on(const std::string &participant,
                                const std::string &source,
                                TalkbackKeyMode mode, bool needs_renewal,
                                std::string &error_out)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_key.open) { error_out = "A talkback key is already open"; return false; }
    if (participant.empty()) { error_out = "No participant named"; return false; }
    if (source.empty())      { error_out = "No OBS audio source chosen"; return false; }

    // Order matters: the engine must have a channel before audio arrives, and
    // the tap must be laid out before the engine maps its region. Start the
    // session first, then open the tap (which sends talkback_open itself).
    ZoomEngineClient::instance().talkback_start(participant);
    if (!m_tap.open(source, error_out)) {
        ZoomEngineClient::instance().talkback_stop();
        return false;
    }

    m_participant = participant;
    m_source      = source;
    m_key.open            = true;
    m_key.mode            = mode;
    m_key.needs_renewal   = needs_renewal;
    m_key.last_audio_ms   = now_ms();
    m_key.last_renewal_ms = now_ms();
    blog(LOG_INFO, "[obs-zoom-plugin] talkback: key OPEN to \"%s\" via \"%s\"",
         participant.c_str(), source.c_str());
    return true;
}

void TalkbackController::key_off()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (!m_key.open) return;
    m_key.open = false;
    m_tap.close();                                  // sends talkback_close
    ZoomEngineClient::instance().talkback_stop();
    blog(LOG_INFO, "[obs-zoom-plugin] talkback: key CLOSED");
}

void TalkbackController::renew()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_key.open) m_key.last_renewal_ms = now_ms();
}

void TalkbackController::evaluate()
{
    TalkbackKeyAction action = TalkbackKeyAction::None;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_key.open) return;
        // The dead-man switch reads the tap's own last-publish stamp, not a
        // value this class maintains: the tap is the thing that knows whether
        // audio is actually still flowing.
        m_key.last_audio_ms = m_tap.last_audio_ms();
        action = talkback_key_evaluate(
            m_key, now_ms(),
            ZoomEngineClient::instance().is_running(),
            ZoomEngineClient::instance().state() == MeetingState::InMeeting);
    }
    if (action == TalkbackKeyAction::Close) {
        blog(LOG_WARNING, "[obs-zoom-plugin] talkback: key closed by the "
                          "dead-man switch (audio stopped, engine gone, or the "
                          "meeting ended)");
        key_off();
    }
}

std::string TalkbackController::status_json() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return std::string(R"({"open":)") + (m_key.open ? "true" : "false") +
           R"(,"participant":")" + m_participant +
           R"(","source":")" + m_source +
           R"(","tap_open":)" + (m_tap.is_open() ? "true" : "false") + "}";
}
```

**VERIFIED for you — use exactly these:** `ZoomEngineClient::state()` and `::is_running()` exist as written (`src/zoom-engine-client.h:136-137`). **But there are TWO `MeetingState` enums in this tree** — `src/zoom-meeting.h:10` and `src/zoom-types.h:7` (the latter has an extra `Recovering`). `zoom-engine-client.h:5` includes **`zoom-types.h`**, so that is the authoritative one here. Do not include `zoom-meeting.h` in this file and do not compare against a value that exists in only one of the two.

`key_off()` takes `m_mtx` and is also called from `evaluate()`, which already released it — confirm there is no recursive lock. If the structure forces one, restructure so the lock is not held across `key_off()`; do not switch to a recursive mutex.

- [ ] **Step 3: Construct it at plugin load**

In `src/plugin-main.cpp`, inside the `OBS_FRONTEND_EVENT_FINISHED_LOADING` handler (beside the dock registrations), touch the singleton so its timer starts on the Qt main thread:

```cpp
    TalkbackController::instance();
```

Add `src/talkback-controller.cpp` to the plugin sources in `CMakeLists.txt`. The class uses `Q_OBJECT`; **AUTOMOC is already ON** (`CMakeLists.txt:50`), so no extra wiring is needed.

- [ ] **Step 4: Build and test**

Expected: **60/60** green.

- [ ] **Step 5: Commit**

```sh
git add src/talkback-controller.h src/talkback-controller.cpp src/plugin-main.cpp CMakeLists.txt
git commit -m "feat(talkback): plugin-side controller with the dead-man switch wired"
```

---

### Task 5: The control-API surface

**Files:**
- Modify: `src/zoom-control-server.cpp`, `src/zoom-control-parse.h` (the known-commands list and its size guard)

**Interfaces:**
- Consumes: `TalkbackController` (Task 4).
- Produces: control API commands `talkback_key`, `talkback_renew`, `talkback_status`.

- [ ] **Step 1: Add the commands**

Following the file's existing branch and response style exactly (read several neighbours first — do NOT copy this sketch literally if it disagrees with the real conventions):

```cpp
    if (cmd == "talkback_key") {
        const std::string who    = json_str(body, "participant");
        const std::string source = json_str(body, "source");
        const std::string state  = json_str(body, "state");
        const bool latch = json_str(body, "mode") == "latch";
        if (state == "off") { TalkbackController::instance().key_off();
                              return R"({"ok":true,"open":false})"; }
        std::string err;
        const bool ok = TalkbackController::instance().key_on(
            who, source,
            latch ? TalkbackKeyMode::Latch : TalkbackKeyMode::PushToTalk,
            true, err);
        return ok ? R"({"ok":true,"open":true})"
                  : std::string(R"({"ok":false,"error":")") + err + "\"}";
    }
    if (cmd == "talkback_renew") {
        TalkbackController::instance().renew();
        return R"({"ok":true})";
    }
    if (cmd == "talkback_status") {
        return TalkbackController::instance().status_json();
    }
```

`needs_renewal` is `true` for the control API: a key opened over a socket must be re-asserted, because a lost release leaves buffers flowing and the audio gap can never fire.

- [ ] **Step 2: Update the known-commands list and its size guard**

`known_control_commands()` is `std::array<const char *, 21>` (`src/zoom-control-parse.h:91`). Add all three and change the array size to **24**. Leaving it stale makes the new commands invisible to `{"cmd":"help"}` and makes the size guard's own stated purpose false.

- [ ] **Step 3: Build and test**

Expected: **60/60** green, including the known-commands size guard.

- [ ] **Step 4: Commit**

```sh
git add src/zoom-control-server.cpp src/zoom-control-parse.h
git commit -m "feat(talkback): control API can key talkback on and off"
```

---

### Task 6: Live verification — THE GATE

**Files:**
- Create: `docs/superpowers/notes/2026-08-25-talkback-live-results.md`

- [ ] **Step 1: Install the matched pair**

Both binaries, always. Close OBS, back up the installed pair, copy `obs-zoom-plugin.dll` AND `zoom-runtime\ZoomObsEngine.exe`, verify with SHA256. **Requires the operator's UAC.**

- [ ] **Step 2: Set up**

- A meeting hosted by the Enhanced Media entitled account, CoreVideo **host or co-host** (proven required — a plain participant is refused with `SDKERR_NO_PERMISSION`).
- An OBS audio source for talkback. **Check its mixer tracks first** — the tap logs a warning if any program track is enabled, and that is the one failure that puts the director on the stream entirely outside our code.
- One participant to invite, and — this is the part still unproven from Milestone 1 — **one participant deliberately NOT invited.**

- [ ] **Step 3: Key on**

```sh
printf '{"cmd":"talkback_key","participant":"<name>","source":"<obs source>","state":"on","mode":"latch"}\n' | nc 127.0.0.1 19870
```
Renew every ~500 ms while open (`{"cmd":"talkback_renew"}`), or the lost-release backstop closes it after ~1 s. Speak. Then `{"cmd":"talkback_key","state":"off"}`.

- [ ] **Step 4: Confirm with humans — the part no log can answer**

- The invited participant heard the director, intelligibly.
- The **non-invited** participant heard **nothing**. This is the exclusivity claim, unproven since Milestone 1, and the single most important observation of the whole feature.
- Program output and any ISO recording from the same session contain no talkback audio.

- [ ] **Step 5: Exercise the dead-man switch live**

Kill the engine mid-key, or stop the OBS source, and confirm the key closes on its own within ~250 ms with the warning logged. A fail-closed design that has never been seen to fail closed is not evidence.

- [ ] **Step 6: Record and decide**

Write the actual log output, both human confirmations, and the dead-man result into the notes file. State the verdict plainly. If exclusivity fails, **stop** — that is the feature's core promise, and nothing else matters until it holds.

---

## Self-Review

**Spec coverage.** This plan delivers the spec's "one talkback channel with an invited participant, keyed from a surface, failing closed." Task 1 is the arbiter the two-subsystem design needs; Task 2 the session; Task 3 closes the wiring gap; Task 4 gives `talkback-key.h` its first caller; Task 5 one surface; Task 6 the gate.

**Deliberately out of scope**, deferred: the full 16/10 caps arithmetic and multi-channel fan-out, the per-person override, Companion and hotkey surfaces, the dock's config/tally/program-track UI, and deadline-anchored tone pacing.

**Resolved by design rather than deferred:** the probe-thread race. Talkback owning its own channel means `tick()` — which destroys the probe's channel from another thread — can never touch the session's. That is structural, not a lock.

**Placeholder scan:** none. Two steps (Task 4's `ZoomEngineClient` accessors, Task 5's control-server style) explicitly instruct the implementer to read the real code and use the real names rather than the sketch — that is a verification instruction, not a placeholder, and it exists because three earlier plans in this series shipped wrong helper names.

**Type consistency:** `TalkbackChannelOwner` and its two functions are defined in Task 1 and consumed in Task 2. `session_start/session_stop/session_live` are declared in Task 2 Step 2 and used in Step 6. The five `ZoomEngineClient` senders are defined in Task 3 and called in Tasks 3 and 4. `TalkbackKeyState`/`TalkbackKeyMode`/`talkback_key_evaluate` come from the existing `src/talkback-key.h` and are used in Tasks 4 and 5 with its real signature.

**Known risk, stated rather than hidden:** `TalkbackController::key_off()` acquires `m_mtx` and is called from `evaluate()`, which must therefore release it first. The plan says so explicitly and forbids a recursive mutex. A reviewer should check this specifically — it is the most likely place for this milestone to deadlock the OBS UI thread.
