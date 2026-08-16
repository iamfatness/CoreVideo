# Broadcast Audio Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make CoreVideo's participant audio lossless, sample-accurate, operator-alignable and measurably in sync with video.

**Architecture:** Four layers, built in dependency order. A pure master clock (`audio-timeline.h`) replaces wall-clock timestamps; a lock-free SPSC ring in shared memory replaces the single-slot mailbox that silently drops samples; a `capture_ns` field in both the audio slot and the video frame header makes the A/V offset a measured number; and an operator delay trim rides on the clock as pure arithmetic. Each task ships working software on its own.

**Tech Stack:** C++17, OBS Studio plugin API, Qt6, Zoom Meeting SDK 7.1.5, CMake + CTest, Windows shared memory (`CreateFileMappingA`)

**Spec:** `docs/superpowers/specs/2026-08-16-broadcast-audio-pipeline-design.md`

## Global Constraints

- **Base branch: `fix/live-defects-2026-08-16`** (in worktree `C:\Users\walla\CoreVideo\cv-fixes-0816-wt`, off `origin/main` @ `ef5de43`). The main checkout is parked on a stale docs branch — do not use it.
- **Broadcast sample rate is 48 kHz.** Zoom delivers 480 frames per 10 ms buffer.
- **If timing must err, err audio-late.** ITU-R BT.1359-1: audio leading is detectable at **+45 ms**, lagging only at **−125 ms**.
- **EBU R37 per-stage target: +5 / −15 ms.**
- **Delay audio to match video, never the reverse.** No video delay is implemented by this plan.
- **The ring writer must never block.** It runs on the Zoom SDK callback thread; blocking there risks the SDK dropping us entirely.
- **Do not re-anchor the timeline on a gap.** A mute is silence with a duration, not a discontinuity.
- Engine and plugin ship as a pair — wire-format changes need no backward compatibility, but a mismatch must fail loudly, not silently.
- Phase 1 covers `src/zoom-participant-audio-source.cpp` only. The other publish sites keep wall-clock stamps until phase 2.
- Tests are standalone `.cpp` under `tests/`, registered in `CMakeLists.txt` as executable `<Name>Test` with `add_test(NAME <Name> COMMAND <Name>Test)`.

## Build & test commands

Configure is already done in the worktree. To build and test:

```bash
cmake --build "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" --config Release --parallel
ctest --test-dir "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" -C Release --output-on-failure
```

If a fresh configure is ever needed, note CMake requires **forward slashes** in these paths or it dies parsing `\U`:

```bash
cmake -S <worktree> -B <worktree>/build -G "Visual Studio 17 2022" -A x64 \
  "-DCMAKE_PREFIX_PATH=<obs>/build_x64/libobs;<obs>/build_x64/frontend/api;<obs>/build_x64/deps/w32-pthreads;<obs>/.deps/obs-deps-2025-08-23-x64;<obs>/.deps/obs-deps-qt6-2025-08-23-x64" \
  "-DCMAKE_MODULE_PATH=<obs>/cmake/finders" \
  "-DZOOM_SDK_DIR=C:/Users/walla/Downloads/zoom-sdk-windows-7.1.5.43953/zoom-sdk-windows-7.1.5.43953/x64" \
  "-DFFMPEG_ROOT=C:/ffmpeg" "-DENABLE_FFMPEG_HW_ACCEL=ON" \
  "-DCOREVIDEO_RELEASE_VERSION=v0.1.40-dev-fixes0816"
```

where `<obs>` = `C:/Users/walla/Documents/Codex/2026-05-14/pull-the-latest-for-this-code/build-deps/obs-studio`.

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/audio-timeline.h` (new) | Pure master clock: cumulative samples → timestamp | 1 |
| `tests/audio-timeline-test.cpp` (new) | Clock arithmetic, drift, re-anchor policy | 1 |
| `src/zoom-participant-audio-source.cpp` | Adopts the clock; later the ring reader and trim | 2, 4, 6 |
| `src/engine-ipc.h` | Ring wire format; `capture_ns` on both headers | 3, 5 |
| `tests/audio-ring-test.cpp` (new) | Ring index arithmetic and overrun detection | 3 |
| `engine/src/engine-audio.cpp` | Ring writer, overrun error, `capture_ns` | 3, 5 |
| `engine/src/engine-video.cpp` | `capture_ns` on video frames | 5 |
| `src/zoom-settings.h` / `.cpp` | Global + per-source audio delay persistence | 6 |
| `src/zoom-output-manager.h` / `.cpp` | Per-output delay and measured latency fields | 5, 6 |
| `src/zoom-control-server.cpp` | Expose delay + measured offset | 7 |
| `src/zoom-output-dialog.cpp` | Delay column, measured offset display | 7 |

---

### Task 1: The master clock

**Files:**
- Create: `src/audio-timeline.h`
- Create: `tests/audio-timeline-test.cpp`
- Modify: `CMakeLists.txt` (register the test, beside the `CoreVideoDirectorHandover` block)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct AudioTimeline { uint64_t anchor_ns; uint64_t samples; uint32_t sample_rate; bool started; }`
  - `uint64_t audio_timeline_stamp(AudioTimeline &tl, uint32_t sample_rate, uint32_t frames, uint64_t arrival_ns)`
  - `void audio_timeline_reset(AudioTimeline &tl)`

  Tasks 2 and 6 call these exact signatures.

- [ ] **Step 1: Write the failing test**

Create `tests/audio-timeline-test.cpp`. Follow the house style of `tests/director-handover-test.cpp`: free `check(bool, const char*)`, a `failures` counter, narrative comments.

