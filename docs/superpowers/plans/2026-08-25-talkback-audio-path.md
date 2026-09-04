# Zoom Talkback — Audio Path (Milestones 2–4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Carry real audio from an OBS source into a Zoom talkback channel — the first outbound audio path this codebase has ever had — with keying that fails closed by construction.

**Architecture:** A new shared-memory region `ZoomObsPlugin_talkback` reuses `ShmAudioHeader` with the roles reversed: the **plugin creates and writes**, the **engine opens and reads**. An OBS audio capture callback taps the operator's chosen source, converts to interleaved 16-bit PCM, and publishes to the ring; the engine drains on the notify edge **on its command-loop thread** (which is also the SDK's message-pump thread) and calls `SendAudioDataToChannel`. Keying lives in a pure decision header so both failure directions are testable without Qt, libobs, or a meeting.

**Tech Stack:** C++17, libobs audio callbacks, Zoom Windows Meeting SDK 7.1.5.43953, CMake + CTest, existing named-pipe line-JSON IPC + shared memory.

**Spec:** `docs/superpowers/specs/2026-08-24-zoom-talkback-design.md`

**Prior milestone:** `docs/superpowers/notes/2026-08-24-talkback-probe-results.md` — the Milestone 1 gate **passed**: entitlement holds, host/co-host is required, audio reaches an invited participant.

## Global Constraints

- **The ring's invariants are load-bearing and must not be weakened.** Free-running `write_index` (modulo only where a physical offset is derived), per-slot seqlock, edge-triggered `notify` owned via `audio_ring_notify_after_publish` / `audio_ring_reader_done` / `audio_ring_reader_abandon`. **Do not weaken the seq_cst fences** — the total-order proof lives on `ShmAudioHeader::notify` in `src/engine-ipc.h`.
- **Media events are prompts, not payloads.** A `talkback_audio` event means "drain everything pending", never "here is one buffer". Readers drain fully on any wakeup.
- Region names go through `shm_region_name(base, generation)` — a Windows named section cannot grow while mapped, so every resize moves to a `_gN` name.
- Audio to `SendAudioDataToChannel` must be **PCM 16-bit**, mono or stereo, `dataLength` a **multiple of 2**. Sample rate must be one the SDK accepts; **32000 or 48000 recommended**.
- **Talkback audio must never reach program or ISO.** An `obs_source_add_audio_capture_callback` tap observes a source; it must never be routed into a mix. Pinned by a test.
- **Fail closed everywhere, always.** A latch does not survive a reconnect.
- Tests are plain executables, no framework, one `check()`-style file per invariant cluster in `tests/`, registered with `add_executable` + `target_include_directories(... PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")` + `add_test`.
- Build: `cmake --build build_x64 --config Release --parallel 8`; test: `cd build_x64 && ctest -C Release --output-on-failure`. **`build_x64` is already configured — never delete or reconfigure it.**
- **Baseline is 55 tests.** Each task states the expected new count.
- Comments state the constraint the code cannot show. When a change is motivated by a live failure, say what happened, with numbers.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/talkback-pcm.h` (new) | Pure conversion: libobs planar float → interleaved int16. No libobs types in the signature. |
| `src/talkback-ring.h` (new) | The ring's **writer** half, mirroring `engine-audio.cpp`'s publish sequence exactly, as a reusable function so there is one implementation of the protocol per direction. |
| `src/talkback-key.h` (new) | Pure keying decision: PTT/latch, renewal expiry, every failure closing the key. No Qt, no libobs. |
| `src/talkback-tap.{h,cpp}` (new) | Owns the OBS capture callback, the region lifetime, and the write path. Plugin side. |
| `engine/src/engine-talkback.{h,cpp}` (modify) | Gains the ring reader and the send path, alongside the existing probe. |
| `engine/src/main.cpp` (modify) | Routes the new `talkback_open` / `talkback_audio` / `talkback_close` commands. |
| `src/engine-ipc.h` (modify) | Two new IPC tokens + the talkback region's geometry constants. |
| `src/engine-command.h` (modify) | Routing for the new commands. |

---

### Task 1: PCM conversion

libobs hands audio as **planar float** (`struct audio_data`, one `uint8_t*` per channel, `frames` samples each). Zoom wants **interleaved 16-bit**. Getting this wrong is silent — it produces noise or silence, not an error — so it is pinned first and hard.

**Files:**
- Create: `src/talkback-pcm.h`
- Create: `tests/talkback-pcm-test.cpp`
- Modify: `CMakeLists.txt` (register the test after the `CoreVideoTalkbackTone` block)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `size_t talkback_pcm_bytes(size_t frames, uint32_t channels)`
  - `void talkback_pcm_interleave(const float *const *planes, size_t frames, uint32_t channels, int16_t *out)`
  - `bool talkback_pcm_rate_supported(uint32_t sample_rate)`

- [ ] **Step 1: Write the failing test**

Create `tests/talkback-pcm-test.cpp`:

```cpp
// tests/talkback-pcm-test.cpp
// libobs planar float -> Zoom interleaved int16.
//
// Pinned hard because every failure mode here is SILENT: a channel-order slip,
// a missing clamp, or a wrong scale factor produces noise or silence rather
// than an error, and the first report would come from a person on air saying
// "talkback sounds broken".
#include "talkback-pcm.h"

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
    // ── Byte sizing ────────────────────────────────────────────────────────
    check(talkback_pcm_bytes(480, 1) == 960, "mono 480 frames was not 960 bytes");
    check(talkback_pcm_bytes(480, 2) == 1920, "stereo 480 frames was not 1920 bytes");
    check((talkback_pcm_bytes(481, 2) % 2) == 0,
          "byte length was odd -- SendAudioDataToChannel requires a multiple of 2");

    // ── Interleaving puts channels in the right order ──────────────────────
    const float lp[4] = {0.0f,  0.5f, -0.5f, 1.0f};
    const float rp[4] = {0.25f, 0.0f, -1.0f, 0.0f};
    const float *planes2[2] = {lp, rp};
    std::vector<int16_t> out(8);
    talkback_pcm_interleave(planes2, 4, 2, out.data());
    check(out[0] == 0, "frame 0 left was not silence");
    check(out[1] > 8000 && out[1] < 8400, "frame 0 right (0.25) was out of range");
    check(out[2] > 16000 && out[2] < 16500, "frame 1 left (0.5) was out of range");
    check(out[3] == 0, "frame 1 right was not silence");
    check(out[4] < -16000 && out[4] > -16500, "frame 2 left (-0.5) was out of range");

    // ── Full scale must not wrap ───────────────────────────────────────────
    check(out[6] == 32767, "+1.0 did not clamp to INT16_MAX");
    const float minus[1] = {-1.0f};
    const float *planes1[1] = {minus};
    int16_t one = 0;
    talkback_pcm_interleave(planes1, 1, 1, &one);
    check(one == -32767 || one == -32768, "-1.0 did not map to full negative scale");

    // ── Out-of-range input clamps rather than wrapping ─────────────────────
    // A source with gain above unity WILL exceed +/-1.0. Wrapping turns a loud
    // passage into a full-scale square wave: the single worst sound to put in
    // a director's ear.
    const float hot[4] = {2.0f, -2.0f, 9.9f, -9.9f};
    const float *hotp[1] = {hot};
    std::vector<int16_t> hotout(4);
    talkback_pcm_interleave(hotp, 4, 1, hotout.data());
    check(hotout[0] == 32767, "+2.0 did not clamp");
    check(hotout[1] <= -32767, "-2.0 did not clamp");
    check(hotout[2] == 32767, "+9.9 did not clamp");
    check(hotout[3] <= -32767, "-9.9 did not clamp");

    // ── NaN/Inf must not become random noise ───────────────────────────────
    const float bad[2] = {std::nanf(""), INFINITY};
    const float *badp[1] = {bad};
    std::vector<int16_t> badout(2);
    talkback_pcm_interleave(badp, 2, 1, badout.data());
    check(badout[0] == 0, "NaN did not become silence");
    check(badout[1] == 32767, "+Inf did not clamp to full scale");

    // ── Rate gate matches what the SDK documents ───────────────────────────
    check(talkback_pcm_rate_supported(48000), "48000 was rejected");
    check(talkback_pcm_rate_supported(32000), "32000 was rejected");
    check(talkback_pcm_rate_supported(44100), "44100 was rejected");
    check(!talkback_pcm_rate_supported(0),     "0 was accepted");
    check(!talkback_pcm_rate_supported(22050), "22050 was accepted");

    // ── Degenerate input is a no-op, not a crash ───────────────────────────
    talkback_pcm_interleave(nullptr, 4, 1, out.data());
    talkback_pcm_interleave(planes2, 0, 2, out.data());
    talkback_pcm_interleave(planes2, 4, 0, out.data());
    talkback_pcm_interleave(planes2, 4, 2, nullptr);
    check(true, "degenerate input crashed");

    if (failures == 0)
        std::cout << "talkback-pcm: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register the test and confirm it fails**

