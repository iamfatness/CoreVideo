# Screen Share as a First-Class Source — Gap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the four real gaps that keep screen share from being a first-class source — share computer audio (dropped entirely today), a share ISO recording stem (explicitly excluded today), the `capture_ns` stamp the share SHM writer never writes, and a `screen_share` start/stop event on the control API and in Companion — without re-implementing the large share pipeline that already exists and works.

**Architecture:** The engine (`ZoomObsEngine.exe`) already subscribes the active share via `IZoomSDKRenderer::subscribe(shareSourceID, RAW_DATA_TYPE_SHARE)` and publishes I420 frames into a generation-named SHM mailbox that the plugin's `zoom_share_source` reads. This plan extends the existing seams only: share audio rides the existing per-uuid audio ring (a new `share_audio` routing flag in `EngineAudio`, filled by the currently-stubbed `onShareAudioRawDataReceived`), the ISO stem rides the existing `record_video_frame`/`record_audio_frame` path (a one-predicate policy change plus a share-idle close), and the operator event rides the control server's existing 250 ms `poll_and_push()` edge detector.

**Tech Stack:** C++17, Zoom Windows Meeting SDK 7.1.5.43953, CMake + CTest, named-pipe line-JSON IPC (`src/engine-ipc.h`), named shared memory, Qt6 for dock UI, TypeScript (`@companion-module/base` 2.x, vitest) for the Companion module.

**Spec:** This document doubles as the spec. Requirements, with their current status stated honestly:

1. **`RAW_DATA_TYPE_SHARE` subscription — ALREADY FULLY IMPLEMENTED.** `engine/src/engine-share.cpp` owns the whole lifecycle: attach on join (`engine/src/main.cpp:1024-1030`), scan `GetViewableSharingUserList()`/`GetSharingSourceInfoList()` for the active `shareSourceID` (`active_share_source_id()`), subscribe via `createRenderer` + `setRawDataResolution(ZoomSDKResolution_1080P)` + `subscribe(id, RAW_DATA_TYPE_SHARE)` (`engine-share.cpp:188-227`), and follow share begin/end/switch through `onSharingStatus`/`onShareContentNotification` (`engine-share.cpp:395-438`). Not re-planned. (SDK note: `ZoomSDKResolution` tops out at `ZoomSDKResolution_1080P` — there is no 4K entry in `third_party/zoom-sdk/h/rawdata_renderer_interface.h:12-20`, so 1080p is the most we can request; that is an SDK ceiling, not a gap.)
2. **Dedicated "content" OBS source — ALREADY FULLY IMPLEMENTED.** `zoom_share_source` is registered in `src/zoom-source.cpp:2867-2872` as a `ZoomSource` variant defaulting to `AssignmentMode::ScreenShare`; assignment plumbing exists in the dock, output dialog, control API (`assign_output_ex` mode `screen_share`), OSC, and Companion's `zoom_assign_screen_share` action. Not re-planned. (`src/zoom-share-delegate.cpp` contains an older in-process duplicate whose `zoom_share_source_register()` has **no call site** — dead registration code. Deleting it is out of scope here; it must not be resurrected by this work.)
3. **Dynamic share resolution — ALREADY FULLY IMPLEMENTED.** The engine's `ensure_shm()` moves to a new `_gN`-suffixed region on any growth (`engine-share.cpp:248-276`, per `src/shm-generation.h`), the frame event carries `shm_gen`, and the plugin's `shm_read_i420_frame()` remaps on generation change (`src/engine-ipc.h:604-677`). The ISO side re-segments on any width/height change (`ensure_session_locked`, `src/zoom-iso-recorder.cpp:467-479`). Not re-planned.
4. **Share audio — GAP.** `EngineAudio::onShareAudioRawDataReceived` is an empty stub (`engine/src/engine-audio.cpp:418`); every sample of shared computer sound is dropped on the engine path. Tasks 1 closes this end to end.
5. **`capture_ns` — GAP**, documented as such in `src/engine-ipc.h:117-122`: the share writer never stamps `ShmFrameHeader::capture_ns`, so a share output shows "-" for A/V Offset forever. Task 2.
6. **Share ISO stem — GAP.** `ZoomIsoRecorder::should_record()` hard-excludes `AssignmentMode::ScreenShare` (`src/zoom-iso-recorder.cpp:452`) even though share frames already reach `record_video_frame()` (`src/zoom-source.cpp:1617`). Task 3.
7. **Share start/stop events to the operator — PARTIAL.** Dock (roster "sharing" tags, `screen_share_assignment_label`, `ScreenShareUnavailable` health), control API polling (`list_participants` → `is_sharing_screen`; `list_outputs` → `screen_share_available`/`_participant_id`/`_participant_name`), and OSC push (`/zoom/event/screen_share`, `src/zoom-osc-server.cpp:938`) all exist. Missing: a pushed `screen_share` event on the TCP control API's `subscribe_events` stream (Task 4) and any Companion feedback/variable for share state (Task 5 — today Companion has only the assign action).

## Global Constraints

- Build from the worktree: `cmake --build build --config Release --parallel 8`; test: `cd build && ctest -C Release --output-on-failure` — must be N/N green (63 tests registered today; 65 after this plan).
- Tests are plain executables, no framework: one `check()`-style file per invariant cluster in `tests/`, registered in `CMakeLists.txt` with `add_executable` + `add_test`. SDK-bound code that cannot be unit-tested says so and names its live verification instead.
- Comments state the constraint the code cannot show; when a change is motivated by a live failure, say what happened, with numbers.
- **Media events are prompts, not payloads** (CLAUDE.md): a share frame event means "read the newest frame", an audio event means "drain the ring". Nothing in this plan may put media payload semantics on the pipe.
- Audio ring discipline is untouchable: free-running indices, edge-triggered `notify`, drain-fully-on-wakeup (`src/engine-ipc.h`). Share audio reuses `output_audio_frame()` verbatim rather than growing a sibling.
- ISO pacing doctrine (`src/iso-video-pacer.h`): `-use_wallclock_as_timestamps` is proven a no-op on this ffmpeg build; every video stem is paced to `kIsoVideoTargetFps` before the pipe, and share inherits that path unchanged.
- Never run a second OBS instance while one is testing (pipe/SDK singleton collision, crash loop). Send `{"cmd":"leave"}` before closing OBS.
- Install verification is always the matched pair — `obs-zoom-plugin.dll` AND `zoom-runtime\ZoomObsEngine.exe`.
- Update CLAUDE.md in the same change as the substantive work (standing directive).