```cpp
// tests/audio-timeline-test.cpp
// The master clock every CoreVideo audio buffer is stamped from.
//
// The defect this exists for (2026-08-16, live show): every audio publish site
// stamped its buffer with os_gettime_ns() -- the wall-clock instant the plugin
// happened to read it. Zoom's buffers are exactly 10 ms apart, but they cross
// an IPC pipe and a shared-memory hop, so ARRIVAL is jittery. OBS was handed a
// stream whose timestamps advanced 8 ms, then 14 ms, then 3 ms, and reconciled
// it against its own audio clock by stretching, dropping and resampling --
// continuously. The operator heard it as "audio is very bad".
//
// The fix is to stop consulting arrival at all. Timestamps come from a
// cumulative sample count, so output advances by exactly one sample period per
// sample no matter when the buffer turned up.

#include "audio-timeline.h"

#include <iostream>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

static constexpr uint32_t kRate   = 48000;   // broadcast standard
static constexpr uint32_t kFrames = 480;     // Zoom's 10 ms buffer
static constexpr uint64_t k10ms   = 10'000'000ULL;

int main()
{
    // --- The first buffer anchors the timeline to its arrival ---
    {
        AudioTimeline tl{};
        const uint64_t ts = audio_timeline_stamp(tl, kRate, kFrames, 1'000'000'000ULL);
        check(ts == 1'000'000'000ULL,
              "the first buffer must publish at its own arrival time -- there is "
              "nothing else to anchor to");
    }

    // --- Jittery arrival must NOT reach the output ---
    {
        AudioTimeline tl{};
        const uint64_t base = 5'000'000'000ULL;
        audio_timeline_stamp(tl, kRate, kFrames, base);
        // Arrivals wander badly: +3 ms, then +21 ms, then +7 ms.
        const uint64_t t1 = audio_timeline_stamp(tl, kRate, kFrames, base + 3'000'000ULL);
        const uint64_t t2 = audio_timeline_stamp(tl, kRate, kFrames, base + 24'000'000ULL);
        const uint64_t t3 = audio_timeline_stamp(tl, kRate, kFrames, base + 31'000'000ULL);
        check(t1 == base + k10ms,
              "second buffer drifted with arrival instead of advancing one "
              "sample period -- this is the jitter reaching OBS");
        check(t2 == base + 2 * k10ms, "third buffer drifted with arrival");
        check(t3 == base + 3 * k10ms, "fourth buffer drifted with arrival");
    }

    // --- A mute is silence with a duration, not a new timeline ---
    {
        AudioTimeline tl{};
        const uint64_t base = 9'000'000'000ULL;
        audio_timeline_stamp(tl, kRate, kFrames, base);
        // Caller keeps the timeline moving through the mute by stamping the
        // silence it emits: 5 seconds is 500 buffers of 10 ms.
        for (int i = 0; i < 500; ++i)
            audio_timeline_stamp(tl, kRate, kFrames, base + 999'999'999ULL);
        const uint64_t after = audio_timeline_stamp(tl, kRate, kFrames, base);
        check(after == base + 501 * k10ms,
              "a 5-second mute did not produce 5 seconds of timeline -- "
              "re-anchoring on gaps is the drift EBU R37 exists to prevent");
    }

    // --- No drift over a long show. One million samples is ~20.8 seconds;
    // any per-buffer rounding error would have accumulated visibly by here ---
    {
        AudioTimeline tl{};
        const uint64_t base = 0;
        audio_timeline_stamp(tl, kRate, kFrames, base);
        const uint64_t buffers = 1'000'000ULL / kFrames;   // 2083
        uint64_t last = 0;
        for (uint64_t i = 0; i < buffers; ++i)
            last = audio_timeline_stamp(tl, kRate, kFrames, base);
        check(last == buffers * k10ms,
              "timestamps accumulated rounding error over 2083 buffers");
    }

    // --- An explicit reset starts a new timeline: subscribe, engine restart ---
    {
        AudioTimeline tl{};
        audio_timeline_stamp(tl, kRate, kFrames, 1'000'000'000ULL);
        audio_timeline_stamp(tl, kRate, kFrames, 1'000'000'000ULL);
        audio_timeline_reset(tl);
        const uint64_t ts = audio_timeline_stamp(tl, kRate, kFrames, 77'000'000'000ULL);
        check(ts == 77'000'000'000ULL,
              "an explicit reset did not re-anchor -- a re-subscribe or engine "
              "restart is a genuinely new timeline");
    }

    // --- A sample-rate change re-anchors on its own: the old sample count
    // means nothing at the new rate ---
    {
        AudioTimeline tl{};
        audio_timeline_stamp(tl, kRate, kFrames, 2'000'000'000ULL);
        const uint64_t ts = audio_timeline_stamp(tl, 16000, 160, 3'000'000'000ULL);
        check(ts == 3'000'000'000ULL,
              "a sample-rate change did not re-anchor -- the accumulated sample "
              "count is meaningless at the new rate");
    }

    // --- A zero rate or zero frame count must not divide by zero or advance ---
    {
        AudioTimeline tl{};
        audio_timeline_stamp(tl, kRate, kFrames, 4'000'000'000ULL);
        const uint64_t ts = audio_timeline_stamp(tl, 0, kFrames, 4'500'000'000ULL);
        check(ts == 4'500'000'000ULL,
              "a zero sample rate must fall back to arrival rather than divide "
              "by zero");
    }

    if (failures == 0)
        std::cout << "audio-timeline: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

Register in `CMakeLists.txt` directly after the `add_test(NAME CoreVideoDirectorHandover ...)` block:

```cmake
    # The master clock every audio buffer is stamped from. Wall-clock arrival
    # stamps let IPC jitter reach OBS, which resampled continuously — the
    # 2026-08-16 "audio is very bad" report.
    add_executable(CoreVideoAudioTimelineTest
        tests/audio-timeline-test.cpp
    )
    target_include_directories(CoreVideoAudioTimelineTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoAudioTimeline
             COMMAND CoreVideoAudioTimelineTest)
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" --config Release --target CoreVideoAudioTimelineTest
```

Expected: FAIL — `audio-timeline.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `src/audio-timeline.h`:

```cpp
#pragma once

// The master clock every CoreVideo audio buffer is stamped from.
//
// Extracted so it can be tested without OBS, Qt or a live engine -- the same
// treatment audio-subscription-state.h and director-handover.h get, and for the
// same reason: it is arithmetic whose only failure symptom is bad audio on air.
//
// THE DEFECT THIS EXISTS FOR (2026-08-16, live show). Every audio publish site
// stamped its buffer os_gettime_ns() -- the wall-clock instant the plugin
// happened to read it. Zoom's buffers are exactly 10 ms apart, but they cross
// an IPC pipe and a shared-memory hop, so ARRIVAL is jittery. OBS received a
// stream whose timestamps advanced 8 ms, then 14 ms, then 3 ms, and reconciled
// that against its own audio clock by stretching, dropping and resampling,
// continuously. Both vMix and Viz Engine run a master clock; CoreVideo had
// none.
//
// The rule: never consult arrival except to anchor. Output advances by exactly
// one sample period per sample.

#include <cstdint>

struct AudioTimeline {
    // Wall-clock instant the current timeline began.
    uint64_t anchor_ns   = 0;
    // Samples published since the anchor. Monotonic within a timeline.
    uint64_t samples     = 0;
    // The rate the accumulated sample count is denominated in.
    uint32_t sample_rate = 0;
    bool     started     = false;
};

// Begins a new timeline. Call ONLY where the old one is genuinely meaningless:
// a participant re-subscribe, a new engine process, a sample-rate change.
//
// Deliberately NOT called on a gap. A mute is silence with a duration, and the
// timeline has to honour that duration or audio walks out of sync across a long
// show -- the drift EBU R37 exists to prevent.
inline void audio_timeline_reset(AudioTimeline &tl)
{
    tl = AudioTimeline{};
}

// The timestamp this buffer publishes at, advancing the timeline by `frames`.
//
// `arrival_ns` is consulted only to anchor a new timeline; once running it is
// ignored entirely, which is the whole point.
inline uint64_t audio_timeline_stamp(AudioTimeline &tl,
                                     uint32_t sample_rate,
                                     uint32_t frames,
                                     uint64_t arrival_ns)
{
    // A rate change invalidates the accumulated sample count: N samples at
    // 16 kHz is not N samples at 48 kHz. Re-anchor rather than mis-scale.
    if (!tl.started || sample_rate == 0 || sample_rate != tl.sample_rate) {
        tl.anchor_ns   = arrival_ns;
        tl.samples     = 0;
        tl.sample_rate = sample_rate;
        tl.started     = true;
        // A zero rate cannot advance a timeline; publish at arrival and leave
        // the counter alone so the next valid buffer re-anchors cleanly.
        if (sample_rate == 0) {
            tl.started = false;
            return arrival_ns;
        }
    }

    // Split the division so neither term can overflow and no rounding error
    // accumulates: `rem` is always < sample_rate, so rem * 1e9 stays far inside
    // uint64 even at 48 kHz, and the seconds term is exact.
    const uint64_t whole = tl.samples / tl.sample_rate;
    const uint64_t rem   = tl.samples % tl.sample_rate;
    const uint64_t ts = tl.anchor_ns
                      + whole * 1'000'000'000ULL
                      + (rem * 1'000'000'000ULL) / tl.sample_rate;

    tl.samples += frames;
    return ts;
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
cmake --build "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" --config Release --target CoreVideoAudioTimelineTest
ctest --test-dir "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" -C Release -R CoreVideoAudioTimeline --output-on-failure
```

Expected: PASS — `audio-timeline: all tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/audio-timeline.h tests/audio-timeline-test.cpp CMakeLists.txt
git commit -m "feat(audio): master clock for sample-accurate timestamps

Every audio publish site stamped os_gettime_ns() -- the instant the plugin
happened to read the buffer. Zoom's buffers are exactly 10ms apart but cross
an IPC pipe and an SHM hop, so arrival is jittery and OBS was resampling
continuously to reconcile it. Both vMix and Viz Engine run a master clock.

Timestamps now come from a cumulative sample count. Re-anchors on subscribe,
engine restart and rate change -- never on a gap, because a mute is silence
with a duration the timeline has to honour."
```

---

### Task 2: Adopt the clock in the participant audio path

Ships real value on its own: fixes the jitter even before the ring lands.

**Files:**
- Modify: `src/zoom-participant-audio-source.cpp` — the `CoreVideoAudioSource` struct, `unsubscribe_audio()`, `forget_subscription_for_new_engine()`, and the publish at line 342
- Test: none possible — this needs OBS and a live engine. The arithmetic is covered by Task 1; see the note below.

**Interfaces:**
- Consumes: `AudioTimeline`, `audio_timeline_stamp()`, `audio_timeline_reset()` from Task 1.
- Produces: `CoreVideoAudioSource::timeline` — Task 6 adds the delay to the value it returns.

**Why there is no unit test here, stated plainly.** `CoreVideoAudioSource` needs OBS to instantiate and a live engine to feed. The rule it delegates to is fully tested in Task 1; the *wiring* is not, and deleting the `audio_timeline_reset()` call from `unsubscribe_audio()` would fail no test in this repo. Task 8's live verification is what stands behind it.

- [ ] **Step 1: Add the timeline to the source struct**

Find `struct CoreVideoAudioSource` in `src/zoom-participant-audio-source.cpp` and add, beside the existing buffers:

```cpp
    // The master clock this source's audio is stamped from. Guarded by the
    // same path that guards audio_buf: only the engine reader thread advances
    // it. See src/audio-timeline.h.
    AudioTimeline timeline;
```

Add `#include "audio-timeline.h"` beside the existing `#include "audio-subscription-state.h"`.

- [ ] **Step 2: Stamp from the timeline instead of the wall clock**

At `src/zoom-participant-audio-source.cpp:342`, replace:

```cpp
    audio.timestamp = os_gettime_ns();
```

with:

```cpp
    // Sample-derived, not arrival-derived: IPC jitter must not reach OBS.
    // `frames` is what this buffer actually carries, so the timeline advances
    // by exactly the audio published. See src/audio-timeline.h.
    const uint32_t timeline_frames =
        byte_len / (kZoomBytesPerSample * std::max<uint16_t>(channels, 1));
    audio.timestamp = audio_timeline_stamp(ctx->timeline, sample_rate,
                                           timeline_frames, os_gettime_ns());
```

Place this **after** `sample_rate`, `channels` and `byte_len` are known — read the surrounding code and confirm all three are in scope at that point; they are set in the seqlock copy loop above.

- [ ] **Step 3: Reset the timeline where the timeline genuinely ends**

In `unsubscribe_audio()` (`src/zoom-participant-audio-source.cpp:132`), after `ctx->current_participant_id.store(0, ...)`:

```cpp
    // The next subscribe is a new timeline: a different participant, or the
    // same one after a gap of unknown length. Neither can be stamped from the
    // old sample count.
    audio_timeline_reset(ctx->timeline);
```

In `forget_subscription_for_new_engine()` (the callback that drops mappings when a new engine process appears — find it near `audio_state_for_new_engine_process`), add the same call. A new engine restarts its own generation counters; our accumulated samples describe a process that no longer exists.

- [ ] **Step 4: Build and run the full suite**

```bash
cmake --build "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" --config Release --parallel
ctest --test-dir "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" -C Release --output-on-failure
```