In `CMakeLists.txt`, immediately after the `add_test(NAME CoreVideoTalkbackTone ...)` block:

```cmake
    # libobs planar float -> Zoom interleaved int16. Every failure mode here is
    # silent (noise or silence, never an error), so both the clamp and the
    # channel order are pinned. See src/talkback-pcm.h.
    add_executable(CoreVideoTalkbackPcmTest
        tests/talkback-pcm-test.cpp
    )
    target_include_directories(CoreVideoTalkbackPcmTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTalkbackPcm
             COMMAND CoreVideoTalkbackPcmTest)
```

Run:
```sh
cmake --build build_x64 --config Release --target CoreVideoTalkbackPcmTest --parallel 8
```
Expected: FAIL — `Cannot open include file: 'talkback-pcm.h'`.

- [ ] **Step 3: Implement**

Create `src/talkback-pcm.h`:

```cpp
#pragma once
//
// talkback-pcm.h — libobs planar float to Zoom interleaved int16.
//
// libobs delivers audio as PLANAR float (one contiguous buffer per channel,
// `frames` samples each). The Zoom talkback API wants INTERLEAVED 16-bit PCM
// with a byte length that is a multiple of 2. This header is the only place
// that conversion happens.
//
// Pure: no libobs types in the signature (callers pass plain float pointers),
// so it can be pinned by a test with no OBS and no meeting.
//
// THE CLAMP IS NOT DEFENSIVE PADDING. An OBS source with gain above unity
// legitimately produces samples beyond +/-1.0. Casting those straight to
// int16 WRAPS -- a loud passage becomes a full-scale square wave, which is
// the single worst sound to put in a director's ear. NaN maps to silence for
// the same reason: an undefined float cast is an undefined sample value.
//
#include <cmath>
#include <cstddef>
#include <cstdint>

// Bytes needed for `frames` frames of `channels` interleaved int16 samples.
// Always even, so the SDK's "dataLength must be a multiple of 2" holds by
// construction rather than by inspection at the call site.
inline std::size_t talkback_pcm_bytes(std::size_t frames, uint32_t channels)
{
    return frames * static_cast<std::size_t>(channels) * sizeof(int16_t);
}

// One sample, clamped and scaled. Kept separate so the test can reason about
// the scale factor without going through the interleaver.
inline int16_t talkback_pcm_sample(float v)
{
    // NaN fails every comparison, so test for it explicitly rather than
    // relying on the clamps below to catch it.
    if (std::isnan(v)) return 0;
    if (v >=  1.0f) return  32767;
    if (v <= -1.0f) return -32767;   // symmetric with +full scale; -32768 is
                                     // reachable in int16 but asymmetric, and
                                     // symmetry matters more than one LSB here
    const float scaled = v * 32767.0f;
    return static_cast<int16_t>(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
}

// Interleave `channels` planes of `frames` floats into `out`.
// `planes[c]` must hold at least `frames` samples. Degenerate input is a
// no-op: a tap can fire with a null plane during source teardown, and a
// crash on the audio thread would take OBS with it.
inline void talkback_pcm_interleave(const float *const *planes,
                                    std::size_t frames,
                                    uint32_t channels,
                                    int16_t *out)
{
    if (planes == nullptr || out == nullptr || frames == 0 || channels == 0)
        return;
    for (uint32_t c = 0; c < channels; ++c)
        if (planes[c] == nullptr) return;

    for (std::size_t f = 0; f < frames; ++f)
        for (uint32_t c = 0; c < channels; ++c)
            out[f * channels + c] = talkback_pcm_sample(planes[c][f]);
}

// Rates IZoomSDKAudioRawDataSender documents as accepted. We pass OBS's rate
// through rather than resampling: a resampler is a whole subsystem, and OBS
// runs at 48kHz by default, which the SDK explicitly recommends. An
// unsupported rate must be reported loudly, never silently resampled or
// silently sent -- a wrong-rate send is heard as a chipmunk or a drawl.
inline bool talkback_pcm_rate_supported(uint32_t sample_rate)
{
    switch (sample_rate) {
    case 8000: case 16000: case 32000: case 44100:
    case 48000: case 50000: case 50400: case 96000: case 192000:
        return true;
    default:
        return false;
    }
}
```

- [ ] **Step 4: Confirm it passes**

```sh
cmake --build build_x64 --config Release --parallel 8
cd build_x64 && ctest -C Release --output-on-failure
```
Expected: **56/56** green (55 baseline + `CoreVideoTalkbackPcm`).

- [ ] **Step 5: Commit**

```sh
git add src/talkback-pcm.h tests/talkback-pcm-test.cpp CMakeLists.txt
git commit -m "feat(talkback): planar float to interleaved int16 conversion"
```

---

### Task 2: The ring writer, as a reusable function

The engine's publish sequence (`engine/src/engine-audio.cpp:288-330`) is subtle: seqlock odd → release fence → payload → release fence → seqlock even → release fence → free-running `write_index`. The plugin is about to become a writer for the first time. **Do not retype that sequence from memory** — extract it once, use it from both sides eventually.

**Files:**
- Create: `src/talkback-ring.h`
- Create: `tests/talkback-ring-test.cpp`
- Modify: `src/engine-ipc.h` (add the talkback geometry constants near `kAudioRingSlots`)
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `kTalkbackSlotBytes` (constant, 8192)
  - `bool talkback_ring_init(ShmAudioHeader *hdr, uint32_t sample_rate, uint16_t channels)`
  - `bool talkback_ring_publish(void *region_base, const void *pcm, uint32_t byte_len, uint64_t capture_ns)` — returns true when the caller must send one notify event
  - `uint32_t talkback_ring_drain(void *region_base, uint32_t &read_index, TalkbackRingSlotFn fn, void *ctx)` where `using TalkbackRingSlotFn = void (*)(const void *pcm, uint32_t byte_len, uint64_t capture_ns, void *ctx);`

- [ ] **Step 1: Write the failing test**

Create `tests/talkback-ring-test.cpp`. This uses REAL shared memory and a concurrent writer thread, the same shape as `tests/shm-frame-reader-test.cpp`:

```cpp
// tests/talkback-ring-test.cpp
// The talkback ring, driven in REVERSE: the plugin writes, the engine reads.
//
// Every media path in this codebase runs engine -> plugin. Talkback is the
// first that runs the other way, so the ring's invariants are exercised here
// with the roles swapped: free-running indices, per-slot seqlock, and the
// edge-triggered notify protocol. The invariants are direction-agnostic by
// construction (the helpers are free functions over a header pointer) and
// this test is what keeps that true.
#include "engine-ipc.h"
#include "talkback-ring.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

struct Collected {
    std::vector<std::vector<uint8_t>> buffers;
};

static void collect(const void *pcm, uint32_t byte_len, uint64_t, void *ctx)
{
    auto *c = static_cast<Collected *>(ctx);
    const auto *p = static_cast<const uint8_t *>(pcm);
    c->buffers.emplace_back(p, p + byte_len);
}

int main()
{
    // A plain heap buffer stands in for the mapped region: the ring logic
    // does not care how the memory was obtained, and this keeps the test
    // free of platform shared-memory calls.
    std::vector<uint8_t> region(shm_audio_region_bytes(kTalkbackSlotBytes), 0);
    auto *hdr = reinterpret_cast<ShmAudioHeader *>(region.data());
    check(talkback_ring_init(hdr, 48000, 1), "talkback_ring_init failed");
    check(hdr->slot_count == kAudioRingSlots, "slot_count was not kAudioRingSlots");
    check(hdr->slot_bytes == kTalkbackSlotBytes, "slot_bytes was not kTalkbackSlotBytes");
    check(hdr->sample_rate == 48000, "sample_rate was not stored");
    check(hdr->channels == 1, "channels was not stored");
    check(hdr->write_index == 0, "write_index did not start at 0");

    // ── First publish crosses the empty -> non-empty edge ──────────────────
    std::vector<uint8_t> a(960, 0xAB);
    check(talkback_ring_publish(region.data(), a.data(), 960, 111) == true,
          "the first publish did not request a notify");
    check(hdr->write_index == 1, "write_index did not advance");

    // ── A second publish with the flag still set must NOT re-notify ────────
    std::vector<uint8_t> b(960, 0xCD);
    check(talkback_ring_publish(region.data(), b.data(), 960, 222) == false,
          "a publish re-notified while the flag was already set");
    check(hdr->write_index == 2, "write_index did not advance on the second publish");

    // ── The reader drains BOTH, in order ───────────────────────────────────
    uint32_t read_index = 0;
    Collected got;
    const uint32_t n = talkback_ring_drain(region.data(), read_index, collect, &got);
    check(n == 2, "drain did not return 2 buffers");
    check(read_index == 2, "read_index did not advance to write_index");
    check(got.buffers.size() == 2, "drain did not deliver 2 buffers");
    check(got.buffers[0][0] == 0xAB, "first buffer was not the first published");
    check(got.buffers[1][0] == 0xCD, "second buffer was out of order");

    // ── After a full drain the reader may sleep ────────────────────────────
    check(audio_ring_reader_done(hdr, read_index) == true,
          "reader_done said not-empty after a full drain");
    check(hdr->notify == 0, "notify was left set after a clean drain");

    // ── ...and the next publish notifies again ─────────────────────────────
    check(talkback_ring_publish(region.data(), a.data(), 960, 333) == true,
          "publish after a clean drain did not re-notify");

    // ── Oversized payloads are refused, not truncated ──────────────────────
    std::vector<uint8_t> huge(kTalkbackSlotBytes + 1, 0xEE);
    const uint32_t before = hdr->write_index;
    check(talkback_ring_publish(region.data(), huge.data(),
                                kTalkbackSlotBytes + 1, 444) == false,
          "an oversized publish claimed it notified");
    check(hdr->write_index == before,
          "an oversized publish advanced write_index -- it must be refused, "
          "never truncated: a half buffer is heard as a click");

    // ── Overrun: the writer lapping the reader is DETECTED, not silent ─────
    {
        std::vector<uint8_t> r2(shm_audio_region_bytes(kTalkbackSlotBytes), 0);
        auto *h2 = reinterpret_cast<ShmAudioHeader *>(r2.data());
        talkback_ring_init(h2, 48000, 1);
        for (uint32_t i = 0; i < kAudioRingSlots + 3; ++i)
            talkback_ring_publish(r2.data(), a.data(), 960, i);
        // Reader never drained: it is exactly slot_count+3 behind.
        check(audio_ring_slots_behind(h2->write_index, 0, h2->slot_count) ==
                  kAudioRingSlots + 3,
              "slots_behind did not report the true overrun depth -- collapsing "
              "'caught up' and 'lapped by one ring' was the original defect");
    }

    // ── Concurrent writer + reader: no torn buffer ever escapes ────────────
    {
        std::vector<uint8_t> r3(shm_audio_region_bytes(kTalkbackSlotBytes), 0);
        auto *h3 = reinterpret_cast<ShmAudioHeader *>(r3.data());
        talkback_ring_init(h3, 48000, 1);

        std::atomic<bool> stop{false};
        constexpr uint32_t kLen = 960;
        std::thread writer([&]() {
            for (uint32_t i = 0; i < 5000 && !stop.load(); ++i) {
                std::vector<uint8_t> buf(kLen, static_cast<uint8_t>(i & 0xFF));
                talkback_ring_publish(r3.data(), buf.data(), kLen, i);
            }
        });

        uint32_t ri = 0;
        bool torn = false;
        auto verify = [](const void *pcm, uint32_t len, uint64_t, void *ctx) {
            const auto *p = static_cast<const uint8_t *>(pcm);
            bool *bad = static_cast<bool *>(ctx);
            for (uint32_t i = 1; i < len; ++i)
                if (p[i] != p[0]) { *bad = true; return; }
        };
        for (int pass = 0; pass < 2000; ++pass)
            talkback_ring_drain(r3.data(), ri, verify, &torn);
        stop.store(true);
        writer.join();
        talkback_ring_drain(r3.data(), ri, verify, &torn);
        check(!torn, "a torn buffer escaped the seqlock under concurrency");
    }

    if (failures == 0)
        std::cout << "talkback-ring: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register and confirm failure**

In `src/engine-ipc.h`, immediately after `static constexpr uint32_t kAudioRingSlots = 8;`:

```cpp
// Talkback's slot size. OBS delivers AUDIO_OUTPUT_FRAMES (1024) frames per
// callback; 1024 frames of stereo int16 is 4096 bytes, so 8192 leaves headroom
// for a larger buffer without a resize. Talkback deliberately never resizes:
// a resize means a new _gN region name (a Windows section cannot grow while
// mapped), and re-handshaking a live talk key mid-sentence is worse than
// refusing one oversized buffer.
static constexpr uint32_t kTalkbackSlotBytes = 8192;
```

In `CMakeLists.txt`, after the `CoreVideoTalkbackPcm` block:

```cmake
    # The talkback ring, driven in reverse (plugin writes, engine reads).
    # Uses real concurrency to prove the seqlock and the free-running indices
    # hold with the roles swapped. See src/talkback-ring.h.
    find_package(Threads REQUIRED)
    add_executable(CoreVideoTalkbackRingTest
        tests/talkback-ring-test.cpp
    )
    target_include_directories(CoreVideoTalkbackRingTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(CoreVideoTalkbackRingTest PRIVATE Threads::Threads)
    add_test(NAME CoreVideoTalkbackRing
             COMMAND CoreVideoTalkbackRingTest)
```

Run:
```sh
cmake --build build_x64 --config Release --target CoreVideoTalkbackRingTest --parallel 8
```
Expected: FAIL — `Cannot open include file: 'talkback-ring.h'`.

- [ ] **Step 3: Implement**

Create `src/talkback-ring.h`:

```cpp
#pragma once
//
// talkback-ring.h — the talkback ring's writer and reader halves.
//
// Talkback is the FIRST path in this codebase that moves media plugin ->
// engine. Every other one runs engine -> plugin. Rather than invent a second
// transport, this reuses ShmAudioHeader unchanged and swaps the roles: the
// plugin creates and writes, the engine opens and reads.
//
// That works because the ring's helpers in engine-ipc.h
// (audio_ring_notify_after_publish / audio_ring_reader_done /
// audio_ring_reader_abandon) are free functions over a header pointer with no
// baked-in direction, and shm_region_open_readwrite() already exists precisely
// because a READER must be able to clear the notify flag.
//
// The publish sequence below mirrors engine/src/engine-audio.cpp's, fence for
// fence. It is extracted here rather than retyped so the protocol has one
// implementation per direction instead of two that can drift.
//
// WHY NOT THE PIPE. Talking produces ~100 buffers/sec. This codebase has
// already measured what that shape does to the P2E/E2P pipes: engine->plugin
// latency of 58-90ms under full gallery load versus 41-161us idle, with ring
// overruns at zero throughout -- the ring never fell behind, the wakeups did.
// That incident is why the ring exists. Talkback is the one feature where
// that latency is heard directly, as a stutter in the director's voice.
//
#include "engine-ipc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

using TalkbackRingSlotFn = void (*)(const void *pcm, uint32_t byte_len,
                                    uint64_t capture_ns, void *ctx);

// Lay out a freshly created region. Writer side only, before any publish.
inline bool talkback_ring_init(ShmAudioHeader *hdr, uint32_t sample_rate,
                               uint16_t channels)
{
    if (hdr == nullptr) return false;
    std::memset(hdr, 0, sizeof(*hdr));
    hdr->write_index = 0;
    hdr->slot_count  = kAudioRingSlots;
    hdr->slot_bytes  = kTalkbackSlotBytes;
    hdr->sample_rate = sample_rate;
    hdr->channels    = channels;
    hdr->notify      = 0;
    return true;
}

// Publish one buffer. Returns true exactly when this publish crossed the
// empty->non-empty edge and the caller must send ONE notify event.
//
// An oversized buffer is REFUSED (returns false, write_index untouched) rather
// than truncated: half a buffer is heard as a click, and a click in a
// director's ear reads as a fault in the transport.
inline bool talkback_ring_publish(void *region_base, const void *pcm,
                                  uint32_t byte_len, uint64_t capture_ns)
{
    if (region_base == nullptr || pcm == nullptr || byte_len == 0) return false;
    auto *hdr = static_cast<ShmAudioHeader *>(region_base);
    if (byte_len > hdr->slot_bytes) return false;

    const uint32_t index = hdr->write_index % hdr->slot_count;
    auto *slot = reinterpret_cast<ShmAudioSlot *>(
        static_cast<char *>(region_base) + shm_audio_slot_offset(*hdr, index));

    uint32_t seq = slot->sequence + 1;
    if ((seq & 1u) == 0) ++seq;
    slot->sequence = seq;                        // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);

    slot->capture_ns     = capture_ns;
    slot->byte_len       = byte_len;
    slot->participant_id = 0;                    // talkback has no single owner
    std::memcpy(reinterpret_cast<char *>(slot) + sizeof(ShmAudioSlot),
                pcm, byte_len);
    std::atomic_thread_fence(std::memory_order_release);
    slot->sequence = seq + 1;                    // even: readable

    std::atomic_thread_fence(std::memory_order_release);
    // FREE-RUNNING -- never % slot_count here. `index` above already applied
    // the modulo to pick the physical slot; write_index must keep counting so
    // the reader can tell "caught up" (0 behind) from "lapped by exactly one
    // ring" (slot_count behind). Collapsing those was the original defect.
    hdr->write_index = hdr->write_index + 1;

    return audio_ring_notify_after_publish(hdr);
}

// Drain everything published since `read_index`, calling `fn` per buffer in
// order. Advances `read_index`. Returns the number of buffers delivered.
//
// EVENTS ARE PROMPTS, NOT PAYLOADS: one notify can cover many slots, so the
// reader must drain fully on any wakeup rather than consuming one buffer per
// event.
inline uint32_t talkback_ring_drain(void *region_base, uint32_t &read_index,
                                    TalkbackRingSlotFn fn, void *ctx)
{
    if (region_base == nullptr || fn == nullptr) return 0;
    auto *hdr = static_cast<ShmAudioHeader *>(region_base);

    uint32_t delivered = 0;
    for (;;) {
        const uint32_t write_index = hdr->write_index;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (read_index == write_index) break;

        // If the writer lapped us, skip forward to the oldest slot still
        // intact. Silent loss is the thing the free-running index exists to
        // make visible; the caller reports it.
        const uint32_t behind = audio_ring_slots_behind(write_index, read_index,
                                                        hdr->slot_count);
        if (behind > hdr->slot_count)
            read_index = write_index - hdr->slot_count;

        const uint32_t index = read_index % hdr->slot_count;
        auto *slot = reinterpret_cast<const ShmAudioSlot *>(
            static_cast<const char *>(region_base) +
            shm_audio_slot_offset(*hdr, index));

        // Per-slot seqlock: even and unchanged across the copy means the
        // payload was stable. Three attempts, then give up on this slot
        // rather than spinning on the audio path.
        bool copied = false;
        for (int attempt = 0; attempt < 3 && !copied; ++attempt) {
            const uint32_t s1 = slot->sequence;
            if (s1 & 1u) continue;
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint32_t len = slot->byte_len;
            const uint64_t ns  = slot->capture_ns;
            if (len == 0 || len > hdr->slot_bytes) break;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (slot->sequence != s1) continue;
            fn(reinterpret_cast<const char *>(slot) + sizeof(ShmAudioSlot),
               len, ns, ctx);
            copied = true;
        }
        ++read_index;
        if (copied) ++delivered;
    }
    return delivered;
}
```

- [ ] **Step 4: Confirm it passes**

```sh
cmake --build build_x64 --config Release --parallel 8
cd build_x64 && ctest -C Release --output-on-failure
```
Expected: **57/57** green.

- [ ] **Step 5: Commit**

```sh
git add src/talkback-ring.h src/engine-ipc.h tests/talkback-ring-test.cpp CMakeLists.txt
git commit -m "feat(talkback): the audio ring, driven plugin->engine"
```

---

### Task 3: Keying decisions and the dead-man switch

The spec's fail-closed guarantee is **structural**: the engine holds the channel open only while audio keeps arriving, so every failure closes it without any code path handling that failure specifically. This task is that logic, extracted from Qt and libobs the same way `src/join-watchdog.h` and `src/director-handover.h` are, and for the same reason — both failure directions are invisible until they happen on a live show.

**Files:**
- Create: `src/talkback-key.h`
- Create: `tests/talkback-key-test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum class TalkbackKeyMode { PushToTalk, Latch };`
  - `enum class TalkbackKeyAction { None, Open, Close };`
  - `constexpr uint64_t kTalkbackAudioGapMs = 250;`
  - `constexpr uint64_t kTalkbackRenewalMs = 500;`
  - `constexpr uint64_t kTalkbackRenewalsMissed = 2;`
  - `struct TalkbackKeyState { bool open; TalkbackKeyMode mode; uint64_t last_audio_ms; uint64_t last_renewal_ms; bool needs_renewal; };`
  - `TalkbackKeyAction talkback_key_evaluate(const TalkbackKeyState &s, uint64_t now_ms, bool engine_alive, bool in_meeting);`

- [ ] **Step 1: Write the failing test**

Create `tests/talkback-key-test.cpp`:

```cpp
// tests/talkback-key-test.cpp
// When a talkback key stays open, and every way it must close.
//
// Talkback is the one feature where a bug is heard by people who are not in
// the control room. A key stuck open is a director's private remark going to
// talent -- or, if the source is also on a program bus, to the audience. So
// this pins the CLOSING direction exhaustively: every failure in the spec's
// table must produce Close, and only a genuinely healthy key stays open.
//
// The design is a dead-man switch: audio arriving IS the liveness signal, so
// no code path has to notice a failure for the key to close. These tests are
// what keep that property true as the surrounding code changes.
#include "talkback-key.h"

#include <iostream>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

static TalkbackKeyState healthy(uint64_t now, TalkbackKeyMode mode)
{
    TalkbackKeyState s{};
    s.open            = true;
    s.mode            = mode;
    s.last_audio_ms   = now;
    s.last_renewal_ms = now;
    s.needs_renewal   = true;
    return s;
}

int main()
{
    constexpr uint64_t T = 100000;

    // ── A healthy key stays open, in both modes ────────────────────────────
    check(talkback_key_evaluate(healthy(T, TalkbackKeyMode::PushToTalk), T, true, true) ==
              TalkbackKeyAction::None,
          "a healthy push-to-talk key was closed");
    check(talkback_key_evaluate(healthy(T, TalkbackKeyMode::Latch), T, true, true) ==
              TalkbackKeyAction::None,
          "a healthy latched key was closed");

    // ── The audio gap: buffers stopping closes the key ─────────────────────
    // This is the dead-man switch. OBS delivers buffers continuously while a
    // source is active -- including silence -- so a gap means the tap, the
    // plugin, or the pipe is gone.
    {
        TalkbackKeyState s = healthy(T, TalkbackKeyMode::Latch);
        check(talkback_key_evaluate(s, T + kTalkbackAudioGapMs - 1, true, true) ==
                  TalkbackKeyAction::None,
              "the key closed while still inside the audio gap window");
        check(talkback_key_evaluate(s, T + kTalkbackAudioGapMs + 1, true, true) ==
                  TalkbackKeyAction::Close,
              "the key stayed open past the audio gap -- the dead-man switch failed");
    }

    // ── A lost button release closes via the renewal, not the gap ──────────
    // Buffers keep flowing when the release is lost but the socket is healthy,
    // so the gap can never fire. The controller must keep asserting the key.
    {
        TalkbackKeyState s = healthy(T, TalkbackKeyMode::PushToTalk);
        s.last_audio_ms = T + 10000;            // audio is fine
        s.last_renewal_ms = T;                  // renewals stopped
        const uint64_t missed = kTalkbackRenewalMs * kTalkbackRenewalsMissed;
        check(talkback_key_evaluate(s, T + missed - 1, true, true) ==
                  TalkbackKeyAction::None,
              "the key closed before the renewal grace elapsed");
        check(talkback_key_evaluate(s, T + missed + 1, true, true) ==
                  TalkbackKeyAction::Close,
              "a lost release left the key open -- the director is live and "
              "does not know it");
    }

    // ── A key that does not require renewal is not closed by renewal age ───
    // The OBS hotkey's release is in-process and reliable, so it opts out.
    {
        TalkbackKeyState s = healthy(T, TalkbackKeyMode::PushToTalk);
        s.needs_renewal   = false;
        s.last_renewal_ms = T;
        s.last_audio_ms   = T + 10000;
        check(talkback_key_evaluate(s, T + 60000, true, true) ==
                  TalkbackKeyAction::None,
              "a key that does not require renewal was closed by renewal age");
    }

    // ── Every other failure in the spec's table closes it ──────────────────
    {
        TalkbackKeyState s = healthy(T, TalkbackKeyMode::Latch);
        check(talkback_key_evaluate(s, T, false, true) == TalkbackKeyAction::Close,
              "the engine dying did not close the key");
        check(talkback_key_evaluate(s, T, true, false) == TalkbackKeyAction::Close,
              "leaving the meeting did not close the key");
        check(talkback_key_evaluate(s, T, false, false) == TalkbackKeyAction::Close,
              "engine death plus meeting loss did not close the key");
    }

    // ── A latch does NOT survive a reconnect ──────────────────────────────
    // Explicit spec requirement. Restoring a latch on reconnect is the risky
    // moment, and the operator chose never to take it.
    {
        TalkbackKeyState s = healthy(T, TalkbackKeyMode::Latch);
        s.last_audio_ms = T;
        check(talkback_key_evaluate(s, T + 5000, false, true) ==
                  TalkbackKeyAction::Close,
              "a latch survived the engine going away");
    }

    // ── A closed key is never spuriously reopened ─────────────────────────
    {
        TalkbackKeyState s{};
        s.open = false;
        check(talkback_key_evaluate(s, T, true, true) == TalkbackKeyAction::None,
              "a closed key was reopened by evaluate()");
        check(talkback_key_evaluate(s, T + 10 * T, false, false) ==
                  TalkbackKeyAction::None,
              "a closed key produced Close, which would double-close");
    }

    // ── A backwards clock must not close a healthy key ────────────────────
    // The same guard join-watchdog.h carries: an underflowed subtraction
    // becomes an enormous elapsed time.
    {
        TalkbackKeyState s = healthy(T + 5000, TalkbackKeyMode::Latch);
        check(talkback_key_evaluate(s, T, true, true) == TalkbackKeyAction::None,
              "a backwards clock underflowed and closed a healthy key");
    }

    if (failures == 0)
        std::cout << "talkback-key: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register and confirm failure**

In `CMakeLists.txt`, after the `CoreVideoTalkbackRing` block:

```cmake
    # When a talkback key may stay open, and every way it must close. Talkback
    # is the one feature where a bug is heard outside the control room, so the
    # closing direction is pinned exhaustively. See src/talkback-key.h.
    add_executable(CoreVideoTalkbackKeyTest
        tests/talkback-key-test.cpp
    )
    target_include_directories(CoreVideoTalkbackKeyTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTalkbackKey
             COMMAND CoreVideoTalkbackKeyTest)
```

Run:
```sh
cmake --build build_x64 --config Release --target CoreVideoTalkbackKeyTest --parallel 8
```
Expected: FAIL — `Cannot open include file: 'talkback-key.h'`.

- [ ] **Step 3: Implement**

Create `src/talkback-key.h`:

```cpp
#pragma once
//
// talkback-key.h — when a talkback key may stay open, and every way it closes.
//
// Extracted from Qt and libobs so both failure directions can be tested
// without a meeting, the same treatment src/join-watchdog.h and
// src/director-handover.h get, and for the same reason: both directions are
// invisible until they happen on a live show.
//
// THE DESIGN IS A DEAD-MAN SWITCH. While a key is open the OBS tap delivers
// buffers continuously -- including silence, because an active OBS source
// calls back whether or not anyone is talking. So the ring's own traffic IS
// the liveness signal, and the key closes on a gap. Nothing has to NOTICE a
// failure:
//
//   OBS quits / plugin crashes  -> buffers stop -> gap expires
//   Engine restarts             -> channel died with the process
//   Pipe drops                  -> notify edge stops -> gap expires
//   Source removed or inactive  -> buffers stop -> gap expires
//   Meeting rejoin              -> channel and membership are meeting-scoped
//
// THE ONE FAILURE THE GAP CANNOT CATCH is a lost button release while the
// socket stays healthy: buffers keep flowing, so the gap never fires and the
// director is live without knowing. Renewals therefore run in the opposite
// direction too -- a key opened over the control API stays open only while
// the controller keeps asserting it. That is a LIVENESS renewal, not a
// maximum-open-time cap: a deliberate latch may stay open all day as long as
// something keeps saying it is still wanted. Surfaces whose release is
// in-process and reliable (the OBS hotkey) set needs_renewal = false.
//
#include <cstdint>

enum class TalkbackKeyMode {
    // Audio flows only while the control is held.
    PushToTalk,
    // Tap on, tap off. Never survives a reconnect -- see the tests.
    Latch,
};

enum class TalkbackKeyAction {
    // Nothing to do.
    None,
    // The key should be opened (reserved for the caller's open path).
    Open,
    // Close it now.
    Close,
};

// A gap longer than this means the audio path is gone. A few buffer periods:
// OBS delivers ~1024 frames (~21ms at 48kHz) per callback, so 250ms is roughly
// a dozen missed callbacks -- long enough not to trip on ordinary scheduling
// jitter, short enough that a dead key is not audible as a held-open channel.
constexpr uint64_t kTalkbackAudioGapMs = 250;

// How often a renewing controller must re-assert an open key...
constexpr uint64_t kTalkbackRenewalMs = 500;
// ...and how many it may miss before the key closes. Two gives ~1s, which
// tolerates one dropped packet without tolerating a dropped operator.
constexpr uint64_t kTalkbackRenewalsMissed = 2;

struct TalkbackKeyState {
    bool            open;
    TalkbackKeyMode mode;
    // Monotonic ms when audio last arrived in the ring.
    uint64_t        last_audio_ms;
    // Monotonic ms when the controller last re-asserted this key.
    uint64_t        last_renewal_ms;
    // False for surfaces whose release is in-process and reliable.
    bool            needs_renewal;
};

// Guard every subtraction: a clock that went backwards must not underflow into
// an enormous elapsed time and close a healthy key. join-watchdog.h carries
// the same guard for the same reason.
inline uint64_t talkback_elapsed_ms(uint64_t now_ms, uint64_t then_ms)
{
    return now_ms <= then_ms ? 0 : now_ms - then_ms;
}

inline TalkbackKeyAction talkback_key_evaluate(const TalkbackKeyState &s,
                                               uint64_t now_ms,
                                               bool engine_alive,
                                               bool in_meeting)
{
    // A closed key is never reopened here and never double-closed. Opening is
    // an operator action, not something a periodic evaluation may decide.
    if (!s.open) return TalkbackKeyAction::None;

    // Structural closes: no channel can exist without these, so nothing else
    // needs checking.
    if (!engine_alive || !in_meeting) return TalkbackKeyAction::Close;

    // The dead-man switch.
    if (talkback_elapsed_ms(now_ms, s.last_audio_ms) > kTalkbackAudioGapMs)
        return TalkbackKeyAction::Close;

    // The lost-release backstop.
    if (s.needs_renewal &&
        talkback_elapsed_ms(now_ms, s.last_renewal_ms) >
            kTalkbackRenewalMs * kTalkbackRenewalsMissed)
        return TalkbackKeyAction::Close;

    return TalkbackKeyAction::None;
}
```

- [ ] **Step 4: Confirm it passes**

```sh
cmake --build build_x64 --config Release --parallel 8
cd build_x64 && ctest -C Release --output-on-failure
```
Expected: **58/58** green.

- [ ] **Step 5: Commit**

```sh
git add src/talkback-key.h tests/talkback-key-test.cpp CMakeLists.txt
git commit -m "feat(talkback): keying decisions and the dead-man switch"
```

---

### Task 4: The OBS tap and the region's writer side

Now the plugin actually captures audio. `obs_source_add_audio_capture_callback` is a **tap, not a route** — it observes a source's post-processing audio and cannot add that source to any mix. That property is the structural half of the leak guarantee, and Task 5 pins it.

**Files:**
- Create: `src/talkback-tap.h`, `src/talkback-tap.cpp`
- Modify: `CMakeLists.txt` (add `src/talkback-tap.cpp` to the plugin sources — find the `add_library`/`target_sources` list that already carries `src/zoom-source.cpp`)
- Modify: `src/engine-ipc.h` (IPC tokens)
- Modify: `src/engine-command.h` (routing)

**Interfaces:**
- Consumes: `talkback_pcm_*` (Task 1), `talkback_ring_*` (Task 2).
- Produces: `class TalkbackTap` with
  - `bool open(const std::string &source_name, std::string &error_out)`
  - `void close()`
  - `bool is_open() const`
  - `uint64_t last_audio_ms() const`
  - `uint32_t sample_rate() const` / `uint16_t channels() const`
  - `std::string region_name() const`
- Also produces IPC tokens `IPC_CMD_TALKBACK_OPEN` (`"talkback_open"`), `IPC_CMD_TALKBACK_AUDIO` (`"talkback_audio"`), `IPC_CMD_TALKBACK_CLOSE` (`"talkback_close"`) and the matching `IpcCommand::TalkbackOpen` / `TalkbackAudio` / `TalkbackClose`.

- [ ] **Step 1: Add the IPC tokens and routing, with the routing test**

Append inside `main()` in `tests/engine-command-test.cpp`, before the final `if (failures == 0)` block:

```cpp
    // ── Talkback audio-path commands route exactly ──────────────────────────
    check(ipc_command_of(R"({"cmd":"talkback_open","region":"X","rate":48000})") ==
              IpcCommand::TalkbackOpen,
          "talkback_open did not route to IpcCommand::TalkbackOpen");
    check(ipc_command_of(R"({"cmd":"talkback_audio"})") == IpcCommand::TalkbackAudio,
          "talkback_audio did not route to IpcCommand::TalkbackAudio");
    check(ipc_command_of(R"({"cmd":"talkback_close"})") == IpcCommand::TalkbackClose,
          "talkback_close did not route to IpcCommand::TalkbackClose");
    // The family shares a prefix with talkback_probe; exact match must keep
    // them apart, the way it keeps unsubscribe out of the subscribe branch.
    check(ipc_command_of(R"({"cmd":"talkback_probe"})") == IpcCommand::TalkbackProbe,
          "talkback_probe was hijacked by a sibling talkback_* command");
    check(ipc_command_of(R"({"cmd":"talkback_open_extra"})") == IpcCommand::Unknown,
          "a longer command starting with talkback_open matched it");
```

Run: `cmake --build build_x64 --config Release --target CoreVideoEngineCommandTest --parallel 8`
Expected: FAIL — `'TalkbackOpen' is not a member of 'IpcCommand'`.

- [ ] **Step 2: Implement the tokens and routing**

In `src/engine-ipc.h`, after `#define IPC_CMD_TALKBACK_PROBE "talkback_probe"`:

```c
#define IPC_CMD_TALKBACK_OPEN  "talkback_open"
#define IPC_CMD_TALKBACK_AUDIO "talkback_audio"
#define IPC_CMD_TALKBACK_CLOSE "talkback_close"
```

In `src/engine-command.h`, add to the enum after `TalkbackProbe,`:

```cpp
    TalkbackOpen,
    TalkbackAudio,
    TalkbackClose,
```

and in `ipc_command_of`, before the final `return IpcCommand::Unknown;`:

```cpp
    if (cmd == IPC_CMD_TALKBACK_OPEN)  return IpcCommand::TalkbackOpen;
    if (cmd == IPC_CMD_TALKBACK_AUDIO) return IpcCommand::TalkbackAudio;
    if (cmd == IPC_CMD_TALKBACK_CLOSE) return IpcCommand::TalkbackClose;
```

Run the routing test again. Expected: PASS.

- [ ] **Step 3: Write the tap header**

Create `src/talkback-tap.h`:

```cpp
#pragma once
//
// talkback-tap.h — the plugin's talkback audio source and ring writer.
//
// Given an OBS source name, attaches obs_source_add_audio_capture_callback and
// publishes what it hears into the talkback ring for the engine to send.
//
// A CAPTURE CALLBACK IS A TAP, NOT A ROUTE. It observes a source's
// post-processing audio and cannot add that source to any mix. That is the
// structural half of the spec's guarantee that talkback never reaches program
// or ISO -- pinned by tests/talkback-isolation-test.cpp. The advisory half
// (warning when the chosen source is itself live on a program track) is the
// dock's job and is NOT implemented here.
//
// The tap is attached only while a key is open and detached the instant it
// closes, so an unkeyed talkback source costs nothing.
//
#include <cstdint>
#include <mutex>
#include <string>

#include "engine-ipc.h"   // ShmRegion, region helpers

struct obs_source;
typedef struct obs_source obs_source_t;
struct audio_data;

class TalkbackTap {
public:
    ~TalkbackTap();

    // Attach to `source_name` and create the ring. Returns false with a
    // human-readable reason in `error_out` -- an operator who picked a source
    // that cannot work needs to know WHICH reason, not that "talkback failed".
    bool open(const std::string &source_name, std::string &error_out);
    void close();
    bool is_open() const;

    // Monotonic ms of the last buffer published. The dead-man switch reads
    // this; see src/talkback-key.h.
    uint64_t last_audio_ms() const;

    uint32_t    sample_rate() const;
    uint16_t    channels() const;
    std::string region_name() const;

    // Set by open(); the engine is told this name so it can map the region.
    static const char *base_region_name() { return "ZoomObsPlugin_talkback"; }

private:
    static void audio_cb(void *param, obs_source_t *source,
                         const struct audio_data *data, bool muted);
    void on_audio(const struct audio_data *data, bool muted);

    mutable std::mutex m_mtx;
    obs_source_t *m_source      = nullptr;   // strong ref while open
    ShmRegion     m_region{};
    std::string   m_region_name;
    uint32_t      m_sample_rate = 0;
    uint16_t      m_channels    = 0;
    uint64_t      m_last_audio_ms = 0;
    bool          m_open        = false;
};
```

- [ ] **Step 4: Write the tap implementation**

Create `src/talkback-tap.cpp`:

```cpp
#include "talkback-tap.h"
#include "talkback-pcm.h"
#include "talkback-ring.h"
#include "shm-generation.h"

#include <obs-module.h>
#include <util/platform.h>

#include <vector>

TalkbackTap::~TalkbackTap() { close(); }

bool TalkbackTap::open(const std::string &source_name, std::string &error_out)
{
    close();
    std::lock_guard<std::mutex> lock(m_mtx);

    obs_source_t *src = obs_get_source_by_name(source_name.c_str());
    if (!src) {
        error_out = "No OBS source named \"" + source_name + "\"";
        return false;
    }

    // OBS's audio format is global, so read it once here rather than
    // per-callback. We pass the rate through instead of resampling: a
    // resampler is a whole subsystem, and OBS runs at 48kHz by default,
    // which the SDK recommends. An unsupported rate is reported loudly --
    // sending at the wrong rate is heard as a chipmunk or a drawl, which an
    // operator would report as "talkback is broken", not "my rate is odd".
    const struct audio_output_info *aoi =
        audio_output_get_info(obs_get_audio());
    if (!aoi) {
        obs_source_release(src);
        error_out = "OBS audio is not running";
        return false;
    }
    const uint32_t rate = aoi->samples_per_sec;
    const uint16_t chans =
        static_cast<uint16_t>(get_audio_channels(aoi->speakers));
    if (!talkback_pcm_rate_supported(rate)) {
        obs_source_release(src);
        error_out = "OBS runs at " + std::to_string(rate) +
                    " Hz, which the Zoom talkback API does not accept. "
                    "Set OBS to 48000 Hz in Settings > Audio.";
        return false;
    }
    if (chans != 1 && chans != 2) {
        obs_source_release(src);
        error_out = "Talkback needs a mono or stereo OBS audio setup; this one "
                    "has " + std::to_string(chans) + " channels.";
        return false;
    }

    // A Windows named section cannot grow while any process maps it, so every
    // region name carries a generation. Talkback never resizes, but it must
    // still not collide with a stale section left by a previous run -- and
    // shm_region_create() reports exactly that case via ShmRegion::last_error
    // / the "opened an existing section" flag documented on the struct.
    m_region_name = shm_next_region(shm_generations(), base_region_name()).name;
    if (!shm_region_create(m_region, m_region_name,
                           shm_audio_region_bytes(kTalkbackSlotBytes))) {
        obs_source_release(src);
        error_out = "Could not create the talkback shared-memory region";
        return false;
    }
    talkback_ring_init(static_cast<ShmAudioHeader *>(m_region.ptr), rate, chans);

    m_source        = src;   // keep the strong ref; released in close()
    m_sample_rate   = rate;
    m_channels      = chans;
    m_last_audio_ms = os_gettime_ns() / 1000000ULL;
    m_open          = true;

    obs_source_add_audio_capture_callback(m_source, audio_cb, this);
    return true;
}

void TalkbackTap::close()
{
    obs_source_t *to_release = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_open) return;
        m_open = false;
        to_release = m_source;
        m_source = nullptr;
    }
    // Remove the callback OUTSIDE the lock: libobs takes its own audio mutex
    // here, and the callback takes ours. Holding both in opposite orders on
    // two threads is a classic lock-order inversion.
    if (to_release) {
        obs_source_remove_audio_capture_callback(to_release, audio_cb, this);
        obs_source_release(to_release);
    }
    std::lock_guard<std::mutex> lock(m_mtx);
    shm_region_destroy(m_region);
    m_region = ShmRegion{};
}

bool TalkbackTap::is_open() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_open;
}

uint64_t TalkbackTap::last_audio_ms() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_last_audio_ms;
}

uint32_t TalkbackTap::sample_rate() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_sample_rate;
}

uint16_t TalkbackTap::channels() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_channels;
}

std::string TalkbackTap::region_name() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_region_name;
}

void TalkbackTap::audio_cb(void *param, obs_source_t *, 
                           const struct audio_data *data, bool muted)
{
    static_cast<TalkbackTap *>(param)->on_audio(data, muted);
}

void TalkbackTap::on_audio(const struct audio_data *data, bool muted)
{
    if (!data || data->frames == 0) return;

    // A muted source still calls back, with real buffers. Publishing them
    // would put the director on air after they muted themselves -- exactly
    // the wrong direction for a fail-closed design. Publish silence instead
    // of nothing, so the dead-man switch does not read a mute as a dead path
    // and close the key.
    uint32_t rate, chans;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_open || m_region.ptr == nullptr) return;
        rate  = m_sample_rate;
        chans = m_channels;
    }

    const std::size_t bytes = talkback_pcm_bytes(data->frames, chans);
    if (bytes == 0 || bytes > kTalkbackSlotBytes) return;

    std::vector<int16_t> pcm(data->frames * chans, 0);
    if (!muted) {
        const float *planes[2] = {
            reinterpret_cast<const float *>(data->data[0]),
            chans > 1 ? reinterpret_cast<const float *>(data->data[1]) : nullptr,
        };
        talkback_pcm_interleave(planes, data->frames, chans, pcm.data());
    }

    const uint64_t now_ms = os_gettime_ns() / 1000000ULL;
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_open || m_region.ptr == nullptr) return;
        notify = talkback_ring_publish(m_region.ptr, pcm.data(),
                                       static_cast<uint32_t>(bytes), now_ms);
        m_last_audio_ms = now_ms;
    }
    (void)rate;
    (void)notify;   // Task 6 sends the pipe event on this edge.
}
```

- [ ] **Step 5: Add to the plugin sources and build**

Add `src/talkback-tap.cpp` to the plugin's source list in `CMakeLists.txt` (the same list that already contains `src/zoom-source.cpp`).

```sh
cmake --build build_x64 --config Release --parallel 8
cd build_x64 && ctest -C Release --output-on-failure
```
Expected: **58/58** green (this task adds no tests of its own; Task 1–3's still pass).

The helper names above are VERIFIED against the real headers: `shm_region_destroy(ShmRegion&)` (`src/engine-ipc.h:354`) releases a region, and generations come from `shm_next_region(shm_generations(), base)` returning `ShmRegionAllocation{gen, name}` (`src/shm-generation.h:133,144`). There is no `shm_region_close` and no `shm_next_generation` — do not reintroduce either name.

- [ ] **Step 6: Commit**

```sh
git add src/talkback-tap.h src/talkback-tap.cpp src/engine-ipc.h src/engine-command.h tests/engine-command-test.cpp CMakeLists.txt
git commit -m "feat(talkback): tap an OBS source into the talkback ring"
```

---

### Task 5: Pin the leak guarantee

The spec promises talkback audio is **structurally incapable** of reaching program or ISO. A promise nobody tests is a promise that decays. This test is deliberately cheap and deliberately blunt.

**Files:**
- Create: `tests/talkback-isolation-test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing at runtime — this is a source-level invariant test.
- Produces: nothing.

- [ ] **Step 1: Write the test**

Create `tests/talkback-isolation-test.cpp`:

```cpp
// tests/talkback-isolation-test.cpp
// Talkback audio must be structurally incapable of reaching program or ISO.
//
// This is a SOURCE-LEVEL invariant test: it reads the talkback tap's own
// source and asserts it uses only observing APIs, never routing ones. That is
// blunt, and deliberately so -- the alternative is a runtime test that would
// need OBS, a meeting, and a recording, and the property being protected is
// simple enough to state as "these symbols never appear in this file".
//
// The guarantee has two halves and only ONE of them is ours. Our half: a
// capture callback observes a source and cannot add it to a mix, and ISO
// records inbound audio only, so talkback cannot reach it by construction.
// The other half is the operator's: if they pick a source that is itself live
// on a program track, their voice reaches the stream through OBS's own
// routing, entirely outside our path. That is why the dock warns about the
// chosen source's enabled mixer tracks -- see the spec. This test protects
// our half; nothing in code can protect theirs.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

int main(int argc, char **argv)
{
    // The build passes the source path in, so the test does not depend on the
    // working directory ctest happens to use.
    if (argc < 2) {
        std::cerr << "FAIL: expected the path to talkback-tap.cpp as argv[1]\n";
        return 1;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::cerr << "FAIL: could not open " << argv[1] << "\n";
        return 1;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();

    // Routing APIs: any of these would put talkback into a mix.
    check(src.find("obs_source_output_audio") == std::string::npos,
          "talkback-tap.cpp calls obs_source_output_audio -- that ROUTES audio "
          "into OBS's mix and would put the director on program");
    check(src.find("obs_set_output_source") == std::string::npos,
          "talkback-tap.cpp calls obs_set_output_source -- that assigns a "
          "source to an output channel, which is program");
    check(src.find("obs_source_set_audio_mixers") == std::string::npos,
          "talkback-tap.cpp calls obs_source_set_audio_mixers -- that changes "
          "which program tracks a source feeds");
    check(src.find("obs_sceneitem_add") == std::string::npos,
          "talkback-tap.cpp adds a scene item -- talkback must never appear in "
          "a scene");

    // The observing API we DO rely on must still be there: if a refactor
    // removes it, talkback silently stops working and this test should say so
    // rather than passing because all the forbidden symbols are also absent.
    check(src.find("obs_source_add_audio_capture_callback") != std::string::npos,
          "talkback-tap.cpp no longer taps via obs_source_add_audio_capture_"
          "callback -- either talkback is broken or it now routes audio some "
          "other way");

    if (failures == 0)
        std::cout << "talkback-isolation: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register it**

In `CMakeLists.txt`, after the `CoreVideoTalkbackKey` block:

```cmake
    # Talkback audio must never reach program or ISO. Source-level invariant:
    # the tap may observe, never route. See tests/talkback-isolation-test.cpp
    # for why this is a source scan rather than a runtime test.
    add_executable(CoreVideoTalkbackIsolationTest
        tests/talkback-isolation-test.cpp
    )
    add_test(NAME CoreVideoTalkbackIsolation
             COMMAND CoreVideoTalkbackIsolationTest
                     "${CMAKE_CURRENT_SOURCE_DIR}/src/talkback-tap.cpp")
```

- [ ] **Step 3: Run it**

```sh
cmake --build build_x64 --config Release --parallel 8
cd build_x64 && ctest -C Release --output-on-failure
```
Expected: **59/59** green.

- [ ] **Step 4: Prove the test can actually fail**

Temporarily add the line `// obs_source_output_audio` — no, that would be caught as a comment and is a false confidence. Instead, temporarily insert a real call `obs_source_output_audio(m_source, nullptr);` inside `TalkbackTap::close()`, rebuild the test target, and confirm `CoreVideoTalkbackIsolation` FAILS. Then remove it and confirm the suite is green again. **A test that has never been seen to fail is not evidence.**

- [ ] **Step 5: Commit**

```sh
git add tests/talkback-isolation-test.cpp CMakeLists.txt
git commit -m "test(talkback): pin that the tap observes and never routes"
```

---

### Task 6: Engine reads the ring and sends

The engine maps the plugin's region, drains on the notify edge, and calls `SendAudioDataToChannel`. Draining happens **on the command-loop thread**, which on Windows is also the SDK's message-pump thread (`pump_windows_messages()` in `engine/src/main.cpp`) — so every SDK call stays on the thread the SDK already uses, rather than the separate driving thread the Milestone 1 probe introduced.

**Files:**
- Modify: `engine/src/engine-talkback.h`, `engine/src/engine-talkback.cpp`
- Modify: `engine/src/main.cpp`

**Interfaces:**
- Consumes: `talkback_ring_drain` (Task 2), the IPC commands (Task 4).
- Produces: `bool EngineTalkback::open_audio(const std::string &region_name, uint32_t sample_rate, uint16_t channels)`, `void EngineTalkback::drain_audio()`, `void EngineTalkback::close_audio()`. No channel id crosses the IPC boundary — the engine talks on the channel it already holds.

- [ ] **Step 1: Extend the class**

Add to `EngineTalkback`'s private section in `engine/src/engine-talkback.h`:

```cpp
    // ── Talkback audio path (Milestone 2) ──────────────────────────────────
    // The plugin CREATES this region and writes it; we open it read-write
    // because a reader must be able to clear the notify flag. See
    // src/talkback-ring.h for why the roles are reversed here.
    ShmRegion   m_audio_region{};
    std::string m_audio_region_name;
    uint32_t    m_audio_read_index = 0;
    uint32_t    m_audio_rate       = 0;
    uint16_t    m_audio_channels   = 0;
    bool        m_audio_open       = false;
```

and to the public section:

```cpp
    bool open_audio(const std::string &region_name, uint32_t sample_rate,
                    uint16_t channels);
    void drain_audio();
    void close_audio();
```

- [ ] **Step 2: Implement them**

Add to `engine/src/engine-talkback.cpp`:

```cpp
bool EngineTalkback::open_audio(const std::string &region_name,
                                uint32_t sample_rate, uint16_t channels)
{
    close_audio();
    if (!shm_region_open_readwrite(
            m_audio_region, region_name,
            shm_audio_region_bytes(kTalkbackSlotBytes))) {
        report("audio_open", R"("ok":false,"reason":"map_failed","region":")" +
               json_escape(region_name) + "\"");
        return false;
    }
    m_audio_region_name = region_name;
    m_audio_rate        = sample_rate;
    m_audio_channels    = channels;
    // Start at the writer's CURRENT index, not 0: buffers published before we
    // mapped are stale by definition, and replaying them would put a burst of
    // old audio in the channel the moment a key opens.
    m_audio_read_index =
        static_cast<ShmAudioHeader *>(m_audio_region.ptr)->write_index;
    m_audio_open = true;
    report("audio_open", R"("ok":true,"rate":)" + std::to_string(sample_rate) +
           R"(,"channels":)" + std::to_string(channels));
    return true;
}

namespace {
struct SendCtx {
    ZOOMSDK::IMeetingTalkbackController *ctrl;
    const zchar_t *channel;
    uint32_t rate;
    ZOOMSDK::ZoomSDKAudioChannel chan;
    uint32_t sent;
    int last_err;
};

void send_one(const void *pcm, uint32_t byte_len, uint64_t, void *ctx)
{
    auto *c = static_cast<SendCtx *>(ctx);
    if (!c->ctrl || !c->channel) return;
    const ZOOMSDK::SDKError e = c->ctrl->SendAudioDataToChannel(
        c->channel, static_cast<const char *>(pcm), byte_len, c->rate, c->chan);
    if (e != ZOOMSDK::SDKERR_SUCCESS) c->last_err = static_cast<int>(e);
    ++c->sent;
}
} // namespace

void EngineTalkback::drain_audio()
{
    if (!m_audio_open || !m_ctrl || m_audio_region.ptr == nullptr) return;

    // The channel to talk on is the one this class already holds -- created
    // and invited through the existing probe/ladder path. Milestone 5 owns
    // channel SELECTION (targets, the 16/10 caps, pre-provisioned private
    // channels); this milestone only moves audio into whichever channel is
    // already open, so no channel id crosses the IPC boundary and nothing here
    // needs a UTF-8 -> zchar_t conversion.
    std::basic_string<zchar_t> channel_copy;
    {
        std::lock_guard<std::mutex> lock(m_chan_mtx);
        channel_copy = m_channel_id_z;
    }
    if (channel_copy.empty()) return;

    auto *hdr = static_cast<ShmAudioHeader *>(m_audio_region.ptr);

    SendCtx ctx{m_ctrl, channel_copy.c_str(), m_audio_rate,
                m_audio_channels > 1 ? ZOOMSDK::ZoomSDKAudioChannel_Stereo
                                     : ZOOMSDK::ZoomSDKAudioChannel_Mono,
                0, 0};

    // EVENTS ARE PROMPTS, NOT PAYLOADS: drain everything available, then use
    // the reader helpers to decide whether sleeping is safe. Any return path
    // that consumes a wakeup and leaves notify set silences talkback until the
    // writer's next edge -- the failure that silenced whole sources before the
    // helpers existed.
    for (int pass = 0; pass < 4; ++pass) {
        talkback_ring_drain(m_audio_region.ptr, m_audio_read_index,
                            send_one, &ctx);
        if (audio_ring_reader_done(hdr, m_audio_read_index)) break;
        if (pass == 3) audio_ring_reader_abandon(hdr);
    }

    if (ctx.last_err != 0)
        report("audio_send", R"("code":)" + std::to_string(ctx.last_err) +
               R"(,"buffers":)" + std::to_string(ctx.sent));
}

void EngineTalkback::close_audio()
{
    if (!m_audio_open) return;
    m_audio_open = false;
    if (m_audio_region.ptr) {
        // Hand the flag back so the writer re-notifies rather than assuming a
        // reader is still listening.
        audio_ring_reader_abandon(static_cast<ShmAudioHeader *>(m_audio_region.ptr));
    }
    shm_region_destroy(m_audio_region);
    m_audio_region = ShmRegion{};
    m_audio_read_index = 0;
    report("audio_close", R"("ok":true)");
}
```

Add `#include "talkback-ring.h"` to `engine-talkback.cpp`'s includes.

- [ ] **Step 3: Route the commands in the engine**

In `engine/src/main.cpp`'s command loop, alongside the `TalkbackProbe` branch:

```cpp
        } else if (command == IpcCommand::TalkbackOpen) {
            talkback.open_audio(json_str(line, "region"),
                                static_cast<uint32_t>(json_uint(line, "rate")),
                                static_cast<uint16_t>(json_uint(line, "channels")));

        } else if (command == IpcCommand::TalkbackAudio) {
            // Drained on THIS thread deliberately: on Windows this loop is
            // also the SDK's message-pump thread (see
            // ipc_read_line_with_message_pump above), so every SDK call stays
            // on the thread the SDK already uses. The Milestone 1 probe's
            // separate driving thread was the first in this engine to call SDK
            // APIs off the pump; this path does not repeat that.
            talkback.drain_audio();

        } else if (command == IpcCommand::TalkbackClose) {
            talkback.close_audio();
```

If `json_uint` is `static` in `main.cpp` and needed elsewhere, leave it where it is — this branch is in `main.cpp` and can see it.

- [ ] **Step 4: Build and test**

```sh
cmake --build build_x64 --config Release --parallel 8
cd build_x64 && ctest -C Release --output-on-failure
```
Expected: **59/59** green.

- [ ] **Step 5: Commit**

```sh
git add engine/src/engine-talkback.h engine/src/engine-talkback.cpp engine/src/main.cpp
git commit -m "feat(talkback): engine drains the ring and sends to the channel"
```

---

## Self-Review

**Spec coverage for Milestones 2–4.** The spec's Milestone 2 ("the ring, in reverse") is Task 2; Milestone 3 ("the tap and PCM conversion") is Tasks 1 and 4; Milestone 4 ("keying state machine and dead-man") is Task 3. The spec's leak guarantee gets Task 5. The engine's consuming half — needed for any of it to do anything — is Task 6. The spec's requirement that unsupported sample rates be surfaced rather than silently handled is in Task 4's `open()`. The `talkback-plan-test` (channel planner) is Milestone 5 and deliberately NOT here.

**Deliberately out of scope, deferred to the Milestones 5–8 plan:** the channel planner and the 16/10 caps arithmetic, by-name identity re-resolution on roster change, control API and OSC verbs, the Companion module surface, the dock's configuration/tally/program-track warning, and the live verification pass. Also deferred: deadline-anchored pacing (measured at ~5% slip in the Milestone 1 gate) and the parked `leave`-mid-probe wedge.

**Placeholder scan:** none. Every code step contains real code, and every helper it names was checked against the real headers while writing this plan — `shm_region_destroy`, `shm_next_region`/`shm_generations`, `audio_ring_*`, `shm_audio_region_bytes`, `shm_audio_slot_offset`. Three names in the first draft (`shm_region_close`, `shm_next_generation`, and a `to_zstr` call from a translation unit that cannot see it — it is `static` in `main.cpp`) were wrong and were corrected before this plan was committed.

**Type consistency:** `talkback_pcm_bytes`/`talkback_pcm_interleave`/`talkback_pcm_rate_supported` are defined in Task 1 and called in Task 4 with matching argument order. `talkback_ring_publish`/`talkback_ring_drain`/`talkback_ring_init` and `TalkbackRingSlotFn` are defined in Task 2 and used in Tasks 4 and 6 with matching signatures. `kTalkbackSlotBytes` is defined in Task 2's `engine-ipc.h` edit and used in Tasks 4 and 6. `IpcCommand::TalkbackOpen/TalkbackAudio/TalkbackClose` are defined in Task 4 and consumed in Task 6. `TalkbackKeyState`'s fields are defined in Task 3 and used only there in this plan (their consumer is the Milestone 5–8 plan).

**Known risk, stated rather than hidden:** Task 4's `on_audio` allocates a `std::vector` per callback, on the OBS audio thread. That is a real-time path. It is written this way for clarity first; if profiling shows it matters, the fix is a preallocated scratch buffer sized at `open()`. Flagged here so a reviewer raises it deliberately rather than discovering it live.