---

### Task 1: Share computer audio, end to end

The engine subscribes one process-wide raw-audio helper and fans buffers out by per-target flags (`isolate_audio`, `audience_audio`, default mixed — `engine/src/engine-audio.cpp:373-417`). Share audio needs a third flag, not a new pipeline: `onShareAudioRawDataReceived` fires only while someone shares with computer sound, exactly the shape of `onOneWayAudioRawDataReceived`. The plugin's side already works untouched — a share-assigned `ZoomSource` carries `OBS_SOURCE_AUDIO`, and its generic per-uuid audio-ring reader (`output_audio_from_shared_memory`) neither knows nor cares which engine callback filled the ring. The opt-in must be explicit on the wire (`"share_audio":true`) so an old plugin against a new engine keeps today's behavior byte-for-byte, and so mixed targets never double-receive share sound (the meeting mix already contains it).

**Files:**
- Modify: `src/engine-command.h:121-124` (new parse helper beside `ipc_subscribe_is_video_only`)
- Modify: `engine/src/engine-audio.h:33-37` (init signature), `:55-83` (AudioTarget), `engine/src/engine-audio.cpp:14-49` (init), `:373-387` (mixed skip), `:418` (the stub)
- Modify: `engine/src/main.cpp:1793-1794` (screenshare subscribe branch)
- Modify: `src/zoom-engine-client.cpp:868-873` (`subscribe_screenshare` emits the field)
- Test: `tests/engine-command-test.cpp`

**Interfaces:**
- Consumes: `EngineAudio::output_audio_frame(AudioTarget&, const std::string&, AudioRawData*, const char*)` (existing, unchanged).
- Produces: `inline bool ipc_subscribe_wants_share_audio(const std::string &line)`; `bool EngineAudio::init(IpcFd e2p_fd, const std::string &source_uuid, uint32_t participant_id, bool isolate_audio, bool audience_audio, bool share_audio = false)`. Task 3's share ISO stem consumes the audio this task makes flow (via `record_audio_frame`, no new interface).

- [ ] **Step 1: Write the failing test**

Append inside `main()` in `tests/engine-command-test.cpp`, before the final `if (failures == 0)` block:

```cpp
    // --- Share audio is an explicit opt-in on the screenshare subscribe ---
    // The engine's onShareAudioRawDataReceived was a stub until 2026-08-29;
    // when it started delivering, the opt-in had to be explicit so an old
    // plugin (which never sends the field) keeps the audio-less behavior it
    // was built against, and so mixed targets never hear share sound twice
    // (the meeting mix already contains it).
    check(ipc_subscribe_wants_share_audio(
              R"({"cmd":"subscribe","source_uuid":"s1","mode":"screenshare","share_audio":true})"),
          "share_audio:true on a screenshare subscribe was not detected");
    check(!ipc_subscribe_wants_share_audio(
              R"({"cmd":"subscribe","source_uuid":"s1","mode":"screenshare"})"),
          "a subscribe without share_audio must not register an audio target");
    check(!ipc_subscribe_wants_share_audio(
              R"({"cmd":"subscribe","source_uuid":"s1","mode":"screenshare","share_audio":false})"),
          "share_audio:false was treated as an opt-in");
```

- [ ] **Step 2: Run test to verify it fails**

```sh
cmake --build build --config Release --target CoreVideoEngineCommandTest --parallel 8
```

Expected: FAIL to compile with `'ipc_subscribe_wants_share_audio': identifier not found`.

- [ ] **Step 3: Write minimal implementation**

In `src/engine-command.h`, immediately after `ipc_subscribe_is_video_only` (line 124):

```cpp
// Share computer sound for a screenshare subscribe. Same match discipline as
// ipc_subscribe_is_video_only above. Explicit opt-in, default OFF: the field
// is absent from every subscribe an older plugin sends, and absent-means-off
// is what keeps a new engine byte-compatible with it.
inline bool ipc_subscribe_wants_share_audio(const std::string &line)
{
    return line.find("\"share_audio\":true") != std::string::npos;
}
```

In `engine/src/engine-audio.h`, change the `init` declaration (line 33) to:

```cpp
    bool init(IpcFd e2p_fd,
              const std::string &source_uuid,
              uint32_t participant_id,
              bool isolate_audio,
              bool audience_audio,
              bool share_audio = false);
```

and in `AudioTarget` change the constructor and add the flag:

```cpp
        AudioTarget(IpcFd e2p, uint32_t pid, bool isolate, bool audience,
                    bool share)
            : e2p_fd(e2p), participant_id(pid),
              isolate_audio(isolate), audience_audio(audience),
              share_audio(share) {}
        // ...existing members...
        // Receives ONLY onShareAudioRawDataReceived buffers. Mutually
        // exclusive with the other routes by construction: main.cpp's
        // screenshare branch always passes isolate/audience false.
        bool share_audio = false;
```

In `engine/src/engine-audio.cpp`, thread the flag through `init` (definition line 14: add the `bool share_audio` parameter, pass `share_audio` as the new constructor argument at line 42, and add `it->second->share_audio = share_audio;` beside the other in-place updates at lines 45-48). Then make the mixed route skip share targets (line 383):

```cpp
        // Skip isolate, audience AND share targets — none receive the mix.
        if (entry.second->isolate_audio || entry.second->audience_audio ||
            entry.second->share_audio)
            continue;
```

(The two `onOneWayAudioRawDataReceived` passes need no change: both already require `isolate_audio` or `audience_audio` true, which a share target never is.)