Expected: builds clean, 45/45 pass (44 existing + `CoreVideoAudioTimeline`).

- [ ] **Step 5: Commit**

```bash
git add src/zoom-participant-audio-source.cpp
git commit -m "fix(audio): stamp participant audio from the master clock

Replaces the wall-clock arrival stamp on the path the operator actually
listens to. Resets the timeline on unsubscribe and on a new engine process --
both are genuinely new timelines -- but never on a gap."
```

---

### Task 3: The ring buffer wire format and engine writer

**Files:**
- Modify: `src/engine-ipc.h:87-93` (`ShmAudioHeader`)
- Create: `tests/audio-ring-test.cpp`
- Modify: `CMakeLists.txt` (register the test)
- Modify: `engine/src/engine-audio.cpp` — `ensure_shm()` and the write path at lines 224-236

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `kAudioRingSlots = 8`
  - `struct ShmAudioSlot { uint32_t sequence; uint64_t capture_ns; uint32_t byte_len; uint32_t reserved; }`
  - `struct ShmAudioHeader { uint32_t write_index; uint32_t slot_count; uint32_t slot_bytes; uint32_t sample_rate; uint16_t channels; uint16_t reserved; }`
  - `size_t shm_audio_region_bytes(uint32_t slot_bytes)`
  - `size_t shm_audio_slot_offset(const ShmAudioHeader &h, uint32_t index)`
  - `uint32_t audio_ring_slots_behind(uint32_t write_index, uint32_t read_index, uint32_t slot_count)`

  Task 4 consumes all of these.

- [ ] **Step 1: Write the failing test**

Create `tests/audio-ring-test.cpp`:

```cpp
// tests/audio-ring-test.cpp
// The index arithmetic of the audio shared-memory ring.
//
// The defect this exists for (2026-08-16, live show): the audio region was a
// SINGLE slot. engine-audio.cpp memcpy'd every Zoom buffer over the previous
// one, guarded only by a seqlock. A seqlock stops the reader seeing a TORN
// buffer; it does nothing about LOSS. Zoom delivers ~100 buffers/second, so any
// reader stall destroyed 10 ms of audio permanently and silently -- on a box
// already at 70% CPU with 10 sources, which is to say routinely.
//
// A ring lets the writer run ahead without destroying unread audio, and makes
// the loss that does happen countable instead of invisible.

#include "engine-ipc.h"

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
    // --- A reader level with the writer has nothing to do ---
    check(audio_ring_slots_behind(5, 5, kAudioRingSlots) == 0,
          "a caught-up reader was told it had slots pending");

    // --- Ordinary case: three buffers written since the last drain ---
    check(audio_ring_slots_behind(5, 2, kAudioRingSlots) == 3,
          "a reader three slots behind was miscounted");

    // --- The indices wrap. A reader near the top of the ring and a writer that
    // has wrapped past zero is the normal steady state, not an error ---
    check(audio_ring_slots_behind(1, kAudioRingSlots - 1, kAudioRingSlots) == 2,
          "wrapped indices were miscounted -- this is the steady state, not an "
          "edge case");

    // --- Exactly full: the writer has lapped the reader by the whole ring.
    // Every slot is unread and none is lost YET ---
    check(audio_ring_slots_behind(0, 0, kAudioRingSlots) == 0,
          "an equal pair must read as caught up, not as a full lap");

    // --- Region sizing must account for header + every slot ---
    {
        const uint32_t slot_bytes = 1920;   // 480 frames, 16-bit stereo
        const size_t total = shm_audio_region_bytes(slot_bytes);
        const size_t expected = sizeof(ShmAudioHeader) +
            static_cast<size_t>(kAudioRingSlots) *
            (sizeof(ShmAudioSlot) + slot_bytes);
        check(total == expected,
              "region sizing does not cover header plus every slot -- a short "
              "region means the last slot writes out of bounds");
    }

    // --- Slot offsets must be distinct, ordered, and inside the region ---
    {
        ShmAudioHeader h{};
        h.slot_count = kAudioRingSlots;
        h.slot_bytes = 1920;
        const size_t total = shm_audio_region_bytes(h.slot_bytes);
        size_t previous = 0;
        for (uint32_t i = 0; i < kAudioRingSlots; ++i) {
            const size_t off = shm_audio_slot_offset(h, i);
            check(off >= sizeof(ShmAudioHeader),
                  "a slot offset landed inside the header");
            check(off + sizeof(ShmAudioSlot) + h.slot_bytes <= total,
                  "a slot ran past the end of the region");
            if (i > 0)
                check(off > previous, "slot offsets are not strictly ordered");
            previous = off;
        }
    }

    if (failures == 0)
        std::cout << "audio-ring: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

Register in `CMakeLists.txt` after the `CoreVideoAudioTimeline` block:

```cmake
    # Index arithmetic of the audio SHM ring. The single-slot mailbox it
    # replaces dropped 10ms of audio on every reader stall, silently.
    add_executable(CoreVideoAudioRingTest
        tests/audio-ring-test.cpp
    )
    target_include_directories(CoreVideoAudioRingTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoAudioRing
             COMMAND CoreVideoAudioRingTest)
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" --config Release --target CoreVideoAudioRingTest
```

Expected: FAIL — `'kAudioRingSlots': undeclared identifier`.

- [ ] **Step 3: Replace the wire format**

In `src/engine-ipc.h`, replace the whole `ShmAudioHeader` definition (lines 87-93):

```cpp
// Audio is a RING, not a mailbox.
//
// It used to be one slot: the engine memcpy'd every Zoom buffer over the
// previous one, guarded by a seqlock. A seqlock prevents the reader seeing a
// TORN buffer and does nothing about LOSS -- so any reader stall destroyed
// 10 ms of audio permanently, silently, on a box where stalls are routine.
// Video keeps the mailbox on purpose (newest frame wins, a dropped frame is
// nearly invisible); the ear is not so forgiving, and Viz Engine ring-buffers
// audio for the same reason.
//
// 8 slots is 80 ms at Zoom's 10 ms buffer. That is CAPACITY, not latency: a
// reader keeping up sees about one slot of delay, and the depth is only spent
// while absorbing a stall.
static constexpr uint32_t kAudioRingSlots = 8;