Replace the stub at `engine/src/engine-audio.cpp:418`:

```cpp
void EngineAudio::onShareAudioRawDataReceived(AudioRawData *data, uint32_t user_id)
{
    // Fires only while someone is sharing WITH computer sound — silence here
    // usually means the sharer didn't tick "Share sound", not a fault.
    if (!data || m_e2p_fd == kIpcInvalidFd || data->GetBufferLen() == 0) return;
    tile_clock_log(user_id, data->GetTimeStamp(), tile_clock_now_ns(), "a");

    std::lock_guard<std::mutex> lock(m_targets_mtx);
    for (auto &entry : m_targets) {
        if (!entry.second || !entry.second->share_audio) continue;
        // Slot self-description (see ShmAudioSlot::participant_id): stamp the
        // SHARER so ISO attribution follows a share takeover mid-recording.
        entry.second->participant_id = user_id;
        output_audio_frame(*entry.second, entry.first, data,
                           "audio_share_frame_received");
    }
}
```

In `engine/src/main.cpp`, extend the screenshare branch (lines 1793-1794):

```cpp
                if (mode == "screenshare") {
                    share_engine.subscribe(uuid, e2p);
                    if (ipc_subscribe_wants_share_audio(line)) {
                        EngineAudio::instance().init(e2p, uuid,
                                                     /*participant_id=*/0,
                                                     /*isolate_audio=*/false,
                                                     /*audience_audio=*/false,
                                                     /*share_audio=*/true);
                    } else {
                        // Mirror the video_only rule above: a re-subscribe
                        // without the flag must stop the audio traffic a
                        // previous opted-in subscribe of this uuid started.
                        EngineAudio::instance().remove(uuid);
                    }
                } else {
```

In `src/zoom-engine-client.cpp`, make the plugin always request it (lines 868-873):

```cpp
void ZoomEngineClient::subscribe_screenshare(const std::string &source_uuid)
{
    if (!m_running.load(std::memory_order_acquire) || source_uuid.empty()) return;
    write_json(R"({"cmd":"subscribe","source_uuid":")" + json_escape(source_uuid) +
        R"(","mode":"screenshare","share_audio":true})");
}
```

- [ ] **Step 4: Run test to verify it passes**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: PASS — `engine-command: all tests passed`, full suite 63/63 (no new registrations in this task).

- [ ] **Step 5: Commit**

```sh
git add src/engine-command.h engine/src/engine-audio.h engine/src/engine-audio.cpp engine/src/main.cpp src/zoom-engine-client.cpp tests/engine-command-test.cpp
git commit -m "feat(share): deliver share computer audio to screenshare sources"
```

---

### Task 2: Stamp `capture_ns` on share frames

`ShmFrameHeader::capture_ns`'s own comment (`src/engine-ipc.h:117-122`) calls this out: `engine/src/engine-video.cpp:318` stamps it, `engine/src/engine-share.cpp` does not — the share writer leaves the field at the region's zero-fill, the plugin's reader treats 0 as "not measured", and every share output shows "-" for A/V Offset permanently. "That is a gap, not a design decision." One line closes it; the comment that documents the lie is corrected in the same commit (this repo's standing comment-lie policy — three of them shipped in the talkback milestone alone).

**Files:**
- Modify: `engine/src/engine-share.cpp` (`#include "tile-clock-log.h"` at the top, one stamp in `onRawDataFrameReceived` at lines 345-353)
- Modify: `src/engine-ipc.h:117-122` (delete the now-false "NOT WRITTEN BY EVERY PRODUCER" paragraph)

**Interfaces:**
- Consumes: `tile_clock_now_ns()` from `engine/src/tile-clock-log.h` (already used by `engine-video.cpp:228` and `engine-audio.cpp` with the same include).
- Produces: nothing new — the existing plugin reader (`shm_read_i420_frame`'s `out_capture_ns`) starts receiving real values with zero plugin changes.

- [ ] **Step 1: The failing check**

No unit test — the writer is SDK-bound (`YUVRawDataI420` arrives only from a live renderer, and linking `engine-share.cpp` into a test drags in `createRenderer`/`destroyRenderer` from the SDK import library). The check is live, and it is written down before the code: with a share running, the diagnostics dialog's A/V Offset column for the share output must show a number instead of "-". Same precedent as the talkback plan's SDK-bound tasks: the compile is the gate at this step, Step 4 is the verification.

- [ ] **Step 2: Confirm the current live failure**

With any meeting + share active on the installed build: diagnostics dialog → share output → A/V Offset reads "-". (This is already known-true from the `engine-ipc.h` comment; re-confirming costs one glance during the Task 1 live pass and anchors the before/after.)

- [ ] **Step 3: Write minimal implementation**

At the top of `engine/src/engine-share.cpp`, after `#include "engine-writer.h"`:

```cpp
#include "tile-clock-log.h"
```

In `onRawDataFrameReceived`, where the header fields are written (lines 351-353), add the stamp beside them:

```cpp
        hdr->width = w;
        hdr->height = h;
        hdr->y_len = static_cast<uint32_t>(y_len);
        // Same clock, same caveats as engine-video.cpp's stamp — see
        // ShmFrameHeader::capture_ns. Unstamped, the reader treats 0 as "not
        // measured" and a share output showed "-" for A/V Offset forever
        // (the engine-ipc.h comment documented this as a known gap).
        hdr->capture_ns = tile_clock_now_ns();
```

In `src/engine-ipc.h`, replace the paragraph at lines 117-122 ("NOT WRITTEN BY EVERY PRODUCER ... That is a gap, not a design decision.") with:

```cpp
    // Written by BOTH producers: engine/src/engine-video.cpp and (since
    // 2026-08-29) engine/src/engine-share.cpp stamp tile_clock_now_ns() here.
    // A reader still treats 0 as "not measured" — a fresh region's zero-fill
    // can be read before the first stamped frame lands.
```

- [ ] **Step 4: Build, run the suite, verify live**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: 63/63 green. Live (matched pair installed, share running): the share output's A/V Offset shows a real millisecond figure.

- [ ] **Step 5: Commit**

```sh
git add engine/src/engine-share.cpp src/engine-ipc.h
git commit -m "feat(share): stamp capture_ns on share frames so A/V offset is measured"
```

---

### Task 3: Share ISO stem

Share frames already flow through the exact machinery the stem needs: `record_video_frame()` is called for every engine frame a share source publishes (`src/zoom-source.cpp:1617`), sessions re-segment on any resolution change (`ensure_session_locked`), video is paced to `kIsoVideoTargetFps` before the ffmpeg pipe (`iso_video_frames_due()`), and — after Task 1 — share audio reaches `record_audio_frame()` (`src/zoom-source.cpp:2100`) with `iso_audio_silence_frames()` gap-fill. The only thing standing in the way is one predicate: `should_record()` returns false for `AssignmentMode::ScreenShare` (`src/zoom-iso-recorder.cpp:452`). Flipping it needs a second rule alongside: a share END sends no `on_output_removed` — the frames just cease — so an idle-close is the end-of-share signal, or one stem would silently span multiple share bursts with the between-time compressed out of the CFR video (exactly the "file shorter than the meeting" defect class the pacer exists for). Both decisions are pure, so they move into a header a test can pin.

**Files:**
- Create: `src/iso-record-policy.h`
- Modify: `src/zoom-iso-recorder.cpp:447-458` (`should_record`), `:428-432` (comment on the share-close branch), `:686-706` (`sweep_unresolved_locked` gains the share-idle close), plus `#include "iso-record-policy.h"` at the top
- Test: `tests/iso-record-policy-test.cpp`
- Modify: `CMakeLists.txt` (register after the `CoreVideoIsoVideoPacer` block, `CMakeLists.txt:950-957`)

**Interfaces:**
- Consumes: `AssignmentMode` from `src/zoom-types.h` (pure — includes only `<cstdint>/<functional>/<string>`).
- Produces: `inline bool iso_should_record(bool recorder_active, const std::string &source_uuid, AssignmentMode assignment, uint32_t resolved_participant_id)`; `inline bool iso_share_session_idle_expired(uint64_t last_video_ns, uint64_t now_ns)`; `constexpr uint64_t kIsoShareIdleCloseNs`.

- [ ] **Step 1: Write the failing test**

Create `tests/iso-record-policy-test.cpp`:

```cpp
// tests/iso-record-policy-test.cpp
// Which outputs get an ISO stem, and when a share stem ends. Pinned because
// the previous rule was an inline hard-exclusion of ScreenShare that nothing
// tested — and because a share end arrives as NOTHING (no roster edge, no
// output removal, the frames just cease), so the idle-close threshold IS the
// end-of-share signal and must not drift.
#include "iso-record-policy.h"

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
    const std::string uuid = "u1";

    // --- The recorder gate and the uuid gate hold for every mode ---
    check(!iso_should_record(false, uuid, AssignmentMode::ScreenShare, 7),
          "an inactive recorder must never open a stem");
    check(!iso_should_record(true, std::string(), AssignmentMode::ScreenShare, 7),
          "an empty source_uuid must never open a stem");

    // --- Share records regardless of participant resolution ---
    // Frames only flow while a share is active, so arrival is the liveness
    // signal; the sharer's id (carried on the frame event) may lag it.
    check(iso_should_record(true, uuid, AssignmentMode::ScreenShare, 0),
          "a share stem must open even before the sharer id resolves");
    check(iso_should_record(true, uuid, AssignmentMode::ScreenShare, 42),
          "a share stem must open with a resolved sharer id");

    // --- The pre-existing rules for the other modes are unchanged ---
    check(iso_should_record(true, uuid, AssignmentMode::Participant, 42),
          "a resolved participant must record");
    check(!iso_should_record(true, uuid, AssignmentMode::Participant, 0),
          "an unresolved participant must not record");
    check(iso_should_record(true, uuid, AssignmentMode::ActiveSpeaker, 0),
          "active speaker records even unresolved");
    check(iso_should_record(true, uuid, AssignmentMode::SpotlightIndex, 0),
          "spotlight records even unresolved");

    // --- Share-idle close: strict threshold, no firing before first frame ---
    check(!iso_share_session_idle_expired(0, 99'000'000'000ULL),
          "a session with no video yet must not idle-close");
    check(!iso_share_session_idle_expired(10'000'000'000ULL, 9'000'000'000ULL),
          "a clock step backwards must not idle-close");
    check(!iso_share_session_idle_expired(10'000'000'000ULL,
                                          10'000'000'000ULL + kIsoShareIdleCloseNs),
          "exactly the threshold is not yet expired (strict greater-than)");
    check(iso_share_session_idle_expired(10'000'000'000ULL,
                                         10'000'000'000ULL + kIsoShareIdleCloseNs + 1),
          "one ns past the threshold must idle-close");

    if (failures == 0)
        std::cout << "iso-record-policy: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Add to `CMakeLists.txt` immediately after the `add_test(NAME CoreVideoIsoVideoPacer ...)` block (line ~957):

```cmake
    # Which outputs get an ISO stem. ScreenShare was hard-excluded inline and
    # untested; a share end also arrives as silence (no output removal), so
    # the idle-close threshold is the end-of-share signal. See
    # src/iso-record-policy.h.
    add_executable(CoreVideoIsoRecordPolicyTest
        tests/iso-record-policy-test.cpp
    )
    target_include_directories(CoreVideoIsoRecordPolicyTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoIsoRecordPolicy
             COMMAND CoreVideoIsoRecordPolicyTest)
```

Then:

```sh
cmake -S . -B build && cmake --build build --config Release --target CoreVideoIsoRecordPolicyTest --parallel 8
```

Expected: FAIL — `Cannot open include file: 'iso-record-policy.h'`.

- [ ] **Step 3: Write minimal implementation**

Create `src/iso-record-policy.h`:

```cpp
#pragma once
//
// iso-record-policy.h — which outputs get an ISO stem, and when a share
// stem ends.
//
// ScreenShare was excluded from ISO by a hard `return false` inside
// ZoomIsoRecorder::should_record() with no test pinning it, dating from
// before the share path could feed the recorder at all. Everything the stem
// needs already exists downstream: record_video_frame() paces share frames
// to the same fixed cadence as every other stem (src/iso-video-pacer.h),
// ensure_session_locked() re-segments on any resolution change (shares
// resize far more often than cameras), and share audio gap-fill rides
// src/iso-audio-gap-fill.h.
//
// The share-specific wrinkle is the ENDING. A participant stem ends via
// on_output_removed() or the unresolved grace sweep; a share ending emits
// neither — Zoom just stops delivering frames. Without an idle close, one
// stem spans every share burst of the meeting with the idle time compressed
// out of the CFR video (the same "file shorter than real time" defect class
// iso-video-pacer.h documents, measured live 2026-08-19 as files finishing
// in ~55-60% of the meeting's duration). Idle time on the video path is
// therefore the end-of-share signal.
//
// Free of Qt / OBS / SDK dependencies so a test can pin both rules.
//
#include "zoom-types.h"

#include <cstdint>
#include <string>

// 5 s with no share frame = the share is over; close and finalize the stem.
// Comfortably above any real mid-share stall this codebase has measured
// (share delivery pauses on static content are sub-second keepalives), and
// short enough that back-to-back shares land in separate, correctly-timed
// files.
constexpr uint64_t kIsoShareIdleCloseNs = 5'000'000'000ULL;

inline bool iso_should_record(bool recorder_active,
                              const std::string &source_uuid,
                              AssignmentMode assignment,
                              uint32_t resolved_participant_id)
{
    if (!recorder_active || source_uuid.empty()) return false;
    // Share frames only arrive while a share is live, so arrival itself is
    // the liveness gate; the sharer id on the frame event may resolve late
    // and must not hold the stem hostage.
    if (assignment == AssignmentMode::ScreenShare) return true;
    if (resolved_participant_id == 0 &&
        assignment != AssignmentMode::ActiveSpeaker &&
        assignment != AssignmentMode::SpotlightIndex)
        return false;
    return true;
}

// Strict greater-than, and never fires before the first frame (0) or across
// a backwards clock step — same defensive shape as ipc_heartbeat_expired().
inline bool iso_share_session_idle_expired(uint64_t last_video_ns,
                                           uint64_t now_ns)
{
    return last_video_ns != 0 && now_ns > last_video_ns &&
           (now_ns - last_video_ns) > kIsoShareIdleCloseNs;
}
```

In `src/zoom-iso-recorder.cpp`: add `#include "iso-record-policy.h"` beside the other iso includes (line 2-4); replace the body of `should_record` (lines 447-458):

```cpp
bool ZoomIsoRecorder::should_record(const ZoomOutputInfo &info,
                                    uint32_t resolved_participant_id) const
{
    return iso_should_record(m_active.load(std::memory_order_acquire),
                             info.source_uuid, info.assignment,
                             resolved_participant_id);
}
```

Update the comment on the ScreenShare branch of `on_output_updated` (line 428-431) — the branch itself stays, its meaning narrows:

```cpp
    if (info.assignment == AssignmentMode::ScreenShare) {
        // Reachable only when should_record() said no — i.e. the recorder
        // stopped (share is otherwise always recordable). Final, no grace.
        close_session_locked(info.source_uuid);
        return;
    }
```

Extend `sweep_unresolved_locked` (lines 686-706) — the share-idle rule joins the existing loop, checked first:

```cpp
    for (auto &entry : m_sessions) {
        Session &session = entry.second;
        // A share end arrives as NOTHING (no output removal, frames just
        // cease), so idle time on the video path is the end-of-share signal
        // — see iso-record-policy.h.
        if (session.assignment == AssignmentMode::ScreenShare &&
            iso_share_session_idle_expired(session.last_video_ns, now_ns)) {
            to_close.push_back(entry.first);
            continue;
        }
        if (session.unresolved_since_ns == 0 ||
            now_ns < session.unresolved_since_ns)
            continue;
        if (now_ns - session.unresolved_since_ns > kUnresolvedGraceNs)
            to_close.push_back(entry.first);
    }
```

(The close loop below it is shared; adjust its log line to say "unresolved or share-idle" rather than duplicating the loop.)

- [ ] **Step 4: Run test to verify it passes**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: 64/64 green including `CoreVideoIsoRecordPolicy`. Live check during the next verification meeting: start ISO recording, share a screen for ~30 s, stop sharing, wait 10 s — a `*_screen_share_*` MP4/WAV pair appears in the ISO folder, plays at real-time speed, and a second share produces a second pair.

- [ ] **Step 5: Commit**

```sh
git add src/iso-record-policy.h src/zoom-iso-recorder.cpp tests/iso-record-policy-test.cpp CMakeLists.txt
git commit -m "feat(share): record a share ISO stem, closed by share-idle"
```

---

### Task 4: `screen_share` event on the control API

The control server already pushes edges to `subscribe_events` subscribers from a 250 ms poll (`poll_and_push()`, `src/zoom-control-server.cpp:129-169`: `meeting_state`, `active_speaker`, `speaker_director_changed`) plus roster-driven `roster_changed`. Share state is only *pollable* today (`list_outputs` / `list_participants`); a Companion button or external controller wanting "share just started" has to diff rosters itself. The sharer scan already exists in three copies (`output_to_json` line 196-200, `src/zoom-output-health.h:34-38`, `src/zoom-dock.cpp:378-385`) — the fourth caller is the moment it moves into a shared pure header so the edge detector can be pinned by a test.

**Files:**
- Create: `src/share-activity.h`
- Modify: `src/zoom-control-server.h:40` (add `uint32_t m_last_share_user = 0;` after `m_last_speaker`), `src/zoom-control-server.cpp` (`poll_and_push` at 129-169; initial snapshot in the `subscribe_events` handler at 1088-1105; `#include "share-activity.h"`)
- Test: `tests/share-activity-test.cpp`
- Modify: `CMakeLists.txt` (register after the `CoreVideoOutputHealth` block, `CMakeLists.txt:549-556`)

**Interfaces:**
- Consumes: `ParticipantInfo` / `std::vector<ParticipantInfo>` from `src/zoom-types.h` (pure); `ZoomControlServer::push_event(const QJsonObject&)` (existing).
- Produces: `inline uint32_t share_active_user(const std::vector<ParticipantInfo> &roster)`; wire event `{"event":"screen_share","active":bool,"user_id":N,"name":"..."}`. Task 5 consumes the wire event.

- [ ] **Step 1: Write the failing test**

Create `tests/share-activity-test.cpp`:

```cpp
// tests/share-activity-test.cpp
// The sharer scan behind the control API's screen_share push event. Pinned
// because the engine guarantees AT MOST ONE is_sharing_screen participant
// (main.cpp's set_active_share_user writes the flag exclusively), and the
// event edge (A->0, 0->A, and the single-edge A->B takeover) depends on
// this function returning that one id deterministically.
#include "share-activity.h"

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

static ParticipantInfo person(uint32_t id, bool sharing)
{
    ParticipantInfo p;
    p.user_id = id;
    p.display_name = "P" + std::to_string(id);
    p.is_sharing_screen = sharing;
    return p;
}

int main()
{
    // --- Nobody sharing -> 0, including the empty roster ---
    check(share_active_user({}) == 0, "empty roster did not report no sharer");
    check(share_active_user({person(1, false), person(2, false)}) == 0,
          "no-sharer roster did not report 0");

    // --- The one sharer is found regardless of position ---
    check(share_active_user({person(1, false), person(2, true)}) == 2,
          "the sharing participant was not found");
    check(share_active_user({person(3, true), person(1, false)}) == 3,
          "a sharer at the head of the roster was not found");

    // --- Defensive: two flagged (engine invariant broken) -> first wins,
    //     deterministically, rather than flapping between the two ---
    check(share_active_user({person(4, true), person(5, true)}) == 4,
          "two flagged sharers did not resolve first-wins");

    // --- A takeover (A -> B) is a VALUE change, so the caller's
    //     last!=now edge detector emits exactly one event for it ---
    const uint32_t before = share_active_user({person(6, true), person(7, false)});
    const uint32_t after  = share_active_user({person(6, false), person(7, true)});
    check(before == 6 && after == 7 && before != after,
          "a share takeover did not present as a single value change");

    if (failures == 0)
        std::cout << "share-activity: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Add to `CMakeLists.txt` immediately after the `add_test(NAME CoreVideoOutputHealth ...)` block (line ~556):

```cmake
    # The sharer scan behind the control API's screen_share push event and
    # its two pre-existing duplicate call sites. See src/share-activity.h.
    add_executable(CoreVideoShareActivityTest
        tests/share-activity-test.cpp
    )
    target_include_directories(CoreVideoShareActivityTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoShareActivity
             COMMAND CoreVideoShareActivityTest)
```

Then:

```sh
cmake -S . -B build && cmake --build build --config Release --target CoreVideoShareActivityTest --parallel 8
```

Expected: FAIL — `Cannot open include file: 'share-activity.h'`.

- [ ] **Step 3: Write minimal implementation**

Create `src/share-activity.h`:

```cpp
#pragma once
//
// share-activity.h — who owns the active screen share, per the roster.
//
// The engine maintains AT MOST ONE is_sharing_screen participant
// (engine/src/main.cpp's set_active_share_user assigns the flag
// exclusively: `p.is_sharing_screen = user_id != 0 && p.user_id == user_id`).
// This scan already existed in three inline copies (zoom-control-server.cpp's
// output_to_json, zoom-output-health.h, zoom-dock.cpp) before the screen_share
// push event needed a fourth; the pattern this repo keeps re-learning (the
// talkback milestone's members_present_locked, among others) is that the
// second hand-copied loop is the one that drifts silently.
//
// Qt/OBS-free so tests/share-activity-test.cpp can pin it.
//
#include "zoom-types.h"

#include <cstdint>
#include <vector>

// The sharing participant's user_id, or 0 when nobody shares. First-wins if
// the at-most-one invariant is ever broken upstream — deterministic beats
// flapping between two ids at the caller's 250 ms poll.
inline uint32_t share_active_user(const std::vector<ParticipantInfo> &roster)
{
    for (const ParticipantInfo &p : roster) {
        if (p.is_sharing_screen)
            return p.user_id;
    }
    return 0;
}
```

In `src/zoom-control-server.h`, after `uint32_t m_last_speaker = 0;` (line 40):

```cpp
    uint32_t            m_last_share_user = 0;
```

In `src/zoom-control-server.cpp`, `#include "share-activity.h"` with the local includes, then extend `poll_and_push()` after the speaker-director block (line 168):

```cpp
    // Share start/stop/takeover, as an edge. Roster-derived like everything
    // else here; OSC already pushes the same edge (/zoom/event/screen_share),
    // this brings the TCP subscribe_events stream to parity.
    const auto share_roster = ZoomEngineClient::instance().roster();
    const uint32_t share_user = share_active_user(share_roster);
    if (share_user != m_last_share_user) {
        m_last_share_user = share_user;
        QString share_name;
        const auto sharer = std::find_if(share_roster.begin(), share_roster.end(),
            [share_user](const ParticipantInfo &p) {
                return p.user_id == share_user;
            });
        if (sharer != share_roster.end())
            share_name = QString::fromStdString(sharer->display_name);
        push_event({
            {"event",   "screen_share"},
            {"active",  share_user != 0},
            {"user_id", static_cast<double>(share_user)},
            {"name",    share_name},
        });
    }
```

And in the `subscribe_events` handler (lines 1088-1105), after the existing `meeting_state` / `active_speaker` snapshot pushes, add the same snapshot so a subscriber that connects mid-share does not wait for the next edge:

```cpp
        const auto snap_roster = ZoomEngineClient::instance().roster();
        const uint32_t snap_share = share_active_user(snap_roster);
        QString snap_share_name;
        const auto snap_sharer = std::find_if(snap_roster.begin(), snap_roster.end(),
            [snap_share](const ParticipantInfo &p) {
                return p.user_id == snap_share;
            });
        if (snap_sharer != snap_roster.end())
            snap_share_name = QString::fromStdString(snap_sharer->display_name);
        push_event({{"event", "screen_share"},
                    {"active", snap_share != 0},
                    {"user_id", static_cast<double>(snap_share)},
                    {"name", snap_share_name}});
```

- [ ] **Step 4: Run test to verify it passes**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: 65/65 green including `CoreVideoShareActivity`. Wire check without a meeting is meaningless; during the next live pass: `printf '{"cmd":"subscribe_events"}\n' | nc 127.0.0.1 19870` held open shows one `{"event":"screen_share","active":true,...}` line when a share starts and `"active":false` when it stops.

- [ ] **Step 5: Commit**

```sh
git add src/share-activity.h src/zoom-control-server.h src/zoom-control-server.cpp tests/share-activity-test.cpp CMakeLists.txt
git commit -m "feat(share): push screen_share start/stop events on the control API"
```

---

### Task 5: Companion share state — variables and feedback

Companion today has exactly one share touchpoint: the `zoom_assign_screen_share` action (`companion/companion-module-corevideo-obs/src/actions.ts:110-119`). An operator cannot light a button when a share is live or show the sharer's name on it. Task 4's pushed event carries everything needed; the state transition goes into `state.ts` as a pure reducer (the module already has vitest coverage there, `state.test.ts`), because this repo's talkback rounds proved twice that logic inlined in event wiring is logic no test can reach. Companion refuses to overwrite an installed module version, so the version bumps in the same commit (CLAUDE.md's standing gotcha).