struct ShmAudioSlot {
    // Even and unchanged across a read = the payload was stable. Odd = a write
    // is in progress. Same seqlock discipline the single slot had, now per-slot.
    uint32_t sequence;
    // The engine's os_gettime_ns() when the Zoom SDK handed this buffer over.
    // Both processes are on one machine and os_gettime_ns() is QPC-based, so
    // the plugin can subtract it directly to measure pipeline latency.
    uint64_t capture_ns;
    uint32_t byte_len;
    uint32_t reserved;
};

struct ShmAudioHeader {
    // Next slot the writer will fill. The reader drains up to (not including)
    // this. Written last, after the slot is complete.
    uint32_t write_index;
    uint32_t slot_count;
    uint32_t slot_bytes;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t reserved;
};

inline size_t shm_audio_region_bytes(uint32_t slot_bytes)
{
    return sizeof(ShmAudioHeader) +
           static_cast<size_t>(kAudioRingSlots) *
               (sizeof(ShmAudioSlot) + slot_bytes);
}

inline size_t shm_audio_slot_offset(const ShmAudioHeader &h, uint32_t index)
{
    return sizeof(ShmAudioHeader) +
           static_cast<size_t>(index) * (sizeof(ShmAudioSlot) + h.slot_bytes);
}

// How many slots the reader has yet to drain. Indices wrap, and a wrapped pair
// is the steady state rather than an error.
inline uint32_t audio_ring_slots_behind(uint32_t write_index,
                                        uint32_t read_index,
                                        uint32_t slot_count)
{
    if (slot_count == 0) return 0;
    return (write_index + slot_count - read_index) % slot_count;
}
```

- [ ] **Step 4: Run the ring test to verify it passes**

```bash
cmake --build "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" --config Release --target CoreVideoAudioRingTest
ctest --test-dir "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" -C Release -R CoreVideoAudioRing --output-on-failure
```

Expected: PASS — `audio-ring: all tests passed`.

- [ ] **Step 5: Size the region for the ring**

In `engine/src/engine-audio.cpp`, in `ensure_shm()`, replace:

```cpp
    const size_t total = sizeof(ShmAudioHeader) + byte_len;
```

with:

```cpp
    const size_t total = shm_audio_region_bytes(byte_len);
```

Immediately after `shm_region_create(target.shm, region.name, total)` succeeds, initialise the header once — the reader relies on `slot_count` and `slot_bytes` being correct before any slot is published:

```cpp
    auto *ring = static_cast<ShmAudioHeader *>(target.shm.ptr);
    ring->write_index = 0;
    ring->slot_count  = kAudioRingSlots;
    ring->slot_bytes  = byte_len;
    ring->sample_rate = 0;
    ring->channels    = 0;
    ring->reserved    = 0;
```

- [ ] **Step 6: Write into the ring**

Replace the write block in `engine/src/engine-audio.cpp` (lines 224-236, from `auto *hdr = static_cast<ShmAudioHeader *>` through `hdr->sequence = seq + 1;`):

```cpp
    auto *ring = static_cast<ShmAudioHeader *>(target.shm.ptr);
    // A buffer larger than the slots we sized for cannot be published without
    // corrupting the neighbouring slot. ensure_shm() grows the region on the
    // next call; drop this one loudly rather than write out of bounds.
    if (byte_len > ring->slot_bytes) {
        EngineIpc::write(
            R"({"cmd":"error","msg":"audio_slot_too_small","source_uuid":")" +
            source_uuid + R"(","byte_len":)" + std::to_string(byte_len) +
            R"(,"slot_bytes":)" + std::to_string(ring->slot_bytes) + "}");
        return;
    }

    const uint32_t index = ring->write_index % ring->slot_count;
    auto *slot = reinterpret_cast<ShmAudioSlot *>(
        static_cast<char *>(target.shm.ptr) +
        shm_audio_slot_offset(*ring, index));

    uint32_t seq = slot->sequence + 1;
    if ((seq & 1u) == 0) ++seq;
    slot->sequence = seq;                       // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    slot->capture_ns = os_gettime_ns();
    slot->byte_len   = byte_len;
    slot->reserved   = 0;
    std::memcpy(reinterpret_cast<char *>(slot) + sizeof(ShmAudioSlot),
                data->GetBuffer(), byte_len);
    std::atomic_thread_fence(std::memory_order_release);
    slot->sequence = seq + 1;                   // even: readable

    ring->sample_rate = data->GetSampleRate();
    ring->channels    = static_cast<uint16_t>(data->GetChannelNum());
    std::atomic_thread_fence(std::memory_order_release);
    // Published last: the reader treats everything below write_index as
    // complete, so this store is what makes the slot visible.
    ring->write_index = (ring->write_index + 1) % ring->slot_count;
```

`os_gettime_ns()` needs `#include <util/platform.h>` — check the file's existing includes and add it only if absent.

**Do not add a blocking wait when the ring is full.** This runs on the Zoom SDK callback thread; blocking there risks the SDK dropping us. Overwriting oldest is the deliberate choice, and Task 4 is what makes the resulting loss countable.

- [ ] **Step 7: Build and run the full suite**

```bash
cmake --build "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" --config Release --parallel
ctest --test-dir "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" -C Release --output-on-failure
```

Expected: builds clean, 46/46 pass. **Audio will be broken at runtime until Task 4** — the plugin still reads the old layout. That is expected and is why these two tasks land together.

- [ ] **Step 8: Commit**

```bash
git add src/engine-ipc.h engine/src/engine-audio.cpp tests/audio-ring-test.cpp CMakeLists.txt
git commit -m "feat(engine): ring-buffer the audio SHM region

The audio region was a single slot: every Zoom buffer memcpy'd over the
previous one under a seqlock. A seqlock stops the reader seeing a torn buffer
and does nothing about loss, so any reader stall destroyed 10ms of audio
permanently and silently -- on a box at 70% CPU with 10 sources, routinely.

8 slots is 80ms of capacity at Zoom's 10ms buffer; a reader keeping up sees
about one slot of delay. Each slot carries capture_ns so the plugin can
measure pipeline latency. The writer never blocks: it runs on the Zoom SDK
callback thread.

Plugin-side reader lands in the next commit; audio is broken between the two."
```

---

### Task 4: The ring reader and overrun accounting

**Files:**
- Modify: `src/zoom-participant-audio-source.cpp` — the `CoreVideoAudioSource` struct and the seqlock read block around lines 300-340
- Test: none possible (needs OBS + live engine). Index arithmetic is covered by Task 3.