**Files:**
- Modify: `companion/companion-module-corevideo-obs/src/state.ts` (state fields + reducer), `src/index.ts:208-227` (`handlePluginEvent` gains the `screen_share` case), `src/variables.ts` (two variables), `src/feedbacks.ts` (one boolean feedback)
- Modify: `companion/companion-module-corevideo-obs/package.json` (version `1.0.2` → `1.0.3`, both the top-level field at line 3 and the legacy `companion.version` at line 24) and `companion/companion-module-corevideo-obs/companion/manifest.json` (its `version` field, same bump)
- Test: `companion/companion-module-corevideo-obs/src/state.test.ts`

**Interfaces:**
- Consumes: wire event `{"event":"screen_share","active":bool,"user_id":N,"name":"..."}` from Task 4.
- Produces: `export function applyScreenShareEvent(state: ModuleState, userId: number, name: string): boolean`; variables `zoom_screen_share` (`yes`/`no`) and `zoom_screen_share_name`; feedback `zoom_screen_share_active`.

- [ ] **Step 1: Write the failing test**

Append to `companion/companion-module-corevideo-obs/src/state.test.ts`:

```ts
describe('applyScreenShareEvent', () => {
	it('records a share start and reports the change', () => {
		const s = defaultState()
		expect(applyScreenShareEvent(s, 42, 'Alex Rivera')).toBe(true)
		expect(s.zoom.screenShareUserId).toBe(42)
		expect(s.zoom.screenShareName).toBe('Alex Rivera')
	})

	it('clears the name on share stop even if the event still carries one', () => {
		const s = defaultState()
		applyScreenShareEvent(s, 42, 'Alex Rivera')
		expect(applyScreenShareEvent(s, 0, 'Alex Rivera')).toBe(true)
		expect(s.zoom.screenShareUserId).toBe(0)
		expect(s.zoom.screenShareName).toBe('')
	})

	it('reports no change for a duplicate event, so feedbacks are not rechecked', () => {
		const s = defaultState()
		applyScreenShareEvent(s, 42, 'Alex Rivera')
		expect(applyScreenShareEvent(s, 42, 'Alex Rivera')).toBe(false)
	})

	it('treats a takeover (A to B) as one change', () => {
		const s = defaultState()
		applyScreenShareEvent(s, 42, 'Alex Rivera')
		expect(applyScreenShareEvent(s, 43, 'Sam Ortiz')).toBe(true)
		expect(s.zoom.screenShareUserId).toBe(43)
		expect(s.zoom.screenShareName).toBe('Sam Ortiz')
	})
})
```