**Interfaces:**
- Consumes: `kAudioRingSlots`, `ShmAudioSlot`, `ShmAudioHeader`, `shm_audio_slot_offset()`, `audio_ring_slots_behind()` from Task 3; `audio_timeline_stamp()` from Task 1.
- Produces: `CoreVideoAudioSource::read_index`, `::overrun_slots` — Task 7 surfaces the counter.

- [ ] **Step 1: Add reader state to the source struct**

In `struct CoreVideoAudioSource`, beside `timeline`:

```cpp
    // Next ring slot this source will drain. Only the engine reader thread
    // touches it, the same thread that owns `timeline`.
    uint32_t read_index    = 0;
    bool     read_started  = false;
    // Slots the writer lapped before we drained them -- audio that was lost.
    // Counted so loss is visible; the old mailbox lost audio invisibly.
    uint64_t overrun_slots = 0;
```

- [ ] **Step 2: Drain the ring instead of reading one slot**

Replace the seqlock read block in `output_audio_frame()` — the loop that reads `hdr->sequence`, copies into `ctx->audio_buf`, and sets `copied` — with a drain that publishes every unread slot **in order**. The publish body (channel conversion, `obs_source_output_audio`, ISO recording, logging) moves inside the loop unchanged.

```cpp
    auto *ring = static_cast<const ShmAudioHeader *>(ctx->audio_shm.ptr);
    const uint32_t slot_count = ring->slot_count;
    if (slot_count == 0) return;

    // First event after a (re)subscribe: start level with the writer rather
    // than replaying whatever stale audio the region still holds.
    if (!ctx->read_started) {
        ctx->read_index   = ring->write_index;
        ctx->read_started = true;
        return;
    }

    const uint32_t write_index = ring->write_index;
    uint32_t pending = audio_ring_slots_behind(write_index, ctx->read_index,
                                               slot_count);
    // A full lap means the writer overwrote slots we never drained. Skip to the
    // oldest slot still intact and count what was lost -- the point of the ring
    // is that this is now visible, not that it can never happen.
    if (pending >= slot_count) {
        ctx->overrun_slots += pending - (slot_count - 1);
        ctx->read_index = (write_index + 1) % slot_count;
        pending = slot_count - 1;
        blog(LOG_WARNING,
             "[obs-zoom-plugin] CoreVideo audio ring overrun: source=%s uuid=%s lost_slots=%llu",
             obs_source_get_name(ctx->source), ctx->source_uuid.c_str(),
             static_cast<unsigned long long>(ctx->overrun_slots));
    }

    for (uint32_t n = 0; n < pending; ++n) {
        const auto *slot = reinterpret_cast<const ShmAudioSlot *>(
            static_cast<const char *>(ctx->audio_shm.ptr) +
            shm_audio_slot_offset(*ring, ctx->read_index));

        uint32_t byte_len = 0;
        uint64_t capture_ns = 0;
        bool copied = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const uint32_t seq1 = slot->sequence;
            std::atomic_thread_fence(std::memory_order_acquire);
            if ((seq1 & 1u) != 0) continue;      // write in progress
            byte_len   = slot->byte_len;
            capture_ns = slot->capture_ns;
            // The payload can never exceed the slot the engine sized for it;
            // a larger value means we are reading a region the writer has
            // since resized, so drop it rather than read out of bounds.
            if (byte_len == 0 || byte_len > ring->slot_bytes) break;
            if (ctx->audio_buf.size() < byte_len)
                ctx->audio_buf.resize(byte_len);
            std::memcpy(ctx->audio_buf.data(),
                        reinterpret_cast<const char *>(slot) +
                            sizeof(ShmAudioSlot),
                        byte_len);
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint32_t seq2 = slot->sequence;
            if (seq1 == seq2 && (seq2 & 1u) == 0) { copied = true; break; }
        }
        ctx->read_index = (ctx->read_index + 1) % slot_count;
        if (!copied) continue;

        const uint32_t sample_rate = ring->sample_rate;
        const uint16_t channels    = ring->channels;
        // ... existing publish body, unchanged, using byte_len / sample_rate /
        // channels and stamping via audio_timeline_stamp() as Task 2 added ...
    }
```

Read the existing function before writing this: `sample_rate` and `channels` came from the old per-buffer header and now come from the ring header, so their declarations move. Keep the publish body byte-for-byte otherwise — this task changes *transport*, not *rendering*.

- [ ] **Step 3: Reset reader state where the timeline resets**

Wherever Task 2 added `audio_timeline_reset(ctx->timeline)` — `unsubscribe_audio()` and the new-engine callback — also add:

```cpp
    ctx->read_started = false;
```

A new region starts at write_index 0 and our old read_index means nothing against it.

- [ ] **Step 4: Build and run the full suite**

```bash
cmake --build "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" --config Release --parallel
ctest --test-dir "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" -C Release --output-on-failure
```

Expected: builds clean, 46/46 pass.

- [ ] **Step 5: Commit**

```bash
git add src/zoom-participant-audio-source.cpp
git commit -m "feat(audio): drain the ring instead of reading one slot

Publishes every unread slot in order, so a late reader catches up rather than
losing audio. A full lap is counted and logged as an overrun -- the loss the
old mailbox took silently is now visible and attributable."
```

---

### Task 5: capture_ns on video, and the A/V offset

**Files:**
- Modify: `src/engine-ipc.h` (`ShmFrameHeader`)
- Modify: `engine/src/engine-video.cpp` (the frame write path)
- Modify: `src/zoom-source.cpp` (`output_video_from_shared_memory`)
- Modify: `src/zoom-output-manager.h` / `.cpp` (`ZoomOutputInfo` fields)
- Test: none — this is plumbing a measured value; correctness is the live number in Task 8.

**Interfaces:**
- Consumes: `ShmAudioSlot::capture_ns` from Task 3.
- Produces: `ZoomOutputInfo::audio_latency_us`, `::video_latency_us` — Task 7 surfaces both.

- [ ] **Step 1: Add capture_ns to the video header**

In `src/engine-ipc.h`, replace `ShmFrameHeader`:

```cpp
struct ShmFrameHeader {
    uint32_t sequence;
    uint32_t width;
    uint32_t height;
    uint32_t y_len;
    // The engine's os_gettime_ns() when the Zoom SDK handed this frame over.
    // Paired with ShmAudioSlot::capture_ns this is what makes the A/V offset a
    // measured number instead of an assertion -- on 2026-08-16 the product
    // could not answer "what is our render latency" because nothing carried a
    // timestamp across the boundary.
    uint64_t capture_ns;
};
```

- [ ] **Step 2: Stamp it in the engine**

In `engine/src/engine-video.cpp`, find the frame write where `hdr->width`, `hdr->height` and `hdr->y_len` are set, and add alongside them:

```cpp
    hdr->capture_ns = os_gettime_ns();
```

It must be inside the same odd/even sequence window as the other header fields.

- [ ] **Step 3: Measure on the plugin side**

In `src/zoom-source.cpp`, in `output_video_from_shared_memory()`, after the seqlock copy succeeds and before publishing, record the latency:

```cpp
    // Both processes share a QPC-based monotonic clock, so this subtraction is
    // meaningful across the boundary.
    if (hdr_capture_ns != 0) {
        const uint64_t now = os_gettime_ns();
        if (now > hdr_capture_ns)
            m_video_latency_us.store((now - hdr_capture_ns) / 1000,
                                     std::memory_order_relaxed);
    }
```

Add `std::atomic<uint64_t> m_video_latency_us{0};` to `ZoomSource` in `src/zoom-source.h`. Name the local to match whatever the function already calls the copied header fields.

Do the same in `src/zoom-participant-audio-source.cpp`, using the `capture_ns` read from the slot in Task 4, into a new `std::atomic<uint64_t> audio_latency_us{0};` on `CoreVideoAudioSource`.

- [ ] **Step 4: Surface both on ZoomOutputInfo**

Add to `struct ZoomOutputInfo` in `src/zoom-output-manager.h`:

```cpp
    // Engine capture to OBS publish, microseconds. 0 = not yet measured.
    uint64_t audio_latency_us = 0;
    uint64_t video_latency_us = 0;
```

Populate them in `ZoomSource::output_info()` from the atomics.

- [ ] **Step 5: Build and run the full suite**

```bash
cmake --build "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" --config Release --parallel
ctest --test-dir "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" -C Release --output-on-failure
```

Expected: builds clean, 46/46 pass.

- [ ] **Step 6: Commit**

```bash
git add src/engine-ipc.h engine/src/engine-video.cpp src/zoom-source.cpp src/zoom-source.h src/zoom-participant-audio-source.cpp src/zoom-output-manager.h src/zoom-output-manager.cpp
git commit -m "feat: measure audio and video pipeline latency

Both SHM headers now carry the engine's capture timestamp, so the plugin can
subtract it at publish. video_latency - audio_latency is the A/V offset, the
number EBU R37 is written in terms of and the one CoreVideo could not state
at all before now."
```

---

### Task 6: The operator delay trim

**Files:**
- Modify: `src/zoom-settings.h` / `src/zoom-settings.cpp` (global default)
- Modify: `src/zoom-output-manager.h` (`ZoomOutputInfo::audio_delay_ms`)
- Modify: `src/zoom-participant-audio-source.cpp` (apply it)
- Test: none — arithmetic is one addition; the value is verified live in Task 8.

**Interfaces:**
- Consumes: `audio_timeline_stamp()` from Task 1.
- Produces: `ZoomPluginSettings::audio_delay_ms`, `ZoomOutputInfo::audio_delay_ms` — Task 7 exposes both.

- [ ] **Step 1: Persist the global default**

In `src/zoom-settings.h`, beside `hide_participants_without_video`:

```cpp
    // Milliseconds to delay CoreVideo audio, to align it with the slower video
    // path. vMix operators routinely run 20-100+ ms here. 0-500 ms.
    uint32_t            audio_delay_ms = 0;
```

Load it beside the `HideParticipantsWithoutVideo` line in `src/zoom-settings.cpp`:

```cpp
    s.audio_delay_ms = static_cast<uint32_t>(
        config_get_int(cfg, SECTION, "AudioDelayMs"));
    if (s.audio_delay_ms > 500) s.audio_delay_ms = 500;
```

Save it beside the matching `config_set_int`:

```cpp
    config_set_int   (cfg, SECTION, "AudioDelayMs", audio_delay_ms);
```

Add the per-output field to `struct ZoomOutputInfo` in `src/zoom-output-manager.h`, beside the latency fields Task 5 added — Task 7 reads it to populate the Output Manager spinbox and the control API:

```cpp
    // Per-source override of ZoomPluginSettings::audio_delay_ms. 0-500 ms.
    uint32_t audio_delay_ms = 0;
```

Populate it in `ZoomSource::output_info()` from the source's own atomic, the same way the other per-source settings are surfaced there.

- [ ] **Step 2: Apply it at the stamp**

In `src/zoom-participant-audio-source.cpp`, where Task 2 set `audio.timestamp`:

```cpp
    // Delay is arithmetic on the timeline, not a buffer: OBS's async path holds
    // timestamped audio until its time comes. Only ever pushes audio LATER --
    // ITU-R BT.1359-1 detects audio leading at +45 ms but tolerates lagging to
    // -125 ms, so late is the safe direction to err.
    audio.timestamp = audio_timeline_stamp(ctx->timeline, sample_rate,
                                           timeline_frames, os_gettime_ns()) +
                      static_cast<uint64_t>(ctx->audio_delay_ms.load(
                          std::memory_order_relaxed)) * 1'000'000ULL;
```

Add `std::atomic<uint32_t> audio_delay_ms{0};` to `CoreVideoAudioSource`, initialised from `ZoomPluginSettings::load().audio_delay_ms` where the source reads its other settings, and re-read on settings update.

- [ ] **Step 3: Build and run the full suite**

```bash
cmake --build "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" --config Release --parallel
ctest --test-dir "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" -C Release --output-on-failure
```

Expected: builds clean, 46/46 pass.

- [ ] **Step 4: Commit**

```bash
git add src/zoom-settings.h src/zoom-settings.cpp src/zoom-participant-audio-source.cpp src/zoom-output-manager.h
git commit -m "feat(audio): operator delay trim, 0-500ms

The control every vMix operator reaches for and CoreVideo did not have. Rides
on the master clock as pure arithmetic -- no buffer, no memory cost. Only ever
delays audio, never advances it, per BT.1359's asymmetry."
```

---

### Task 7: Expose it — control API, Output Manager, docs

**Files:**
- Modify: `src/zoom-control-server.cpp` (`list_outputs` fields; accept `audio_delay_ms` in `assign_output`)
- Modify: `src/zoom-output-dialog.cpp` (delay spinbox column, measured offset display)
- Modify: `README.md`, `CHANGELOG.md`