with `applyScreenShareEvent` added to the existing import from `./state.js`.

- [ ] **Step 2: Run test to verify it fails**

```sh
cd companion/companion-module-corevideo-obs && npm test
```

Expected: FAIL — `applyScreenShareEvent` is not exported from `./state.js`.

- [ ] **Step 3: Write minimal implementation**

In `src/state.ts`, extend the `zoom` block of `ModuleState` (after `outputs: Output[]`):

```ts
		screenShareUserId: number
		screenShareName: string
```

seed both in `defaultState()` (`screenShareUserId: 0, screenShareName: ''`), and add:

```ts
// Reducer for the control API's screen_share push event. Pure and exported
// so state.test.ts can pin the stop-clears-name and duplicate-is-no-change
// rules — the module's event wiring is unreachable from tests.
export function applyScreenShareEvent(
	state: ModuleState,
	userId: number,
	name: string,
): boolean {
	const nextName = userId !== 0 ? name : ''
	const changed =
		state.zoom.screenShareUserId !== userId ||
		state.zoom.screenShareName !== nextName
	state.zoom.screenShareUserId = userId
	state.zoom.screenShareName = nextName
	return changed
}
```

In `src/index.ts`, add a case to `handlePluginEvent` (the switch at lines 209-225), importing `applyScreenShareEvent` from `./state.js`:

```ts
			case 'screen_share':
				if (applyScreenShareEvent(
						this.state,
						(msg['user_id'] as number) ?? 0,
						(msg['name'] as string) ?? '',
					))
					this.checkFeedbacks('zoom_screen_share_active')
				break
```

In `src/variables.ts`, add to `variableDefinitions`:

```ts
	zoom_screen_share:      { name: 'Zoom: Screen Share Active' },
	zoom_screen_share_name: { name: 'Zoom: Screen Share Participant Name' },
```

and to `buildVariableValues`:

```ts
		zoom_screen_share:      state.zoom.screenShareUserId !== 0 ? 'yes' : 'no',
		zoom_screen_share_name: state.zoom.screenShareName,
```

In `src/feedbacks.ts`, add beside `zoom_recovery_active`:

```ts
		zoom_screen_share_active: {
			type: 'boolean',
			name: 'Zoom: Screen Share Active',
			description: 'True while any participant is sharing their screen',
			defaultStyle: { bgcolor: GREEN, color: BLACK },
			options: [],
			callback: () => inst.state.zoom.screenShareUserId !== 0,
		},
```

Bump the version to `1.0.3` in `package.json` (both fields) and `companion/manifest.json` — Companion refuses to overwrite an installed version, so without the bump every install test would exercise the old bundle.