**Interfaces:**
- Consumes: `ZoomOutputInfo::audio_delay_ms`, `::audio_latency_us`, `::video_latency_us` from Tasks 5-6.
- Produces: nothing.

- [ ] **Step 1: Add the fields to list_outputs**

In `src/zoom-control-server.cpp`, in `output_to_json()` beside the existing `obj["subscribed_age_ms"]`:

```cpp
    obj["audio_delay_ms"]   = static_cast<double>(o.audio_delay_ms);
    obj["audio_latency_us"] = static_cast<double>(o.audio_latency_us);
    obj["video_latency_us"] = static_cast<double>(o.video_latency_us);
    // The EBU R37 number: positive means audio is EARLY relative to video.
    obj["av_offset_us"] =
        static_cast<double>(static_cast<int64_t>(o.video_latency_us) -
                            static_cast<int64_t>(o.audio_latency_us));
```

- [ ] **Step 2: Accept the delay over the API**

In the `assign_output` handler, beside the existing `audio_channels` read:

```cpp
    uint32_t audio_delay_ms = 0;
    if (req.contains("audio_delay_ms"))
        json_to_uint32(req, "audio_delay_ms", audio_delay_ms);
    if (audio_delay_ms > 500) audio_delay_ms = 500;
```

Thread it through `ZoomOutputManager::configure_output()` alongside `audio_mode`.

- [ ] **Step 3: Add the Output Manager control**

In `src/zoom-output-dialog.cpp`, add a `QSpinBox` column beside the existing audio channel combo (created near line 868), range 0-500, suffix `" ms"`, wired through `mark_dirty_on_user_change`. Add a read-only label showing `av_offset_us / 1000.0` with one decimal, so the operator can see what they are trimming toward.

Follow the existing column pattern exactly: add to the `OutputColumns` enum, set the header text where the other headers are set, and use `center_in_cell()` as the neighbouring widgets do.

- [ ] **Step 4: Build and check by hand**

```bash
cmake --build "C:/Users/walla/CoreVideo/cv-fixes-0816-wt/build" --config Release --parallel
```

Open the Output Manager. Verify the spinbox persists across a restart and that the measured offset shows a plausible non-zero number once media is active.

- [ ] **Step 5: Document it**

Add to `README.md` in the Output Manager section: what the delay does, that it only ever delays audio, and that the measured A/V offset is what to trim toward. State the EBU R37 target (+5 / −15 ms per stage) so the number has meaning.

Add to the `## [Unreleased]` section of `CHANGELOG.md`, matching the surrounding narrative style:

```markdown
- **Audio no longer drops samples.** The engine wrote every Zoom audio buffer
  into a single shared-memory slot, overwriting whatever had not been read yet
  — so on a loaded machine, audio was being lost continuously and silently.
  Audio now travels through an 8-slot ring, and the rare loss that does happen
  is counted and logged instead of vanishing.
- **Audio is stamped from a master clock.** Timestamps came from the moment the
  plugin happened to read a buffer, so IPC jitter reached OBS and it resampled
  continuously to compensate. Timestamps now derive from a running sample count,
  so they advance by exactly one sample period regardless of arrival.
- **Added an audio delay control (0–500 ms) and a measured A/V offset.** Video
  is the slower path in any software production chain, so audio needs delaying
  to match — the control every vMix operator expects. The Output Manager now
  also shows the measured offset, so you can trim against a number instead of
  by ear.
```

- [ ] **Step 6: Commit**

```bash
git add src/zoom-control-server.cpp src/zoom-output-dialog.cpp README.md CHANGELOG.md
git commit -m "feat: expose audio delay and measured A/V offset"
```

---

### Task 8: Live verification

**Files:** none. Verification only.

**Interfaces:**
- Consumes: everything above.
- Produces: the evidence that decides whether this shipped or not.

- [ ] **Step 1: Install both binaries**

Both are required — the wire format changed on each side, and a mismatched pair produces silence:

```powershell
Start-Process powershell -Verb RunAs -ArgumentList '-Command', 'Copy-Item "<builddir>\Release\obs-zoom-plugin.dll" "C:\Program Files\obs-studio\obs-plugins\64bit\obs-zoom-plugin.dll" -Force; Copy-Item "<builddir>\Release\ZoomObsEngine.exe" "C:\Program Files\obs-studio\obs-plugins\64bit\zoom-runtime\ZoomObsEngine.exe" -Force'
```

OBS must be fully closed first. Send `{"cmd":"leave"}` to `127.0.0.1:19870` before closing, or the engine is left in a bad state needing a force kill.

- [ ] **Step 2: Reproduce the original load**

Join a meeting with **10 sources at 1080p plus an 8-tile gallery** — the configuration that produced the "audio is very bad" report. Anything lighter does not test the stall behaviour the ring exists for.

- [ ] **Step 3: Assert zero overruns**

```powershell
Select-String -Path "$env:APPDATA\obs-studio\logs\<current>.txt" -Pattern "audio ring overrun" | Measure-Object
```

Expected: **zero**. A non-zero count means 8 slots is too shallow for this box — raise `kAudioRingSlots` and re-measure rather than shipping a number that does not hold.

- [ ] **Step 4: Read the measured A/V offset**

```powershell
$c=New-Object System.Net.Sockets.TcpClient; $c.Connect("127.0.0.1",19870)
$s=$c.GetStream(); $w=New-Object System.IO.StreamWriter($s); $w.NewLine="`n"; $w.AutoFlush=$true
$w.WriteLine('{"cmd":"list_outputs"}')
(New-Object System.IO.StreamReader($s)).ReadLine(); $c.Close()
```

Record `av_offset_us` per output. This is the first time the product has been able to state this number.

- [ ] **Step 5: Trim to EBU R37 and confirm**

Set `audio_delay_ms` to bring the offset inside **+5 / −15 ms**, biasing audio late. Re-read and confirm it holds over several minutes.

- [ ] **Step 6: The acceptance test that actually matters**

Have the operator listen, on a real show, for a sustained period. Every measurement above is necessary and none is sufficient — the report that opened this work was "audio is very bad", and only a person can close it.

- [ ] **Step 7: Record the result**

If it holds, note the measured offset and the delay that achieved it in `CHANGELOG.md`. If overruns appeared, return to Task 3 Step 3 with the real number rather than tuning blind.