- [ ] **Step 4: Run test to verify it passes**

```sh
cd companion/companion-module-corevideo-obs && npm run build && npm test
```

Expected: tsc clean, vitest green including the four new `applyScreenShareEvent` cases. (The C++ suite is untouched by this task; no ctest run needed.)

- [ ] **Step 5: Commit**

```sh
git add companion/companion-module-corevideo-obs/src/state.ts companion/companion-module-corevideo-obs/src/state.test.ts companion/companion-module-corevideo-obs/src/index.ts companion/companion-module-corevideo-obs/src/variables.ts companion/companion-module-corevideo-obs/src/feedbacks.ts companion/companion-module-corevideo-obs/package.json companion/companion-module-corevideo-obs/companion/manifest.json
git commit -m "feat(share): Companion screen-share feedback and variables"
```

---

## Self-Review

**Gap coverage.** Requirement 4 (share audio) → Task 1. Requirement 5 (`capture_ns`) → Task 2. Requirement 6 (ISO stem) → Task 3, riding the existing pacer and gap-fill rather than duplicating either. Requirement 7 (events) → Tasks 4-5 for the two surfaces that lacked them; dock and OSC are stated as already done in the Spec and not re-planned. Requirements 1-3 are stated as fully implemented, with file/line evidence, and no task touches them beyond Task 2's one-line stamp inside the existing writer.

**SDK ground truth.** Every SDK name used was verified against `third_party/zoom-sdk/h`: `RAW_DATA_TYPE_SHARE` and `IZoomSDKRenderer::subscribe(uint32_t, ZoomSDKRawDataType)` (`rawdata_renderer_interface.h:22-57`), `IMeetingShareController` / `IMeetingShareCtrlEvent` / `ZoomSDKSharingSourceInfo` with fields `userid`/`shareSourceID`/`status`/`contentType` (`meeting_sharing_interface.h:43-80,149-207`), `YUVRawDataI420::GetStreamWidth/GetStreamHeight/GetSourceID` and `AudioRawData::GetBuffer/GetBufferLen/GetSampleRate/GetChannelNum/GetTimeStamp` (`rawdata_def.h`), `onShareAudioRawDataReceived(AudioRawData*, uint32_t)` (`rawdata_audio_helper_interface.h:18`). Notable layout fact: the tracked tree is FLAT — `meeting_sharing_interface.h`, `rawdata_renderer_interface.h`, `rawdata_audio_helper_interface.h` and `rawdata_def.h` sit directly under `h/`, with `h/meeting_service_components/` containing only two unrelated headers; the existing `__has_include` fallbacks in `engine-share.h`/`engine-audio.h` already absorb this and no task adds a bare `meeting_service_components/` or `rawdata/` include path. No interface this plan needs is missing from the tracked tree.

**Type consistency.** `ipc_subscribe_wants_share_audio(const std::string&)` is defined in Task 1 Step 3 and called in Task 1's `main.cpp` branch with that signature. `EngineAudio::init` gains `bool share_audio = false` as a defaulted sixth parameter — the untouched `SubscribeAudio` branch at `main.cpp:1772` keeps compiling, and the screenshare branch passes all six explicitly. `AudioTarget`'s constructor gains a fifth `bool share` matching the single `make_unique<AudioTarget>` call site, which Task 1 updates in the same edit. `iso_should_record(bool, const std::string&, AssignmentMode, uint32_t)` and `iso_share_session_idle_expired(uint64_t, uint64_t)` match between test, header, and both recorder call sites. `share_active_user(const std::vector<ParticipantInfo>&)` matches between test and both control-server call sites. `applyScreenShareEvent(state, number, string): boolean` matches between `state.test.ts` and `index.ts`.

**Placeholder scan:** none. The two test-less steps (Task 2, and Task 1's engine/plugin wiring beyond its helper test) are explicit statements about SDK-bound code with a named live verification, following the talkback Milestone 1 precedent, not deferred work.

**Deliberately out of scope, stated rather than hidden:** deleting the dead `zoom_share_source_register()` duplicate in `src/zoom-share-delegate.cpp` and the legacy in-process share delegate it belongs to (strangler-pattern cleanup with its own blast radius); any share *sending* (`StartMonitorShare` etc. — CoreVideo consumes shares, it does not produce them); multi-share (the engine deliberately follows the single active share, first-wins, per `active_share_source_id()`); and a 4K share path, which `ZoomSDKResolution`'s 1080P ceiling forecloses at the SDK boundary.

**One known soft spot, flagged:** Task 3's `kIsoShareIdleCloseNs = 5 s` is a judgment call, not a measured number — no soak has yet measured the longest frame gap Zoom produces mid-share on static content. If the first live pass shows a single share splitting into multiple files, raise the constant; the test pins the strict-threshold semantics, not the 5.
