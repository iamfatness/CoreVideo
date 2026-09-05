# Loudness Engine + Readiness-Board Meter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure ITU-R BS.1770-4 loudness per Zoom panelist inside the OBS plugin, and render an operator-facing readiness board showing each panelist's deviation in LU from the panel median.

**Architecture:** A pure, header-only DSP core (`src/audio-loudness.h`) derives its K-weighting biquad coefficients from the **runtime** sample rate read out of the SHM ring header, and is fed 16-bit interleaved PCM from inside the existing drain loop in `src/zoom-participant-audio-source.cpp` — on the audio lane thread, integrating across the whole drain rather than per wakeup. A second pure header (`src/loudness-board.h`) turns the per-source readings into a board model (panel median, per-panelist deviation, pass/fail, row geometry). A new OBS rendering source, `corevideo_loudness_meter_source`, draws that model with the existing `Solid` technique from `data/effects/corevideo-tiles.effect` plus private child text sources for the labels.

**Tech Stack:** C++17, libobs (`obs_source_info`, `graphics/graphics.h`), CMake + CTest, no test framework, no new third-party dependency.

**Spec:** `docs/superpowers/specs/2026-09-05-panelist-feedback-design.md` — this plan implements **Subsystem 1 only** (§"Subsystem 1 — Loudness engine + meter source", plus the "Audio" and "Testing conventions" entries in §"Integration facts"). The face detector (Subsystem 2), Tiles auto-framing (3a) and the return overlay (3b) are **out of scope** and belong to other plans.

## Global Constraints

- **Sample rate is a runtime variable.** BS.1770-4 publishes biquad coefficients only for 48 kHz. Zoom commonly delivers 32 kHz. Read `ShmAudioHeader::sample_rate` per buffer and **derive** coefficients for that rate. Hardcoding the published 48 kHz constants is the single most likely way to ship a meter that reads plausibly and is wrong.
- **Input format is 16-bit signed, interleaved.** Channel count is likewise runtime-discovered from `ShmAudioHeader::channels`.
- **All three measures are required:** momentary (400 ms, ungated), short-term (3 s, ungated), integrated (400 ms blocks at 100 ms hop — 75% overlap — with the absolute −70 LUFS gate **and** the relative −10 LU gate). The gate is load-bearing: a panelist is silent roughly 80% of a panel.
- **Integrated loudness is scoped to a resettable per-panelist check window** (a 20–60 s mic check), not the whole session.
- **The headline number is deviation in LU from the panel MEDIAN** of gated integrated loudness. Median, never mean. Absolute LUFS is secondary. Reference presets: EBU R128 −23 LUFS, ATSC A/85 −24 LKFS, streaming −16 LUFS; **default is panel median**.
- **Pure logic lives in a header-only file under `src/`** with a "why this exists" comment, pinned by a test in `tests/`. No libobs, Qt or Zoom SDK includes in those headers.
- **No test framework.** Plain `int main()` with a local `static void check(bool, const char *)` that increments a file-scope `failures` counter and prints `FAIL: <message>`. Never gtest, never Catch.
- **Tests are hand-registered inside `if(BUILD_TESTING)` in the root `CMakeLists.txt`.** There is no `tests/CMakeLists.txt`. Target name `CoreVideo<Thing>Test`, ctest name `CoreVideo<Thing>`.
- **Metering runs on the audio lane thread only** — never the IPC reader thread (it head-of-line-blocks every source), never the OBS audio-mixer thread (budget-critical). Media events are coalescing prompts, not payloads: integrate over the whole drain loop.
- **`ZoomEngineClient::roster()` deep-copies strings under a hot mutex.** Cache display names on the roster callback (`add_roster_callback`); never call `roster()` from the audio path.
- Lock order in `src/zoom-participant-audio-source.cpp`: **`g_sources_mtx` before any `ctx->mtx`, never the reverse.**
- Build/verify: `cmake --build build --config Release --parallel 8`, then `cd build && ctest -C Release --output-on-failure`, N/N green.
- Comment style: state the constraint the code cannot show. When a decision comes from a measured number, put the number in the comment.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/audio-loudness.h` **(new)** | Pure BS.1770-4 DSP: runtime coefficient derivation, K-weighting biquads, 100 ms hop accumulator, momentary/short-term, gated integration over a resettable check window. No libobs/Qt/SDK. |
| `tests/audio-loudness-test.cpp` **(new)** | Pins the DSP against the published 48 kHz coefficient table, against reference tones with concrete expected LUFS values, and pins the gate and the chunk-invariance law. |
| `src/loudness-board.h` **(new)** | Pure board model: panel median, deviation, reference presets, pass/fail status, deterministic row order, change signature, and the row/bar rectangle arithmetic. No libobs/Qt/SDK. |
| `tests/loudness-board-test.cpp` **(new)** | Pins median-not-mean, deviation sign, status precedence, signature stability under reorder, and the layout arithmetic. |
| `src/zoom-participant-audio-source.h` **(modify)** | Declares the loudness readout and the check-window reset entry points. |
| `src/zoom-participant-audio-source.cpp` **(modify)** | Holds a `LoudnessMeter` per source, feeds it inside the drain loop, caches the display name on the roster callback, resets the window on (re)subscribe, and exposes readings through the existing `g_sources_mtx` registry. |
| `src/zoom-loudness-meter-source.h/.cpp` **(new)** | The `corevideo_loudness_meter_source` OBS rendering source: builds the board model at 10 Hz, draws bars with the `Solid` technique, labels with private child text sources. |
| `CMakeLists.txt` **(modify)** | Adds the new plugin source file and two test registrations. |
| `src/plugin-main.cpp` **(modify)** | Registers the meter source and its graphics load/unload hooks. |
| `data/locale/en-US.ini` **(modify)** | Strings for the meter source's name and properties. |

---

### Task 1: Runtime-derived BS.1770-4 K-weighting coefficients

**Files:**
- Create: `src/audio-loudness.h`
- Create: `tests/audio-loudness-test.cpp`
- Modify: `CMakeLists.txt` (inside the `if(BUILD_TESTING)` block that opens at line ~597)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct LoudnessBiquadCoeffs { double b0, b1, b2, a1, a2; };`
  - `LoudnessBiquadCoeffs bs1770_stage1_coeffs(uint32_t sample_rate);`
  - `LoudnessBiquadCoeffs bs1770_stage2_coeffs(uint32_t sample_rate);`
  - `struct LoudnessBiquadState { double x1, x2, y1, y2; };`
  - `double loudness_biquad_step(const LoudnessBiquadCoeffs &, LoudnessBiquadState &, double x);`

- [ ] **Step 1: Write the failing test**

Create `tests/audio-loudness-test.cpp`:

```cpp
// tests/audio-loudness-test.cpp
// ITU-R BS.1770-4 loudness, measured at whatever rate Zoom actually sends.
//
// WHY THIS TEST IS THE WHOLE FEATURE. BS.1770-4 publishes its K-weighting
// biquad coefficients for 48 kHz and for no other rate. This plugin does not
// receive a guaranteed rate: the engine reads GetSampleRate() per buffer and
// stamps it into ShmAudioHeader::sample_rate (engine/src/engine-audio.cpp),
// and Zoom commonly delivers 32 kHz. Coefficients pinned at 48 kHz and fed
// 32 kHz audio still produce a plausible-looking number -- measured below at
// 1.3 LU wrong on a 1 kHz tone -- which is precisely the failure a meter
// cannot survive, because nothing about the reading says it is wrong.
#include "audio-loudness.h"

#include <cmath>
#include <cstdint>
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

static bool near(double a, double b, double tol)
{
    return std::fabs(a - b) <= tol;
}

int main()
{
    // ── The published BS.1770-4 table, at 48 kHz, to the digit ─────────────
    // Table 1 (stage 1, the "head"/high-shelf pre-filter) and Table 2 (stage
    // 2, the RLB high-pass) of BS.1770-4. If the derivation is right, it
    // reproduces these exactly at 48 kHz -- that is the only rate at which
    // there is anything published to check against, which is why it is
    // checked to 1e-11 and not to a comfortable tolerance.
    {
        const LoudnessBiquadCoeffs s1 = bs1770_stage1_coeffs(48000);
        check(near(s1.b0,  1.53512485958697, 1e-11), "48k stage-1 b0 does not match the published BS.1770-4 table");
        check(near(s1.b1, -2.69169618940638, 1e-11), "48k stage-1 b1 does not match the published BS.1770-4 table");
        check(near(s1.b2,  1.19839281085285, 1e-11), "48k stage-1 b2 does not match the published BS.1770-4 table");
        check(near(s1.a1, -1.69065929318241, 1e-11), "48k stage-1 a1 does not match the published BS.1770-4 table");
        check(near(s1.a2,  0.73248077421585, 1e-11), "48k stage-1 a2 does not match the published BS.1770-4 table");

        const LoudnessBiquadCoeffs s2 = bs1770_stage2_coeffs(48000);
        check(near(s2.b0,  1.0, 1e-12), "48k stage-2 b0 must be exactly 1");
        check(near(s2.b1, -2.0, 1e-12), "48k stage-2 b1 must be exactly -2");
        check(near(s2.b2,  1.0, 1e-12), "48k stage-2 b2 must be exactly 1");
        check(near(s2.a1, -1.99004745483398, 1e-11), "48k stage-2 a1 does not match the published BS.1770-4 table");
        check(near(s2.a2,  0.99007225036621, 1e-11), "48k stage-2 a2 does not match the published BS.1770-4 table");
    }

    // ── 32 kHz must produce DIFFERENT, correctly derived coefficients ──────
    // These are the bilinear transform of the same analog prototype at
    // 32 kHz. A "derivation" that quietly returned the 48 kHz numbers for
    // every rate would pass every check above and fail every one here.
    {
        const LoudnessBiquadCoeffs s1 = bs1770_stage1_coeffs(32000);
        check(near(s1.b0,  1.51117789957, 1e-9), "32k stage-1 b0 is wrong");
        check(near(s1.b1, -2.46488941336, 1e-9), "32k stage-1 b1 is wrong");
        check(near(s1.b2,  1.04163327352, 1e-9), "32k stage-1 b2 is wrong");
        check(near(s1.a1, -1.53904509625, 1e-9), "32k stage-1 a1 is wrong");
        check(near(s1.a2,  0.62696685598, 1e-9), "32k stage-1 a2 is wrong");

        const LoudnessBiquadCoeffs s2 = bs1770_stage2_coeffs(32000);
        check(near(s2.a1, -1.98508966899, 1e-9), "32k stage-2 a1 is wrong");
        check(near(s2.a2,  0.98514532067, 1e-9), "32k stage-2 a2 is wrong");
    }

    // ── The two rates must not be the same numbers ─────────────────────────
    // Stated as its own assertion rather than left implicit in the two blocks
    // above, because "the coefficients are rate-dependent" is the invariant,
    // and an implementer reading only this file should see it said out loud.
    {
        const LoudnessBiquadCoeffs a = bs1770_stage1_coeffs(48000);
        const LoudnessBiquadCoeffs b = bs1770_stage1_coeffs(32000);
        check(std::fabs(a.a1 - b.a1) > 0.10,
              "stage-1 a1 barely moved between 48 kHz and 32 kHz -- the "
              "coefficients are not being derived from the rate at all");
        const LoudnessBiquadCoeffs c = bs1770_stage2_coeffs(48000);
        const LoudnessBiquadCoeffs d = bs1770_stage2_coeffs(32000);
        check(std::fabs(c.a1 - d.a1) > 0.004,
              "stage-2 a1 barely moved between 48 kHz and 32 kHz -- the "
              "high-pass corner is being placed at a fixed digital frequency "
              "rather than a fixed 38 Hz");
    }

    // ── A degenerate rate must not produce NaN or a divide by zero ─────────
    {
        const LoudnessBiquadCoeffs s1 = bs1770_stage1_coeffs(0);
        check(std::isfinite(s1.b0) && std::isfinite(s1.a1),
              "a zero sample rate produced non-finite coefficients -- the "
              "ring header can be read before the writer has initialised it");
    }

    // ── The biquad itself: a direct-form-II-transposed step ────────────────
    // Pinned against hand-computed values so a sign slip on the feedback
    // terms cannot hide inside a filter response test.
    {
        const LoudnessBiquadCoeffs c{0.5, 0.25, 0.125, -0.5, 0.25};
        LoudnessBiquadState st{};
        // y[0] = 0.5*1 = 0.5
        const double y0 = loudness_biquad_step(c, st, 1.0);
        check(near(y0, 0.5, 1e-12), "biquad sample 0 was not b0*x0");
        // y[1] = 0.5*0 + 0.25*1 + 0.125*0 - (-0.5)*0.5 - 0.25*0 = 0.5
        const double y1 = loudness_biquad_step(c, st, 0.0);
        check(near(y1, 0.5, 1e-12), "biquad sample 1 is wrong -- check the "
              "sign convention on a1 (y = b.x - a.y)");
        // y[2] = 0.125*1 - (-0.5)*0.5 - 0.25*0.5 = 0.125 + 0.25 - 0.125 = 0.25
        const double y2 = loudness_biquad_step(c, st, 0.0);
        check(near(y2, 0.25, 1e-12), "biquad sample 2 is wrong -- the second "
              "feedback tap (a2) is not being applied");
    }

    if (failures == 0)
        std::cout << "audio-loudness: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register the test in CMake**

In `CMakeLists.txt`, inside the `if(BUILD_TESTING)` block, immediately after the `CoreVideoAudioSilenceFadeTest` registration, add:

```cmake
    # ITU-R BS.1770-4 loudness. The coefficients are DERIVED from the runtime
    # sample rate rather than taken from the standard's 48 kHz table, because
    # the engine stamps whatever rate Zoom gave it into ShmAudioHeader and
    # Zoom commonly sends 32 kHz -- measured 1.3 LU of error on a 1 kHz tone
    # if the 48 kHz constants are used at 32 kHz, with nothing in the reading
    # to say it is wrong. See src/audio-loudness.h.
    add_executable(CoreVideoAudioLoudnessTest
        tests/audio-loudness-test.cpp
    )
    target_include_directories(CoreVideoAudioLoudnessTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoAudioLoudness
             COMMAND CoreVideoAudioLoudnessTest)
```

- [ ] **Step 3: Run the test to verify it fails**

```sh
cmake --build build --config Release --parallel 8
```

Expected: FAIL at compile time — `Cannot open include file: 'audio-loudness.h'`.

- [ ] **Step 4: Write the minimal implementation**

Create `src/audio-loudness.h`:

```cpp
#pragma once

// ITU-R BS.1770-4 loudness measurement, derived for the sample rate the audio
// ACTUALLY arrives at.
//
// WHY THIS FILE DERIVES INSTEAD OF QUOTING. BS.1770-4 tabulates its two
// K-weighting biquads' coefficients for 48 kHz and for no other rate. This
// plugin has no guaranteed rate: engine/src/engine-audio.cpp calls
// data->GetSampleRate() per buffer and stamps the answer into
// ShmAudioHeader::sample_rate, and Zoom commonly delivers 32 kHz. Applying
// the published 48 kHz numbers to 32 kHz audio moves both filters' corner
// frequencies by a factor of 1.5 and mis-weights every measurement: on a
// 1 kHz tone whose true value is -19.98 LUFS it reads -18.66 LUFS. That is
// 1.3 LU of error on a meter whose entire product claim is that a 6 LU
// spread between panelists is visible -- and nothing about the number looks
// wrong. So the coefficients come from the analog prototype in the standard,
// bilinear-transformed at the runtime rate. At 48 kHz the derivation
// reproduces the published table to fourteen digits, which is what
// tests/audio-loudness-test.cpp asserts.
//
// Pure by design -- no libobs, no Qt, no Zoom SDK -- so the whole measurement
// can be pinned against reference tones with no meeting, the same treatment
// audio-timeline.h and audio-silence-fade.h get, and for the same reason:
// the only symptom of a regression here is a number that is quietly wrong.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// One biquad section, y[n] = b0.x[n] + b1.x[n-1] + b2.x[n-2]
//                           - a1.y[n-1] - a2.y[n-2]
// (a0 normalised to 1). Sign convention matches the standard's tables, so a
// published a1 of -1.69065929318241 is stored verbatim.
struct LoudnessBiquadCoeffs {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

struct LoudnessBiquadState {
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;
};

inline double loudness_biquad_step(const LoudnessBiquadCoeffs &c,
                                   LoudnessBiquadState &s, double x)
{
    const double y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2
                     - c.a1 * s.y1 - c.a2 * s.y2;
    s.x2 = s.x1;
    s.x1 = x;
    s.y2 = s.y1;
    s.y1 = y;
    return y;
}

// The analog prototype BS.1770-4's 48 kHz table was itself produced from.
// These five constants are the whole of the standard's filter specification
// once the rate is factored out; every published coefficient falls out of
// them. Kept at full precision because the 48 kHz reproduction is asserted to
// 1e-11.
constexpr double kBs1770Stage1Hz   = 1681.974450955533;
constexpr double kBs1770Stage1GdB  = 3.999843853973347;
constexpr double kBs1770Stage1Q    = 0.7071752369554196;
constexpr double kBs1770Stage1VbExp = 0.4996667741545416;
constexpr double kBs1770Stage2Hz   = 38.13547087602444;
constexpr double kBs1770Stage2Q    = 0.5003270373238773;

// A rate to fall back on when the caller hands us nothing usable. The ring
// header can legitimately be read before the writer has initialised it (see
// output_audio_frame()'s slot_count guard), and a zero rate must produce
// finite coefficients rather than a NaN that then poisons every subsequent
// filter state for the life of the source.
constexpr uint32_t kLoudnessFallbackRate = 48000;

inline uint32_t loudness_usable_rate(uint32_t sample_rate)
{
    return (sample_rate >= 8000 && sample_rate <= 384000)
               ? sample_rate : kLoudnessFallbackRate;
}

// Stage 1: the "head" high-shelf, roughly +4 dB above 1 kHz.
inline LoudnessBiquadCoeffs bs1770_stage1_coeffs(uint32_t sample_rate)
{
    const double fs = static_cast<double>(loudness_usable_rate(sample_rate));
    const double K  = std::tan(3.14159265358979323846 * kBs1770Stage1Hz / fs);
    const double Vh = std::pow(10.0, kBs1770Stage1GdB / 20.0);
    const double Vb = std::pow(Vh, kBs1770Stage1VbExp);
    const double a0 = 1.0 + K / kBs1770Stage1Q + K * K;

    LoudnessBiquadCoeffs c;
    c.b0 = (Vh + Vb * K / kBs1770Stage1Q + K * K) / a0;
    c.b1 = 2.0 * (K * K - Vh) / a0;
    c.b2 = (Vh - Vb * K / kBs1770Stage1Q + K * K) / a0;
    c.a1 = 2.0 * (K * K - 1.0) / a0;
    c.a2 = (1.0 - K / kBs1770Stage1Q + K * K) / a0;
    return c;
}

// Stage 2: the RLB high-pass, roughly 38 Hz. b0/b1/b2 are exactly 1/-2/1 at
// every rate -- that is a property of the prototype, not a rounding of the
// published table, so they are written as literals.
inline LoudnessBiquadCoeffs bs1770_stage2_coeffs(uint32_t sample_rate)
{
    const double fs = static_cast<double>(loudness_usable_rate(sample_rate));
    const double K  = std::tan(3.14159265358979323846 * kBs1770Stage2Hz / fs);
    const double d  = 1.0 + K / kBs1770Stage2Q + K * K;

    LoudnessBiquadCoeffs c;
    c.b0 =  1.0;
    c.b1 = -2.0;
    c.b2 =  1.0;
    c.a1 = 2.0 * (K * K - 1.0) / d;
    c.a2 = (1.0 - K / kBs1770Stage2Q + K * K) / d;
    return c;
}
```

- [ ] **Step 5: Run the test to verify it passes**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release -R CoreVideoAudioLoudness --output-on-failure
```

Expected: PASS, `audio-loudness: all tests passed`.

- [ ] **Step 6: Commit**

```bash
git add src/audio-loudness.h tests/audio-loudness-test.cpp CMakeLists.txt
git commit -m "feat(loudness): derive BS.1770-4 K-weighting coefficients from the runtime sample rate"
```

---

### Task 2: K-weighted momentary and short-term loudness from int16 interleaved PCM

**Files:**
- Modify: `src/audio-loudness.h` (append after `bs1770_stage2_coeffs`)
- Modify: `tests/audio-loudness-test.cpp` (append new blocks before the final `if (failures == 0)`)

**Interfaces:**
- Consumes: `LoudnessBiquadCoeffs`, `LoudnessBiquadState`, `loudness_biquad_step`, `bs1770_stage1_coeffs`, `bs1770_stage2_coeffs` (Task 1).
- Produces:
  - `constexpr double kLoudnessOffsetDb = -0.691;`
  - `double loudness_lufs_from_mean_square(double z);`
  - `double loudness_channel_weight(uint16_t channels, uint16_t channel);`
  - `struct LoudnessMeter { ... };` (fields listed in the implementation below)
  - `void loudness_meter_configure(LoudnessMeter &, uint32_t sample_rate, uint16_t channels);`
  - `void loudness_meter_feed_int16(LoudnessMeter &, const int16_t *pcm, size_t frames, uint16_t channels, uint32_t sample_rate);`
  - `bool loudness_meter_momentary(const LoudnessMeter &, double *out_lufs);`
  - `bool loudness_meter_short_term(const LoudnessMeter &, double *out_lufs);`

- [ ] **Step 1: Write the failing test**

Append to `tests/audio-loudness-test.cpp`, immediately before the closing `if (failures == 0)`:

```cpp
    // ── Reference tones: the numbers an implementer can check by hand ──────
    //
    // The K-weighting curve has a gain of exactly +0.691 dB at 997 Hz, and
    // BS.1770's -0.691 dB offset is there to cancel it. So for a ~1 kHz sine
    // the whole measurement collapses to L = 10*log10(mean square of the
    // un-weighted signal), which is a number that can be worked out on paper:
    //
    //   peak 1.0        -> mean square 0.5    -> -3.01 LUFS
    //   peak 0.1        -> mean square 0.005  -> -23.01 LUFS
    //   RMS  0.1        -> mean square 0.01   -> -20.00 LUFS
    //
    // The third is the one to remember: a 1 kHz tone at -20 dBFS RMS reads
    // -20.0 LUFS. If that does not hold, the offset, the channel weight, the
    // int16 scaling or the K-weighting is wrong, and no amount of relative
    // comparison downstream will save the reading.
    auto feed_sine = [](LoudnessMeter &m, uint32_t rate, double peak,
                        double freq, double seconds) {
        const size_t n = static_cast<size_t>(rate * seconds);
        std::vector<int16_t> pcm(n);
        for (size_t i = 0; i < n; ++i) {
            const double v = peak * std::sin(2.0 * 3.14159265358979323846 *
                                             freq * static_cast<double>(i) /
                                             static_cast<double>(rate));
            double s = v * 32767.0;
            if (s > 32767.0)  s =  32767.0;
            if (s < -32767.0) s = -32767.0;
            pcm[i] = static_cast<int16_t>(std::lround(s));
        }
        loudness_meter_feed_int16(m, pcm.data(), n, 1, rate);
    };

    {
        LoudnessMeter m;
        feed_sine(m, 48000, 1.0, 1000.0, 5.0);
        double lufs = 0.0;
        check(loudness_meter_momentary(m, &lufs),
              "momentary loudness was unavailable after 5 s of tone");
        check(near(lufs, -3.01, 0.10),
              "a full-scale 1 kHz sine at 48 kHz did not read -3.01 LUFS");
    }
    {
        LoudnessMeter m;
        feed_sine(m, 48000, 0.1, 1000.0, 5.0);
        double lufs = 0.0;
        check(loudness_meter_momentary(m, &lufs), "momentary unavailable");
        check(near(lufs, -23.01, 0.10),
              "a 1 kHz sine of peak amplitude 0.1 at 48 kHz did not read "
              "-23.01 LUFS");
    }
    {
        // -20 dBFS RMS: peak = sqrt(2) * 0.1.
        LoudnessMeter m;
        feed_sine(m, 48000, std::sqrt(2.0) * 0.1, 1000.0, 5.0);
        double lufs = 0.0;
        check(loudness_meter_momentary(m, &lufs), "momentary unavailable");
        check(near(lufs, -20.00, 0.10),
              "a -20 dBFS RMS 1 kHz sine at 48 kHz did not read -20.0 LUFS -- "
              "K-weighting is ~0 dB at 1 kHz once the -0.691 offset is "
              "applied, so this is an equality, not an approximation");
    }

    // ── The same tone at 32 kHz must read the same, not 1.3 LU high ────────
    // This is the assertion the whole runtime-rate design exists for. With
    // the 48 kHz coefficients applied to 32 kHz audio this tone reads
    // -18.66 LUFS instead of -19.98: it passes a "looks like a plausible
    // loudness" eyeball test and fails here.
    {
        LoudnessMeter m;
        feed_sine(m, 32000, std::sqrt(2.0) * 0.1, 1000.0, 5.0);
        double lufs = 0.0;
        check(loudness_meter_momentary(m, &lufs), "momentary unavailable at 32 kHz");
        check(near(lufs, -19.98, 0.12),
              "a -20 dBFS RMS 1 kHz sine at 32 kHz did not read -20 LUFS -- "
              "the coefficients are not following the runtime rate");
        check(lufs < -19.5,
              "the 32 kHz reading is more than 0.5 LU hot, which is the "
              "signature of 48 kHz coefficients being used at 32 kHz");
    }

    // ── Short-term needs 3 s; momentary needs 400 ms ───────────────────────
    {
        LoudnessMeter m;
        feed_sine(m, 48000, std::sqrt(2.0) * 0.1, 1000.0, 0.35);
        double lufs = 0.0;
        check(!loudness_meter_momentary(m, &lufs),
              "momentary reported a value before a full 400 ms block existed");
        feed_sine(m, 48000, std::sqrt(2.0) * 0.1, 1000.0, 0.20);
        check(loudness_meter_momentary(m, &lufs),
              "momentary was still unavailable after 550 ms");
        check(!loudness_meter_short_term(m, &lufs),
              "short-term reported a value before 3 s of audio existed");
        feed_sine(m, 48000, std::sqrt(2.0) * 0.1, 1000.0, 3.0);
        check(loudness_meter_short_term(m, &lufs),
              "short-term was still unavailable after 3.5 s");
        check(near(lufs, -20.00, 0.15), "short-term did not read -20 LUFS");
    }

    // ── Stereo: two identical channels are +3 dB, not the same as mono ─────
    // BS.1770 sums the weighted per-channel mean squares (G = 1.0 for L and
    // R), it does not average them. Averaging is the mistake that makes a
    // stereo panelist read 3 LU quieter than the identical mono one beside
    // them, which is exactly the comparison this feature exists to make.
    {
        LoudnessMeter mono;
        feed_sine(mono, 48000, std::sqrt(2.0) * 0.1, 1000.0, 2.0);
        double mono_lufs = 0.0;
        check(loudness_meter_momentary(mono, &mono_lufs), "mono unavailable");

        LoudnessMeter st;
        const size_t n = 48000 * 2;
        std::vector<int16_t> pcm(n * 2);
        for (size_t i = 0; i < n; ++i) {
            const double v = std::sqrt(2.0) * 0.1 *
                std::sin(2.0 * 3.14159265358979323846 * 1000.0 *
                         static_cast<double>(i) / 48000.0);
            const int16_t s = static_cast<int16_t>(std::lround(v * 32767.0));
            pcm[i * 2]     = s;
            pcm[i * 2 + 1] = s;
        }
        loudness_meter_feed_int16(st, pcm.data(), n, 2, 48000);
        double st_lufs = 0.0;
        check(loudness_meter_momentary(st, &st_lufs), "stereo unavailable");
        check(near(st_lufs - mono_lufs, 3.01, 0.05),
              "dual-mono stereo was not +3.01 LU relative to mono -- the "
              "channels are being averaged instead of summed");
    }

    // ── Digital silence never produces NaN or -inf leaking to a caller ─────
    {
        LoudnessMeter m;
        std::vector<int16_t> zeros(48000, 0);
        loudness_meter_feed_int16(m, zeros.data(), zeros.size(), 1, 48000);
        double lufs = 0.0;
        const bool have = loudness_meter_momentary(m, &lufs);
        check(!have || std::isfinite(lufs),
              "true digital silence produced a non-finite momentary reading -- "
              "a panelist who has not spoken yet is the normal case here, not "
              "an edge case");
    }
```

- [ ] **Step 2: Run the test to verify it fails**

```sh
cmake --build build --config Release --parallel 8
```

Expected: FAIL at compile time — `'LoudnessMeter': undeclared identifier`.

- [ ] **Step 3: Write the implementation**

Append to `src/audio-loudness.h`:

```cpp
// The standard's absolute offset. It exists to cancel the K-weighting's
// +0.691 dB gain at 997 Hz, which is why a 1 kHz tone's LUFS value equals
// 10*log10 of its un-weighted mean square exactly.
constexpr double kLoudnessOffsetDb = -0.691;

// Block/hop geometry. 400 ms blocks advancing every 100 ms is 75% overlap,
// which is what BS.1770-4 specifies for gated integration; momentary IS one
// such block, and short-term is 30 hops.
constexpr uint32_t kLoudnessHopMs        = 100;
constexpr uint32_t kLoudnessMomentaryHops = 4;   // 400 ms
constexpr uint32_t kLoudnessShortTermHops = 30;  // 3 s

// L = -0.691 + 10*log10(sum of G_i * z_i). Returns -HUGE_VAL for a
// non-positive mean square rather than letting log10 produce -inf/NaN at an
// arbitrary call site; every caller in this header checks for it.
inline double loudness_lufs_from_mean_square(double z)
{
    if (!(z > 0.0)) return -HUGE_VAL;
    return kLoudnessOffsetDb + 10.0 * std::log10(z);
}

// BS.1770-4 channel weights, in the standard's channel order
// (L, R, C, LFE, Ls, Rs). Zoom participant audio is mono or stereo, so in
// practice only the G = 1.0 terms are ever reached -- but a source configured
// for more channels must not silently weight a surround channel as if it were
// a front one, and the LFE must not be counted at all.
inline double loudness_channel_weight(uint16_t channels, uint16_t channel)
{
    if (channels <= 2) return 1.0;
    switch (channel) {
    case 0: case 1: case 2: return 1.00;  // L, R, C
    case 3:                 return 0.00;  // LFE is excluded, not attenuated
    case 4: case 5:         return 1.41;  // Ls, Rs
    default:                return 0.00;
    }
}

// A running BS.1770-4 measurement for ONE participant.
//
// OWNERSHIP: not thread-safe and deliberately so. In the plugin exactly one
// thread -- the audio lane that owns output_audio_frame() -- feeds it, under
// the same ctx->mtx that already guards the source's timeline, and readers
// take that mutex to copy the three numbers out. Adding a lock in here would
// put one on the media path for no gain.
struct LoudnessMeter {
    uint32_t sample_rate = 0;
    uint16_t channels    = 0;

    LoudnessBiquadCoeffs c1{};
    LoudnessBiquadCoeffs c2{};
    std::vector<LoudnessBiquadState> s1;   // stage 1 state, one per channel
    std::vector<LoudnessBiquadState> s2;   // stage 2 state, one per channel

    // Current partial 100 ms hop.
    uint32_t hop_frames = 0;      // frames per hop at the configured rate
    uint32_t hop_filled = 0;
    double   hop_acc    = 0.0;    // sum over frames of sum_ch(G * y^2)

    // The last kLoudnessShortTermHops completed hops, newest at
    // (hop_total - 1) % kLoudnessShortTermHops.
    double   hop_ring[kLoudnessShortTermHops] = {};
    uint64_t hop_total = 0;
};

// (Re)configures for a rate/channel count and clears all filter state. Called
// automatically by loudness_meter_feed_int16() whenever the wire format
// changes -- which it can, mid-source: Zoom renegotiates, and the operator's
// Mix/Isolated role flip changes the channel count on the same subscription.
// Carrying filter history across that would smear one format's transient into
// the other's measurement.
inline void loudness_meter_configure(LoudnessMeter &m, uint32_t sample_rate,
                                     uint16_t channels)
{
    const uint32_t rate = loudness_usable_rate(sample_rate);
    m.sample_rate = rate;
    m.channels    = channels == 0 ? 1 : channels;
    m.c1 = bs1770_stage1_coeffs(rate);
    m.c2 = bs1770_stage2_coeffs(rate);
    m.s1.assign(m.channels, LoudnessBiquadState{});
    m.s2.assign(m.channels, LoudnessBiquadState{});
    m.hop_frames = (rate * kLoudnessHopMs) / 1000;
    if (m.hop_frames == 0) m.hop_frames = 1;
    m.hop_filled = 0;
    m.hop_acc    = 0.0;
    for (uint32_t i = 0; i < kLoudnessShortTermHops; ++i) m.hop_ring[i] = 0.0;
    m.hop_total = 0;
}

// Hook the gated integrator into the hop boundary. Defined in Task 3; the
// forward declaration keeps feed_int16 below unchanged when it lands.
inline void loudness_meter_on_hop_complete(LoudnessMeter &m);

// Feeds interleaved 16-bit signed PCM -- the format the engine writes into
// the SHM ring, unconverted.
//
// SCALING: /32768.0, not /32767.0. int16 is asymmetric and full negative
// scale is -32768; dividing by 32767 would let a legitimate sample exceed
// -1.0 and is the wrong direction for a measurement.
//
// PARTIAL BUFFERS ARE THE NORMAL CASE. Zoom delivers ~10 ms buffers and one
// media event can carry eight of them, so a 100 ms hop is assembled from many
// calls. The hop boundary is decided by frame count alone and never by call
// boundaries, which is what makes "feed the whole drain loop" identical to
// "feed one big buffer" -- pinned as chunk invariance in the test.
inline void loudness_meter_feed_int16(LoudnessMeter &m, const int16_t *pcm,
                                      size_t frames, uint16_t channels,
                                      uint32_t sample_rate)
{
    if (pcm == nullptr || frames == 0 || channels == 0) return;
    if (m.sample_rate != loudness_usable_rate(sample_rate) ||
        m.channels != channels) {
        loudness_meter_configure(m, sample_rate, channels);
    }

    for (size_t f = 0; f < frames; ++f) {
        double frame_sum = 0.0;
        for (uint16_t ch = 0; ch < channels; ++ch) {
            const double g = loudness_channel_weight(channels, ch);
            const double x = static_cast<double>(pcm[f * channels + ch]) /
                             32768.0;
            const double y1 = loudness_biquad_step(m.c1, m.s1[ch], x);
            const double y2 = loudness_biquad_step(m.c2, m.s2[ch], y1);
            // The filters run even for a zero-weight channel: their state is
            // per channel and skipping them would make the LFE's history
            // depend on how long it had been zero-weighted.
            frame_sum += g * y2 * y2;
        }
        m.hop_acc += frame_sum;
        if (++m.hop_filled >= m.hop_frames) {
            const double hop_mean = m.hop_acc /
                                    static_cast<double>(m.hop_frames);
            m.hop_ring[m.hop_total % kLoudnessShortTermHops] = hop_mean;
            ++m.hop_total;
            m.hop_acc    = 0.0;
            m.hop_filled = 0;
            loudness_meter_on_hop_complete(m);
        }
    }
}

// Mean of the newest `n` completed hops. False when fewer than `n` exist --
// which is the honest answer for a panelist who has just been subscribed, and
// is why every getter here returns bool rather than a sentinel loudness.
inline bool loudness_hop_mean(const LoudnessMeter &m, uint32_t n, double *out)
{
    if (n == 0 || n > kLoudnessShortTermHops || m.hop_total < n) return false;
    double sum = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t idx = m.hop_total - 1 - i;
        sum += m.hop_ring[idx % kLoudnessShortTermHops];
    }
    *out = sum / static_cast<double>(n);
    return true;
}

// Momentary (M): one 400 ms block, ungated.
inline bool loudness_meter_momentary(const LoudnessMeter &m, double *out_lufs)
{
    double z = 0.0;
    if (!loudness_hop_mean(m, kLoudnessMomentaryHops, &z)) return false;
    const double l = loudness_lufs_from_mean_square(z);
    if (!std::isfinite(l)) return false;
    *out_lufs = l;
    return true;
}

// Short-term (S): 3 s, ungated. The number an operator reads while the
// panelist is talking.
inline bool loudness_meter_short_term(const LoudnessMeter &m, double *out_lufs)
{
    double z = 0.0;
    if (!loudness_hop_mean(m, kLoudnessShortTermHops, &z)) return false;
    const double l = loudness_lufs_from_mean_square(z);
    if (!std::isfinite(l)) return false;
    *out_lufs = l;
    return true;
}
```

Also add a temporary definition so the forward declaration links; Task 3 replaces its body. Append at the very end of the file for now:

```cpp
// Placeholder until Task 3 lands the gated integrator. Declared above so
// feed_int16 already calls it; defined empty here so this task builds alone.
inline void loudness_meter_on_hop_complete(LoudnessMeter &) {}
```

- [ ] **Step 4: Run the test to verify it passes**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release -R CoreVideoAudioLoudness --output-on-failure
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/audio-loudness.h tests/audio-loudness-test.cpp
git commit -m "feat(loudness): K-weighted momentary and short-term loudness from int16 interleaved PCM"
```

---

### Task 3: Gated integrated loudness over a resettable check window

**Files:**
- Modify: `src/audio-loudness.h`
- Modify: `tests/audio-loudness-test.cpp`

**Interfaces:**
- Consumes: `LoudnessMeter`, `loudness_lufs_from_mean_square`, `loudness_hop_mean` (Task 2).
- Produces:
  - `constexpr double kLoudnessAbsoluteGateLufs = -70.0;`
  - `constexpr double kLoudnessRelativeGateLu = -10.0;`
  - `constexpr size_t kLoudnessMaxGatedBlocks = 6000;`
  - `void loudness_meter_reset_window(LoudnessMeter &);`
  - `bool loudness_meter_integrated(const LoudnessMeter &, double *out_lufs);`
  - `uint64_t loudness_meter_gated_blocks(const LoudnessMeter &);`

- [ ] **Step 1: Write the failing test**

Append to `tests/audio-loudness-test.cpp`, before the closing `if (failures == 0)`:

```cpp
    // ── The gate is the reason this measure is usable at all ───────────────
    // A panelist is silent roughly 80% of a panel. 4 s of speech at
    // -20 LUFS followed by 16 s of silence averages to -27.08 LUFS if
    // ungated -- an answer that describes the meeting, not the microphone.
    // The BS.1770 absolute gate at -70 LUFS discards the silent blocks and
    // the answer comes back to -20.
    {
        LoudnessMeter m;
        feed_sine(m, 48000, std::sqrt(2.0) * 0.1, 1000.0, 4.0);
        std::vector<int16_t> zeros(48000 * 16, 0);
        loudness_meter_feed_int16(m, zeros.data(), zeros.size(), 1, 48000);

        double lufs = 0.0;
        check(loudness_meter_integrated(m, &lufs),
              "integrated loudness was unavailable after 4 s of speech");
        check(near(lufs, -20.16, 0.35),
              "4 s of -20 LUFS speech in 20 s of silence did not integrate to "
              "about -20 LUFS -- an ungated running average reads -27.08 here");
        check(lufs > -22.0,
              "the integrated reading is dragged down by silence: the "
              "absolute -70 LUFS gate is not being applied");
        check(loudness_meter_gated_blocks(m) > 30 &&
              loudness_meter_gated_blocks(m) < 60,
              "the gated block count is not ~40 -- 4 s of speech at a 100 ms "
              "hop is about 40 blocks that clear the absolute gate");
    }

    // ── The RELATIVE gate, which the absolute gate cannot stand in for ─────
    // 10 s at -20 LUFS then 10 s at -40 LUFS: every block clears -70, so the
    // absolute gate alone leaves -22.96. The relative gate (-10 LU below the
    // absolute-gated mean) drops the quiet half and the answer is -20.06 --
    // the loudness of the speech, which is what a mic check is asking about.
    {
        LoudnessMeter m;
        feed_sine(m, 48000, std::sqrt(2.0) * 0.1,  1000.0, 10.0);
        feed_sine(m, 48000, std::sqrt(2.0) * 0.01, 1000.0, 10.0);
        double lufs = 0.0;
        check(loudness_meter_integrated(m, &lufs), "integrated unavailable");
        check(near(lufs, -20.06, 0.30),
              "loud-then-quiet did not integrate to about -20 LUFS -- with "
              "only the absolute gate this reads -22.96");
    }

    // ── The check window is resettable, and a reset is a clean slate ───────
    // A mic check is per panelist. Without this the number is polluted by
    // whoever spoke before them on the same source.
    {
        LoudnessMeter m;
        feed_sine(m, 48000, 1.0, 1000.0, 3.0);            // very loud, -3 LUFS
        double before = 0.0;
        check(loudness_meter_integrated(m, &before), "integrated unavailable");
        check(near(before, -3.01, 0.30), "the loud pass did not read -3 LUFS");

        loudness_meter_reset_window(m);
        double after = 0.0;
        check(!loudness_meter_integrated(m, &after),
              "integrated loudness survived a window reset -- the previous "
              "panelist's check is still in the number");
        check(loudness_meter_gated_blocks(m) == 0,
              "the gated block count survived a window reset");

        feed_sine(m, 48000, std::sqrt(2.0) * 0.1, 1000.0, 3.0);
        check(loudness_meter_integrated(m, &after), "integrated unavailable "
              "after refilling the window");
        check(near(after, -20.00, 0.30),
              "the post-reset reading is contaminated by the pre-reset audio");
    }

    // ── A panelist who has never spoken has NO integrated reading ──────────
    // Not -70, not 0. The board must be able to say "no audio" rather than
    // print a number that looks like a measurement.
    {
        LoudnessMeter m;
        std::vector<int16_t> zeros(48000 * 5, 0);
        loudness_meter_feed_int16(m, zeros.data(), zeros.size(), 1, 48000);
        double lufs = 0.0;
        check(!loudness_meter_integrated(m, &lufs),
              "five seconds of pure silence produced an integrated loudness");
        check(loudness_meter_gated_blocks(m) == 0,
              "silent blocks were counted as gated blocks");
    }

    // ── Chunk invariance: the drain-loop law, stated as arithmetic ─────────
    // A media event is a coalescing prompt, not a payload: one wakeup can
    // carry eight ring slots. Measuring "the buffer that woke us" would throw
    // away up to seven eighths of the audio. This asserts that feeding the
    // same samples in 10 ms pieces and in one 2 s piece are the same
    // measurement, which is what makes feeding from inside the drain loop
    // correct.
    {
        const uint32_t rate = 32000;
        const size_t n = rate * 2;
        std::vector<int16_t> pcm(n);
        for (size_t i = 0; i < n; ++i) {
            const double v = 0.2 * std::sin(2.0 * 3.14159265358979323846 *
                                            440.0 * static_cast<double>(i) /
                                            static_cast<double>(rate));
            pcm[i] = static_cast<int16_t>(std::lround(v * 32767.0));
        }
        LoudnessMeter whole;
        loudness_meter_feed_int16(whole, pcm.data(), n, 1, rate);

        LoudnessMeter pieces;
        const size_t chunk = rate / 100;   // 10 ms, Zoom's buffer size
        for (size_t off = 0; off < n; off += chunk) {
            const size_t take = (off + chunk <= n) ? chunk : (n - off);
            loudness_meter_feed_int16(pieces, pcm.data() + off, take, 1, rate);
        }

        double a = 0.0, b = 0.0;
        check(loudness_meter_integrated(whole, &a) &&
              loudness_meter_integrated(pieces, &b),
              "one of the two feeding patterns produced no integrated value");
        check(near(a, b, 1e-9),
              "feeding in 10 ms chunks did not match feeding in one buffer -- "
              "the hop boundary is following call boundaries instead of frame "
              "counts, so the measurement depends on IPC batching");
        double ma = 0.0, mb = 0.0;
        check(loudness_meter_momentary(whole, &ma) &&
              loudness_meter_momentary(pieces, &mb) && near(ma, mb, 1e-9),
              "momentary differed between chunked and whole feeding");
    }
```

- [ ] **Step 2: Run the test to verify it fails**

```sh
cmake --build build --config Release --parallel 8
```

Expected: FAIL at compile time — `'loudness_meter_integrated': identifier not found`.

- [ ] **Step 3: Write the implementation**

In `src/audio-loudness.h`, **delete** the placeholder line added at the end in Task 2:

```cpp
inline void loudness_meter_on_hop_complete(LoudnessMeter &) {}
```

Add these fields to `struct LoudnessMeter`, after `uint64_t hop_total = 0;`:

```cpp
    // The gated integration window -- ONE PANELIST'S MIC CHECK, not the
    // session. Each entry is the mean square of a 400 ms block that cleared
    // the absolute gate. Held as values rather than a running sum because the
    // relative gate has to re-examine every block once the absolute-gated
    // mean is known.
    std::vector<double> gated;
    size_t   gated_head  = 0;   // ring write position once `gated` is full
    uint64_t gated_total = 0;   // blocks ever admitted, never wrapped
```

Append after `loudness_meter_short_term`:

```cpp
// BS.1770-4's two gates. The absolute one discards silence for free, which is
// exactly the mechanism a panel needs: a panelist is silent roughly 80% of a
// panel, and an ungated integrated reading over that measures the meeting
// rather than the microphone (measured: 4 s of -20 LUFS speech inside 20 s
// reads -27.08 ungated). The relative one then discards the quiet tail so the
// answer describes the speech.
constexpr double kLoudnessAbsoluteGateLufs = -70.0;
constexpr double kLoudnessRelativeGateLu   = -10.0;

// 6000 blocks is 10 minutes of continuously-gated audio at a 100 ms hop. A
// mic check is 20-60 s (~200-600 blocks), so this is never reached in the
// use this was built for; past it the window keeps the most RECENT 10 minutes
// rather than growing without bound. Documented rather than silent, because
// "the oldest audio quietly leaves the window" is a real semantic and an
// operator who leaves a board running all show is entitled to know it.
constexpr size_t kLoudnessMaxGatedBlocks = 6000;

// Called at every completed 100 ms hop. A 400 ms block is the newest four
// hops, so admitting one block per hop is the standard's 75% overlap.
inline void loudness_meter_on_hop_complete(LoudnessMeter &m)
{
    double z = 0.0;
    if (!loudness_hop_mean(m, kLoudnessMomentaryHops, &z)) return;
    const double l = loudness_lufs_from_mean_square(z);
    if (!std::isfinite(l) || l <= kLoudnessAbsoluteGateLufs) return;

    if (m.gated.size() < kLoudnessMaxGatedBlocks) {
        m.gated.push_back(z);
    } else {
        m.gated[m.gated_head] = z;
        m.gated_head = (m.gated_head + 1) % kLoudnessMaxGatedBlocks;
    }
    ++m.gated_total;
}

// Starts this source's check window over. Clears the gated blocks and the hop
// history, but NOT the biquad state: the filters describe the signal that is
// still arriving, and zeroing them mid-stream would inject a transient into
// the first block of the new window.
inline void loudness_meter_reset_window(LoudnessMeter &m)
{
    m.gated.clear();
    m.gated_head  = 0;
    m.gated_total = 0;
    m.hop_acc     = 0.0;
    m.hop_filled  = 0;
    for (uint32_t i = 0; i < kLoudnessShortTermHops; ++i) m.hop_ring[i] = 0.0;
    m.hop_total   = 0;
}

// Blocks admitted to the current window. A board uses this to decide whether
// an integrated reading is worth showing: the spec's 20 s mic check yields
// ~200 blocks, so a handful of blocks is a cough, not a check.
inline uint64_t loudness_meter_gated_blocks(const LoudnessMeter &m)
{
    return m.gated_total;
}

// Integrated (I): the two-pass gate, over the current check window.
// False means "this panelist has not produced a measurable check yet", which
// is a different statement from any loudness value and must stay
// distinguishable all the way to the board.
inline bool loudness_meter_integrated(const LoudnessMeter &m, double *out_lufs)
{
    if (m.gated.empty()) return false;

    double sum = 0.0;
    for (double z : m.gated) sum += z;
    const double abs_mean_lufs =
        loudness_lufs_from_mean_square(sum / static_cast<double>(m.gated.size()));
    if (!std::isfinite(abs_mean_lufs)) return false;

    const double relative_threshold = abs_mean_lufs + kLoudnessRelativeGateLu;
    double sum2 = 0.0;
    size_t n2 = 0;
    for (double z : m.gated) {
        // Strictly greater, per BS.1770-4: a block exactly on the threshold
        // is excluded.
        if (loudness_lufs_from_mean_square(z) > relative_threshold) {
            sum2 += z;
            ++n2;
        }
    }
    if (n2 == 0) return false;

    const double l = loudness_lufs_from_mean_square(sum2 /
                                                    static_cast<double>(n2));
    if (!std::isfinite(l)) return false;
    *out_lufs = l;
    return true;
}
```

- [ ] **Step 4: Run the test to verify it passes**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release -R CoreVideoAudioLoudness --output-on-failure
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/audio-loudness.h tests/audio-loudness-test.cpp
git commit -m "feat(loudness): gated integrated loudness over a resettable per-panelist check window"
```

---

### Task 4: The readiness-board model — panel median, deviation, status, layout

**Files:**
- Create: `src/loudness-board.h`
- Create: `tests/loudness-board-test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from earlier tasks (deliberately independent of `audio-loudness.h` so the board can be reasoned about with hand-written readings).
- Produces:
  - `struct LoudnessReading { std::string source_uuid, display_name; uint32_t participant_id; bool subscribed; bool has_short_term; double short_term_lufs; bool has_integrated; double integrated_lufs; uint64_t gated_blocks; };`
  - `enum class LoudnessReference { PanelMedian, EbuR128, AtscA85, Streaming };`
  - `enum class LoudnessRowStatus { NoAudio, Measuring, Pass, Loud, Quiet };`
  - `struct LoudnessBoardRow { std::string name, detail; bool has_deviation; double deviation_lu; bool has_short_term; double short_term_lufs; bool has_integrated; double integrated_lufs; LoudnessRowStatus status; };`
  - `struct LoudnessBoardModel { bool has_reference; double reference_lufs; LoudnessReference reference_kind; std::vector<LoudnessBoardRow> rows; std::string signature; };`
  - `constexpr uint64_t kLoudnessBoardMinBlocks = 30;`
  - `constexpr double kLoudnessBoardDefaultToleranceLu = 2.0;`
  - `bool loudness_panel_median(const std::vector<LoudnessReading> &, uint64_t min_blocks, double *out);`
  - `LoudnessBoardModel loudness_board_build(const std::vector<LoudnessReading> &, LoudnessReference, double tolerance_lu, uint64_t min_blocks);`
  - `struct LoudnessBoardRect { int x, y, w, h; };`
  - `LoudnessBoardRect loudness_board_row_rect(int canvas_w, int canvas_h, size_t row_count, size_t row_index);`
  - `LoudnessBoardRect loudness_board_bar_rect(const LoudnessBoardRect &row, double deviation_lu, double full_scale_lu);`
  - `constexpr int kLoudnessBoardHeaderPx = 28;` / `kLoudnessBoardRowGapPx = 4;` / `kLoudnessBoardFullScaleLu = 6.0;`

- [ ] **Step 1: Write the failing test**

Create `tests/loudness-board-test.cpp`:

```cpp
// tests/loudness-board-test.cpp
// The readiness board: what an operator actually reads during a mic check.
//
// The product claim is relative, not absolute. An operator does not primarily
// care that a panelist hits -23 LUFS; they care that panelist A is not 6 LU
// louder than panelist B. So the headline number is deviation from the panel
// MEDIAN of gated integrated loudness -- median, because one panelist on a
// laptop mic at -35 LUFS must not drag the reference everyone else is judged
// against, which is exactly what a mean does.
//
// The layout arithmetic is pinned here too rather than looked at on screen.
// This repo has no headless GPU harness and has ruled against building one
// (an offscreen Qt harness "certified it three times and was wrong three
// times"); the sanctioned approach is to extract the decision into a pure
// header and unit-test that, the way tests/tile-shape-test.cpp reproduces the
// shader's crop arithmetic.
#include "loudness-board.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

static bool near(double a, double b, double tol)
{
    return std::fabs(a - b) <= tol;
}

static LoudnessReading measured(const char *name, double integrated,
                                double short_term = -20.0,
                                uint64_t blocks = 200)
{
    LoudnessReading r;
    r.source_uuid     = std::string("uuid_") + name;
    r.display_name    = name;
    r.participant_id  = 1;
    r.subscribed      = true;
    r.has_short_term  = true;
    r.short_term_lufs = short_term;
    r.has_integrated  = true;
    r.integrated_lufs = integrated;
    r.gated_blocks    = blocks;
    return r;
}

int main()
{
    // ── Median, not mean ───────────────────────────────────────────────────
    {
        std::vector<LoudnessReading> panel = {
            measured("Ana",   -18.0), measured("Ben",   -21.0),
            measured("Cara",  -23.0), measured("Dev",   -24.0),
            measured("Erik",  -30.0),
        };
        double median = 0.0;
        check(loudness_panel_median(panel, kLoudnessBoardMinBlocks, &median),
              "no panel median was produced from five measured panelists");
        check(near(median, -23.0, 1e-9),
              "the panel reference is not the median -- the mean of this "
              "panel is -23.2, and Erik at -30 is exactly the outlier the "
              "median exists to survive");
    }

    // ── An even panel averages the two middle values ───────────────────────
    {
        std::vector<LoudnessReading> panel = {
            measured("Ana", -18.0), measured("Ben", -21.0),
            measured("Cara", -23.0), measured("Dev", -24.0),
        };
        double median = 0.0;
        check(loudness_panel_median(panel, kLoudnessBoardMinBlocks, &median),
              "no median from an even-sized panel");
        check(near(median, -22.0, 1e-9),
              "an even-sized panel's median was not the mean of the two "
              "middle values");
    }

    // ── Unmeasured panelists must not vote on the reference ────────────────
    {
        LoudnessReading quiet;
        quiet.source_uuid  = "uuid_Fay";
        quiet.display_name = "Fay";
        quiet.subscribed   = true;
        // never spoke: no integrated value at all
        std::vector<LoudnessReading> panel = {
            measured("Ana", -18.0), measured("Ben", -22.0), quiet,
        };
        double median = 0.0;
        check(loudness_panel_median(panel, kLoudnessBoardMinBlocks, &median),
              "a panel with one silent member produced no median");
        check(near(median, -20.0, 1e-9),
              "a panelist with no integrated reading was counted in the "
              "median -- a person who has not spoken is not a data point");
    }

    // ── A too-short check does not count either ────────────────────────────
    {
        std::vector<LoudnessReading> panel = {
            measured("Ana", -18.0, -18.0, 200),
            measured("Ben", -22.0, -22.0, 200),
            measured("Cough", -5.0, -5.0, 4),   // four blocks: 400 ms
        };
        double median = 0.0;
        check(loudness_panel_median(panel, kLoudnessBoardMinBlocks, &median),
              "no median produced");
        check(near(median, -20.0, 1e-9),
              "a 400 ms cough set the panel reference -- the minimum gated "
              "block count is not being applied");
    }

    // ── No measurable panelist means NO reference, not zero ────────────────
    {
        std::vector<LoudnessReading> panel;
        double median = 0.0;
        check(!loudness_panel_median(panel, kLoudnessBoardMinBlocks, &median),
              "an empty panel produced a reference value");
    }

    // ── Deviation sign, and status ─────────────────────────────────────────
    {
        std::vector<LoudnessReading> panel = {
            measured("Ana",  -18.0), measured("Ben",  -21.0),
            measured("Cara", -23.0), measured("Dev",  -24.0),
            measured("Erik", -30.0),
        };
        const LoudnessBoardModel m = loudness_board_build(
            panel, LoudnessReference::PanelMedian,
            kLoudnessBoardDefaultToleranceLu, kLoudnessBoardMinBlocks);
        check(m.has_reference && near(m.reference_lufs, -23.0, 1e-9),
              "the built model's reference is not the panel median");
        check(m.rows.size() == 5, "the board did not produce one row per panelist");
        // Rows are ordered by name from CONTENT alone.
        check(m.rows[0].name == "Ana" && m.rows[4].name == "Erik",
              "rows are not in deterministic name order");
        check(near(m.rows[0].deviation_lu, 5.0, 1e-9),
              "a panelist 5 LU above the median did not report +5 LU -- "
              "louder than the reference must be POSITIVE");
        check(near(m.rows[4].deviation_lu, -7.0, 1e-9),
              "a panelist 7 LU below the median did not report -7 LU");
        check(m.rows[0].status == LoudnessRowStatus::Loud,
              "+5 LU was not flagged as too loud at a 2 LU tolerance");
        check(m.rows[4].status == LoudnessRowStatus::Quiet,
              "-7 LU was not flagged as too quiet");
        check(m.rows[2].status == LoudnessRowStatus::Pass,
              "the panelist sitting exactly on the median did not pass");
        check(m.rows[1].status == LoudnessRowStatus::Pass,
              "-21 against a -23 median is +2 LU, exactly the tolerance, and "
              "must pass -- the boundary is inclusive");
    }

    // ── Fixed-target presets ───────────────────────────────────────────────
    {
        std::vector<LoudnessReading> panel = { measured("Ana", -18.0) };
        const LoudnessBoardModel r128 = loudness_board_build(
            panel, LoudnessReference::EbuR128, 2.0, kLoudnessBoardMinBlocks);
        check(r128.has_reference && near(r128.reference_lufs, -23.0, 1e-9),
              "EBU R128 preset is not -23 LUFS");
        check(near(r128.rows[0].deviation_lu, 5.0, 1e-9),
              "-18 against the R128 target is not +5 LU");

        const LoudnessBoardModel a85 = loudness_board_build(
            panel, LoudnessReference::AtscA85, 2.0, kLoudnessBoardMinBlocks);
        check(near(a85.reference_lufs, -24.0, 1e-9),
              "ATSC A/85 preset is not -24 LKFS");

        const LoudnessBoardModel str = loudness_board_build(
            panel, LoudnessReference::Streaming, 2.0, kLoudnessBoardMinBlocks);
        check(near(str.reference_lufs, -16.0, 1e-9),
              "the streaming preset is not -16 LUFS");
    }

    // ── A fixed target works with NOBODY measured; the median does not ─────
    {
        LoudnessReading silent;
        silent.source_uuid  = "uuid_Ana";
        silent.display_name = "Ana";
        silent.subscribed   = true;
        std::vector<LoudnessReading> panel = { silent };

        const LoudnessBoardModel med = loudness_board_build(
            panel, LoudnessReference::PanelMedian, 2.0, kLoudnessBoardMinBlocks);
        check(!med.has_reference,
              "a panel median was invented from a panel nobody has spoken on");
        check(med.rows.size() == 1 && !med.rows[0].has_deviation &&
              med.rows[0].status == LoudnessRowStatus::NoAudio,
              "a silent panelist was given a deviation");

        const LoudnessBoardModel fixed = loudness_board_build(
            panel, LoudnessReference::EbuR128, 2.0, kLoudnessBoardMinBlocks);
        check(fixed.has_reference,
              "a FIXED target disappeared because nobody had spoken -- the "
              "target does not depend on the panel");
        check(!fixed.rows[0].has_deviation,
              "a silent panelist got a deviation against a fixed target");
    }

    // ── Measuring: subscribed and audible, but not enough blocks yet ───────
    {
        std::vector<LoudnessReading> panel = {
            measured("Ana", -20.0, -20.0, 200),
            measured("Ben", -20.0, -20.0, 5),
        };
        const LoudnessBoardModel m = loudness_board_build(
            panel, LoudnessReference::PanelMedian, 2.0, kLoudnessBoardMinBlocks);
        check(m.rows[1].status == LoudnessRowStatus::Measuring,
              "a panelist with 5 gated blocks was given a verdict rather than "
              "reported as still measuring");
        check(!m.rows[1].has_deviation,
              "a still-measuring panelist was given a deviation");
    }

    // ── The signature changes on content and NOT on input order ────────────
    // The Talkback dock shipped a live defect (2026-08-29) where a merely
    // REORDERED roster rebuilt the whole widget list several times a second
    // and threw away the operator's clicks. The board's consumer rebuilds
    // child text sources off this signature, so the same rule applies here.
    {
        std::vector<LoudnessReading> a = {
            measured("Ana", -20.0), measured("Ben", -22.0),
        };
        std::vector<LoudnessReading> b = { a[1], a[0] };   // same set, reordered
        const LoudnessBoardModel ma = loudness_board_build(
            a, LoudnessReference::PanelMedian, 2.0, kLoudnessBoardMinBlocks);
        const LoudnessBoardModel mb = loudness_board_build(
            b, LoudnessReference::PanelMedian, 2.0, kLoudnessBoardMinBlocks);
        check(ma.signature == mb.signature,
              "reordering the input changed the board signature -- the "
              "consumer will rebuild its text children on every roster event");

        std::vector<LoudnessReading> c = {
            measured("Ana", -20.0), measured("Ben", -26.0),
        };
        const LoudnessBoardModel mc = loudness_board_build(
            c, LoudnessReference::PanelMedian, 2.0, kLoudnessBoardMinBlocks);
        check(ma.signature != mc.signature,
              "a 4 LU change in one panelist did not change the signature");
    }

    // ── Layout: rows tile the canvas below the header, in order ────────────
    {
        const LoudnessBoardRect r0 = loudness_board_row_rect(640, 360, 4, 0);
        const LoudnessBoardRect r3 = loudness_board_row_rect(640, 360, 4, 3);
        check(r0.x == 0 && r0.w == 640, "a row does not span the canvas width");
        check(r0.y == kLoudnessBoardHeaderPx,
              "the first row does not start below the header band");
        check(r0.h == 79,
              "a 4-row board on a 640x360 canvas did not give 79 px rows "
              "((360-28)/4 - 4 gap)");
        check(r3.y == kLoudnessBoardHeaderPx + 83 * 3,
              "row 3 is not at the fourth slot");
        check(r3.y + r3.h <= 360,
              "the last row overflows the canvas");
        const LoudnessBoardRect bad = loudness_board_row_rect(640, 360, 4, 9);
        check(bad.w == 0 && bad.h == 0,
              "an out-of-range row index produced a drawable rect");
        const LoudnessBoardRect none = loudness_board_row_rect(640, 360, 0, 0);
        check(none.w == 0 && none.h == 0,
              "a zero-row board produced a drawable rect");
    }

    // ── Layout: the bar grows from the centre of the right half ────────────
    {
        const LoudnessBoardRect row{0, 28, 640, 79};
        const LoudnessBoardRect zero =
            loudness_board_bar_rect(row, 0.0, kLoudnessBoardFullScaleLu);
        check(zero.w == 0 && zero.x == 480,
              "a zero deviation did not collapse to nothing at the centre "
              "line (x=480 on a 640 px row)");

        const LoudnessBoardRect hot =
            loudness_board_bar_rect(row, 3.0, kLoudnessBoardFullScaleLu);
        check(hot.x == 480 && hot.w == 80,
              "+3 LU of a 6 LU full scale did not fill half the right side");

        const LoudnessBoardRect cold =
            loudness_board_bar_rect(row, -6.0, kLoudnessBoardFullScaleLu);
        check(cold.x == 320 && cold.w == 160,
              "-6 LU did not fill the left half of the meter");

        const LoudnessBoardRect clipped =
            loudness_board_bar_rect(row, 40.0, kLoudnessBoardFullScaleLu);
        check(clipped.x == 480 && clipped.w == 160 &&
              clipped.x + clipped.w <= 640,
              "an off-the-scale deviation drew past the canvas instead of "
              "clamping at full scale");
        check(cold.y == row.y && cold.h == row.h,
              "the bar's vertical extent does not match its row");
    }

    if (failures == 0)
        std::cout << "loudness-board: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register the test in CMake**

In `CMakeLists.txt`, immediately after the `CoreVideoAudioLoudnessTest` block added in Task 1:

```cmake
    # The readiness board's model: panel MEDIAN (not mean, so one laptop mic
    # cannot drag the reference), deviation in LU, pass/fail, deterministic
    # row order, and the row/bar rectangle arithmetic. The layout is pinned
    # here rather than looked at on screen because this repo has no headless
    # GPU harness and has ruled against building one. See src/loudness-board.h.
    add_executable(CoreVideoLoudnessBoardTest
        tests/loudness-board-test.cpp
    )
    target_include_directories(CoreVideoLoudnessBoardTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoLoudnessBoard
             COMMAND CoreVideoLoudnessBoardTest)
```

- [ ] **Step 3: Run the test to verify it fails**

```sh
cmake --build build --config Release --parallel 8
```

Expected: FAIL at compile time — `Cannot open include file: 'loudness-board.h'`.

- [ ] **Step 4: Write the implementation**

Create `src/loudness-board.h`:

```cpp
#pragma once

// The preshow readiness board: one row per panelist, showing how far their
// loudness sits from the panel's, and whether that is acceptable.
//
// WHY THE HEADLINE NUMBER IS RELATIVE. An operator running a mic check does
// not primarily care that a panelist hits -23 LUFS. They care that panelist A
// is not 6 LU louder than panelist B, because that is what the audience
// hears. So the reference defaults to the panel's own MEDIAN gated integrated
// loudness and the number on each row is a deviation in LU. Median, never
// mean: one person on a laptop mic at -35 LUFS would drag a mean far enough
// to fail everybody else, which is the opposite of useful.
//
// WHY THE LAYOUT MATHS IS IN HERE TOO. This repo has no headless GPU harness
// and has ruled against building one -- an offscreen Qt harness certified the
// Talkback dock's layout three times and was wrong three times. The sanctioned
// approach is to extract the decision into a pure header and unit-test that,
// the way tests/tile-shape-test.cpp reproduces the tile shader's crop
// arithmetic in plain C++. So the row and bar rectangles are decided here and
// the renderer only fills them.
//
// Pure: no libobs, no Qt, no Zoom SDK.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// One live audio source's measurement, flattened for the board. Every "has_"
// flag is load-bearing: "this panelist has not produced a measurable check"
// is a different statement from any loudness value, and collapsing it to a
// sentinel number is how a board ends up confidently reporting -70 LUFS for
// somebody who simply has not spoken yet.
struct LoudnessReading {
    std::string source_uuid;
    std::string display_name;
    uint32_t    participant_id  = 0;
    bool        subscribed      = false;
    bool        has_short_term  = false;
    double      short_term_lufs = 0.0;
    bool        has_integrated  = false;
    double      integrated_lufs = 0.0;
    uint64_t    gated_blocks    = 0;
};

// Default: the panel's own median. The presets exist because a show sometimes
// has a delivery spec, but matching each other is the actual goal here, which
// is why PanelMedian is first and is the default.
enum class LoudnessReference {
    PanelMedian = 0,
    EbuR128     = 1,   // -23 LUFS
    AtscA85     = 2,   // -24 LKFS
    Streaming   = 3,   // -16 LUFS
};

enum class LoudnessRowStatus {
    NoAudio   = 0,   // subscribed to nobody, or nobody has spoken
    Measuring = 1,   // audible, but not enough gated blocks for a verdict
    Pass      = 2,
    Loud      = 3,
    Quiet     = 4,
};

// Minimum gated blocks before a reading is treated as a check rather than a
// noise. At a 100 ms hop this is 3 s of gated speech; the spec's 20 s mic
// check yields roughly 200. Set low enough that an operator sees a verdict
// while the panelist is still talking, high enough that a cough or a chair
// scrape cannot set the panel reference for everybody.
constexpr uint64_t kLoudnessBoardMinBlocks = 30;

// +/- this many LU from the reference still passes. 2 LU is below the ~3 LU
// step most listeners call "noticeably louder", so a passing board really is
// a matched panel.
constexpr double kLoudnessBoardDefaultToleranceLu = 2.0;

// The deviation at which the bar is full. Beyond it the bar clamps rather
// than growing, because past 6 LU the exact number stops mattering: the
// answer is already "fix this microphone".
constexpr double kLoudnessBoardFullScaleLu = 6.0;

constexpr int kLoudnessBoardHeaderPx = 28;
constexpr int kLoudnessBoardRowGapPx = 4;

struct LoudnessBoardRow {
    std::string       name;
    std::string       detail;          // short status text for the row
    bool              has_deviation  = false;
    double            deviation_lu   = 0.0;
    bool              has_short_term = false;
    double            short_term_lufs = 0.0;
    bool              has_integrated = false;
    double            integrated_lufs = 0.0;
    LoudnessRowStatus status = LoudnessRowStatus::NoAudio;
};

struct LoudnessBoardModel {
    bool              has_reference  = false;
    double            reference_lufs = 0.0;
    LoudnessReference reference_kind = LoudnessReference::PanelMedian;
    std::vector<LoudnessBoardRow> rows;
    // Changes only when something an operator can SEE changed. The consumer
    // rebuilds its child text sources off this, and the Talkback dock's
    // 2026-08-29 live defect -- a merely reordered roster rebuilding the
    // whole widget list several times a second and eating the operator's
    // clicks -- is why it is derived from sorted content and never from
    // input order.
    std::string signature;
};

// The fixed presets. PanelMedian has no fixed value and returns false.
inline bool loudness_reference_fixed_target(LoudnessReference kind, double *out)
{
    switch (kind) {
    case LoudnessReference::EbuR128:   *out = -23.0; return true;
    case LoudnessReference::AtscA85:   *out = -24.0; return true;
    case LoudnessReference::Streaming: *out = -16.0; return true;
    case LoudnessReference::PanelMedian:
    default:                           return false;
    }
}

// Median of the gated integrated loudness of everyone who has actually
// produced a check. Even counts average the two middle values, which is the
// ordinary definition and keeps a two-person panel from arbitrarily electing
// one of them as the reference.
inline bool loudness_panel_median(const std::vector<LoudnessReading> &readings,
                                  uint64_t min_blocks, double *out)
{
    std::vector<double> values;
    values.reserve(readings.size());
    for (const LoudnessReading &r : readings) {
        if (!r.has_integrated) continue;
        if (r.gated_blocks < min_blocks) continue;
        if (!std::isfinite(r.integrated_lufs)) continue;
        values.push_back(r.integrated_lufs);
    }
    if (values.empty()) return false;
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    *out = (n % 2 == 1) ? values[n / 2]
                        : 0.5 * (values[n / 2 - 1] + values[n / 2]);
    return true;
}

inline const char *loudness_row_status_text(LoudnessRowStatus s)
{
    switch (s) {
    case LoudnessRowStatus::NoAudio:   return "no audio";
    case LoudnessRowStatus::Measuring: return "measuring";
    case LoudnessRowStatus::Pass:      return "ok";
    case LoudnessRowStatus::Loud:      return "too loud";
    case LoudnessRowStatus::Quiet:     return "too quiet";
    default:                           return "";
    }
}

inline LoudnessBoardModel loudness_board_build(
    const std::vector<LoudnessReading> &readings,
    LoudnessReference kind, double tolerance_lu, uint64_t min_blocks)
{
    LoudnessBoardModel model;
    model.reference_kind = kind;
    if (!(tolerance_lu > 0.0)) tolerance_lu = kLoudnessBoardDefaultToleranceLu;

    double reference = 0.0;
    if (loudness_reference_fixed_target(kind, &reference)) {
        // A fixed target does not depend on the panel, so it survives a panel
        // nobody has spoken on. The median does not, and must not be invented.
        model.has_reference  = true;
        model.reference_lufs = reference;
    } else if (loudness_panel_median(readings, min_blocks, &reference)) {
        model.has_reference  = true;
        model.reference_lufs = reference;
    }

    // Ordered by CONTENT alone -- name, then uuid to break a duplicate-name
    // tie -- so a roster that merely reorders produces an identical board.
    std::vector<const LoudnessReading *> ordered;
    ordered.reserve(readings.size());
    for (const LoudnessReading &r : readings) ordered.push_back(&r);
    std::sort(ordered.begin(), ordered.end(),
              [](const LoudnessReading *a, const LoudnessReading *b) {
                  if (a->display_name != b->display_name)
                      return a->display_name < b->display_name;
                  return a->source_uuid < b->source_uuid;
              });

    model.rows.reserve(ordered.size());
    for (const LoudnessReading *r : ordered) {
        LoudnessBoardRow row;
        row.name = r->display_name.empty()
                       ? (r->participant_id != 0
                              ? "ID " + std::to_string(r->participant_id)
                              : std::string("- unassigned -"))
                       : r->display_name;
        row.has_short_term  = r->has_short_term;
        row.short_term_lufs = r->short_term_lufs;
        row.has_integrated  = r->has_integrated;
        row.integrated_lufs = r->integrated_lufs;

        if (!r->has_integrated || r->gated_blocks == 0) {
            row.status = LoudnessRowStatus::NoAudio;
        } else if (r->gated_blocks < min_blocks) {
            row.status = LoudnessRowStatus::Measuring;
        } else if (model.has_reference) {
            row.has_deviation = true;
            row.deviation_lu  = r->integrated_lufs - model.reference_lufs;
            if (row.deviation_lu > tolerance_lu)
                row.status = LoudnessRowStatus::Loud;
            else if (row.deviation_lu < -tolerance_lu)
                row.status = LoudnessRowStatus::Quiet;
            else
                row.status = LoudnessRowStatus::Pass;   // boundary is inclusive
        } else {
            row.status = LoudnessRowStatus::Measuring;
        }
        row.detail = loudness_row_status_text(row.status);
        model.rows.push_back(std::move(row));
    }

    // Deviation is quantised to 0.1 LU in the signature: the renderer prints
    // one decimal place, so a change smaller than that is invisible and must
    // not cost a text-source rebuild.
    std::string sig;
    sig.reserve(model.rows.size() * 24 + 16);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "R%d:%s%.1f|",
                  static_cast<int>(kind), model.has_reference ? "" : "x",
                  model.has_reference ? model.reference_lufs : 0.0);
    sig += buf;
    for (const LoudnessBoardRow &row : model.rows) {
        sig += row.name;
        std::snprintf(buf, sizeof(buf), "|%d|%s%.1f;",
                      static_cast<int>(row.status),
                      row.has_deviation ? "" : "x",
                      row.has_deviation ? row.deviation_lu : 0.0);
        sig += buf;
    }
    model.signature = std::move(sig);
    return model;
}

struct LoudnessBoardRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// One row's band. A zero-size result means "do not draw", which is what every
// degenerate input produces -- the renderer checks w/h rather than
// re-validating the arguments it just passed in.
inline LoudnessBoardRect loudness_board_row_rect(int canvas_w, int canvas_h,
                                                 size_t row_count,
                                                 size_t row_index)
{
    LoudnessBoardRect r;
    if (canvas_w <= 0 || canvas_h <= 0 || row_count == 0 ||
        row_index >= row_count)
        return r;
    const int body_top = kLoudnessBoardHeaderPx;
    const int body_h   = canvas_h - body_top;
    if (body_h <= 0) return r;
    const int slot = body_h / static_cast<int>(row_count);
    if (slot <= 0) return r;
    const int h = slot - kLoudnessBoardRowGapPx;
    r.x = 0;
    r.w = canvas_w;
    r.y = body_top + slot * static_cast<int>(row_index);
    r.h = (h > 0) ? h : slot;
    return r;
}

// The deviation bar, growing right (louder) or left (quieter) from the centre
// of the row's right half. Clamped at full scale rather than allowed to run
// off the canvas: past 6 LU the exact number has stopped mattering.
inline LoudnessBoardRect loudness_board_bar_rect(const LoudnessBoardRect &row,
                                                 double deviation_lu,
                                                 double full_scale_lu)
{
    LoudnessBoardRect r;
    if (row.w <= 0 || row.h <= 0 || !(full_scale_lu > 0.0)) return r;
    const int meter_w = row.w / 2;            // right half is the meter
    const int meter_x = row.x + row.w - meter_w;
    const int half    = meter_w / 2;
    const int centre  = meter_x + half;

    double d = deviation_lu;
    if (!std::isfinite(d)) d = 0.0;
    if (d >  full_scale_lu) d =  full_scale_lu;
    if (d < -full_scale_lu) d = -full_scale_lu;

    const int len = static_cast<int>(std::fabs(d) / full_scale_lu *
                                     static_cast<double>(half) + 0.5);
    r.y = row.y;
    r.h = row.h;
    r.w = len;
    r.x = (d >= 0.0) ? centre : centre - len;
    return r;
}
```

- [ ] **Step 5: Run the test to verify it passes**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release -R CoreVideoLoudnessBoard --output-on-failure
```

Expected: PASS, `loudness-board: all tests passed`.

- [ ] **Step 6: Commit**

```bash
git add src/loudness-board.h tests/loudness-board-test.cpp CMakeLists.txt
git commit -m "feat(loudness): readiness-board model with panel-median reference and pinned layout arithmetic"
```

---

### Task 5: Feed the meter from the audio lane and expose readings

**Files:**
- Modify: `src/zoom-participant-audio-source.h`
- Modify: `src/zoom-participant-audio-source.cpp`

**Interfaces:**
- Consumes: `LoudnessMeter`, `loudness_meter_feed_int16`, `loudness_meter_reset_window`, `loudness_meter_momentary`, `loudness_meter_short_term`, `loudness_meter_integrated`, `loudness_meter_gated_blocks` (Tasks 2–3); `LoudnessReading` (Task 4).
- Produces:
  - `std::vector<LoudnessReading> corevideo_loudness_readings();`
  - `void corevideo_reset_loudness_windows();`

- [ ] **Step 1: Write the failing test**

There is no host test that can reach an OBS source, so the invariant this task depends on is pinned in the pure header instead — the same treatment `talkback-key.h` got. Append to `tests/audio-loudness-test.cpp`, before the closing `if (failures == 0)`:

```cpp
    // ── The wire format the tap actually hands over ────────────────────────
    // output_audio_frame() reads ShmAudioHeader::sample_rate and ::channels
    // per slot and can see them CHANGE mid-source: the engine restamps
    // whatever GetSampleRate() returned, and an operator flipping a target
    // between Mix (stereo) and Isolated (mono) changes the channel count on
    // the same live subscription. The meter must follow that without
    // carrying one format's filter history into the other's measurement.
    {
        LoudnessMeter m;
        feed_sine(m, 48000, std::sqrt(2.0) * 0.1, 1000.0, 2.0);
        check(m.sample_rate == 48000 && m.channels == 1,
              "the meter did not adopt the first buffer's wire format");

        // Same tone, now arriving at 32 kHz: the meter must re-derive rather
        // than keep filtering with 48 kHz coefficients.
        feed_sine(m, 32000, std::sqrt(2.0) * 0.1, 1000.0, 4.0);
        check(m.sample_rate == 32000,
              "a mid-stream rate change did not reconfigure the meter");
        double lufs = 0.0;
        check(loudness_meter_momentary(m, &lufs) && near(lufs, -19.98, 0.15),
              "after a mid-stream rate change the reading is wrong -- the "
              "coefficients did not follow");
        check(m.hop_frames == 3200,
              "the 100 ms hop is not 3200 frames at 32 kHz -- the hop length "
              "is fixed in samples instead of in time");
    }

    // ── A null or empty buffer is a no-op, not a crash ────────────────────
    // The drain loop can hand over a slot it failed to copy.
    {
        LoudnessMeter m;
        loudness_meter_feed_int16(m, nullptr, 480, 1, 48000);
        std::vector<int16_t> one(1, 0);
        loudness_meter_feed_int16(m, one.data(), 0, 1, 48000);
        loudness_meter_feed_int16(m, one.data(), 1, 0, 48000);
        check(m.hop_total == 0,
              "a degenerate feed advanced the measurement");
    }
```

- [ ] **Step 2: Run the test to verify it fails**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release -R CoreVideoAudioLoudness --output-on-failure
```

Expected: PASS for the new blocks if Tasks 2–3 were implemented correctly; if `hop_frames` is not exposed or the reconfigure path is missing, this FAILS with `the 100 ms hop is not 3200 frames at 32 kHz`. Run it and record which. If it passes, that is the correct outcome — these blocks exist to pin the behaviour the wiring below depends on before the wiring is written.

- [ ] **Step 3: Extend the public header**

In `src/zoom-participant-audio-source.h`, add the include and the two declarations at the end of the file:

```cpp
#include "loudness-board.h"
```

```cpp
// One BS.1770-4 reading per live CoreVideoAudioSource, for the readiness
// board. Safe to call from any thread; takes g_sources_mtx and then each
// source's own mutex, in that order and never the reverse.
//
// The display name here is a CACHED copy, refreshed on the engine's roster
// callback. ZoomEngineClient::roster() deep-copies every ParticipantInfo --
// strings included -- under the client's hot mutex, so resolving a name on
// the audio path (about a hundred buffers a second, per source) would put a
// full roster copy on the media path.
std::vector<LoudnessReading> corevideo_loudness_readings();

// Starts every live source's mic-check window over. Integrated loudness is
// scoped to ONE panelist's check, not the session: without this the number
// is polluted by whoever spoke before them on the same source.
void corevideo_reset_loudness_windows();
```

- [ ] **Step 4: Add the meter to the source context**

In `src/zoom-participant-audio-source.cpp`, add the include beside the other project includes at the top:

```cpp
#include "audio-loudness.h"
```

Add these members to `struct CoreVideoAudioSource`, immediately after `bool prev_was_silent = false;`:

```cpp
    // BS.1770-4 loudness for this participant. Same ownership as `timeline`
    // and `prev_was_silent`: advanced by the audio lane thread inside
    // output_audio_frame(), which holds ctx->mtx for the whole drain, and
    // read under that same mutex by corevideo_loudness_readings().
    LoudnessMeter loudness;
    // Set by any thread, consumed by the audio lane at the next slot. A reset
    // has to land on a hop boundary the meter itself controls, so it is a
    // request rather than a direct call: touching the meter from the caller's
    // thread would race the drain that is filling it.
    std::atomic<bool> loudness_reset_requested{false};
    // Display name, cached on the roster callback. Guarded by ctx->mtx.
    std::string display_name;
```

- [ ] **Step 5: Feed the meter inside the drain loop**

In `output_audio_frame()`, immediately after the line `ctx->prev_was_silent = cur_silent;` and before `const auto *pcm = pcm_mut;`, insert:

```cpp
            // ── BS.1770-4 metering ────────────────────────────────────────
            // HERE, inside the per-slot loop, and nowhere else. A media event
            // is a coalescing PROMPT, not a payload: one wakeup routinely
            // carries several ring slots and this loop drains until the ring
            // is seen empty. Measuring "the buffer that woke us" would
            // silently discard most of the audio and read low by a
            // load-dependent amount -- the worst shape of wrong, because it
            // looks fine on an idle box.
            //
            // Fed with the WIRE format (`pcm`, `channels`, `sample_rate`)
            // rather than the publish format assembled below: the operator's
            // Mono/Stereo choice is a routing decision for OBS, and a
            // mono-summed copy of a stereo panelist would read 3 LU different
            // from the same person carried as stereo. The measurement has to
            // describe what the panelist SENT.
            //
            // The resume fade above has already been applied to these
            // samples, which is correct: it is part of what we publish, it is
            // 3 ms long, and excluding it would mean measuring audio that
            // nobody hears.
            if (ctx->loudness_reset_requested.exchange(
                    false, std::memory_order_acq_rel)) {
                loudness_meter_reset_window(ctx->loudness);
            }
            loudness_meter_feed_int16(ctx->loudness, pcm_mut, pcm_frames,
                                      channels, sample_rate);
```

- [ ] **Step 6: Cache the display name on the roster callback, and reset on resubscribe**

In `maybe_resubscribe_for_roster()`, replace the `if (needs_roster) { ... }` block with this version (the only additions are the `cached_name` capture and the write-back after the block):

```cpp
    bool held_participant_present = true;
    std::string cached_name;
    const bool needs_roster =
        (state.subscribed && state.participant_id != 0) || target != 0;
    if (needs_roster) {
        const auto roster = ZoomEngineClient::instance().roster();
        const auto present = [&](uint32_t id) {
            return std::any_of(roster.begin(), roster.end(),
                               [&](const ParticipantInfo &p) {
                                   return p.user_id == id;
                               });
        };
        if (state.subscribed && state.participant_id != 0)
            held_participant_present = present(state.participant_id);
        if (target != 0 && !present(target))
            target = 0;

        // The ONE place a display name is resolved for this source. This
        // function runs on the engine's roster callback, which is exactly as
        // often as a name can change, and it has already paid for the roster
        // copy above. The readiness board reads the cached string instead of
        // calling roster() itself, because roster() deep-copies every
        // ParticipantInfo under the engine client's hot mutex and the board
        // asks ten times a second.
        const uint32_t name_for = target != 0 ? target : state.participant_id;
        if (name_for != 0) {
            for (const ParticipantInfo &p : roster) {
                if (p.user_id == name_for) {
                    cached_name = p.display_name;
                    break;
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(ctx->mtx);
        ctx->display_name = cached_name;
    }
```

In `unsubscribe_audio()`, inside the existing `{ std::lock_guard<std::mutex> lk(ctx->mtx); ... }` block that resets the timeline, add:

```cpp
        // A new subscription is a new panelist's mic check -- or the same
        // one after a gap of unknown length. Either way the previous
        // window's gated blocks describe audio that is not this check.
        loudness_meter_reset_window(ctx->loudness);
        ctx->display_name.clear();
```

In `forget_subscription_for_new_engine()`, inside its `{ std::lock_guard<std::mutex> lk(ctx->mtx); ... }` block, add the same two lines:

```cpp
        loudness_meter_reset_window(ctx->loudness);
        ctx->display_name.clear();
```

- [ ] **Step 7: Expose the readings through the existing registry**

In `src/zoom-participant-audio-source.cpp`, immediately after `corevideo_audio_source_infos()`, add:

```cpp
std::vector<LoudnessReading> corevideo_loudness_readings()
{
    std::vector<LoudnessReading> out;
    // Same lock order as corevideo_audio_source_infos(): g_sources_mtx first,
    // then each source's own mutex. The audio lane takes only ctx->mtx and
    // never touches g_sources_mtx, so this can never invert.
    std::lock_guard<std::mutex> lk(g_sources_mtx);
    out.reserve(g_sources.size());
    for (CoreVideoAudioSource *ctx : g_sources) {
        if (!ctx) continue;
        LoudnessReading r;
        r.source_uuid = ctx->source_uuid;
        r.participant_id =
            ctx->current_participant_id.load(std::memory_order_acquire);
        r.subscribed = ctx->subscribed.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> ctx_lk(ctx->mtx);
            r.display_name   = ctx->display_name;
            r.has_short_term = loudness_meter_short_term(ctx->loudness,
                                                         &r.short_term_lufs);
            r.has_integrated = loudness_meter_integrated(ctx->loudness,
                                                         &r.integrated_lufs);
            r.gated_blocks   = loudness_meter_gated_blocks(ctx->loudness);
        }
        out.push_back(std::move(r));
    }
    return out;
}

void corevideo_reset_loudness_windows()
{
    std::lock_guard<std::mutex> lk(g_sources_mtx);
    for (CoreVideoAudioSource *ctx : g_sources) {
        if (ctx)
            ctx->loudness_reset_requested.store(true,
                                                std::memory_order_release);
    }
    blog(LOG_INFO,
         "[obs-zoom-plugin] CoreVideo loudness: mic-check windows reset on %d "
         "source(s)",
         static_cast<int>(g_sources.size()));
}
```

- [ ] **Step 8: Build and run the whole suite**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: the plugin compiles and every test is green (N/N).

- [ ] **Step 9: Commit**

```bash
git add src/audio-loudness.h tests/audio-loudness-test.cpp \
        src/zoom-participant-audio-source.h src/zoom-participant-audio-source.cpp
git commit -m "feat(loudness): meter every participant on the audio lane and expose per-source readings"
```

---

### Task 6: The readiness-board meter source

**Files:**
- Create: `src/zoom-loudness-meter-source.h`
- Create: `src/zoom-loudness-meter-source.cpp`
- Modify: `CMakeLists.txt` (the `add_library(obs-zoom-plugin MODULE ...)` source list, line ~264)
- Modify: `src/plugin-main.cpp`
- Modify: `data/locale/en-US.ini`

**Interfaces:**
- Consumes: `corevideo_loudness_readings()`, `corevideo_reset_loudness_windows()` (Task 5); `loudness_board_build`, `loudness_board_row_rect`, `loudness_board_bar_rect`, `LoudnessBoardModel`, `LoudnessRowStatus`, `LoudnessReference` (Task 4); `TilesEffect`, `tiles_effect_load`, `tiles_effect_destroy` (existing, `src/zoom-tiles-effect.h`).
- Produces:
  - `void corevideo_loudness_meter_source_register();`
  - `void corevideo_loudness_meter_load_gfx();`
  - `void corevideo_loudness_meter_unload_gfx();`

- [ ] **Step 1: Write the failing test**

The rendering itself is not testable in this repo (no headless GPU harness, and one has been ruled against). Everything this source decides is already pinned in `tests/loudness-board-test.cpp` from Task 4. Add the one remaining decision — how many rows the source will draw and what it does when there are more panelists than the canvas can hold. Append to `tests/loudness-board-test.cpp`, before the closing `if (failures == 0)`:

```cpp
    // ── The board is bounded, and it says so ───────────────────────────────
    // A 25-person Zoom Events room would give rows a few pixels tall, which
    // is not a readiness board, it is a texture. The renderer caps the rows
    // it draws; the cap has to be a decision that can be reasoned about here
    // rather than a magic number buried in a draw loop.
    {
        check(loudness_board_visible_rows(360, 3) == 3,
              "three panelists on a 360 px canvas did not all fit");
        check(loudness_board_visible_rows(360, 40) ==
              (360 - kLoudnessBoardHeaderPx) / kLoudnessBoardMinRowPx,
              "forty panelists were not capped to what the canvas can show "
              "at the minimum readable row height");
        check(loudness_board_visible_rows(360, 0) == 0,
              "an empty panel produced rows to draw");
        check(loudness_board_visible_rows(0, 10) == 0,
              "a zero-height canvas produced rows to draw");
        const size_t capped = loudness_board_visible_rows(360, 40);
        const LoudnessBoardRect last =
            loudness_board_row_rect(640, 360, capped, capped - 1);
        check(last.h >= kLoudnessBoardMinRowPx,
              "the capped row count still produced rows below the minimum "
              "readable height");
    }
```

- [ ] **Step 2: Run the test to verify it fails**

```sh
cmake --build build --config Release --parallel 8
```

Expected: FAIL at compile time — `'loudness_board_visible_rows': identifier not found`.

- [ ] **Step 3: Add the cap to the pure header**

Append to `src/loudness-board.h`, after `loudness_board_bar_rect`:

```cpp
// The shortest row that is still a readiness board rather than a texture: a
// name and a number at a size an operator reads across a control room, plus
// the gap. A 25-person Zoom Events room would otherwise produce 13 px rows.
constexpr int kLoudnessBoardMinRowPx = 24;

// How many rows this canvas can actually show. Beyond it the renderer draws
// the first N (which, because rows are name-ordered, is stable frame to frame
// rather than shuffling) and says so in the header band.
inline size_t loudness_board_visible_rows(int canvas_h, size_t row_count)
{
    if (canvas_h <= kLoudnessBoardHeaderPx || row_count == 0) return 0;
    const int body_h = canvas_h - kLoudnessBoardHeaderPx;
    const size_t capacity =
        static_cast<size_t>(body_h / kLoudnessBoardMinRowPx);
    if (capacity == 0) return 0;
    return row_count < capacity ? row_count : capacity;
}
```

- [ ] **Step 4: Run the test to verify it passes**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release -R CoreVideoLoudnessBoard --output-on-failure
```

Expected: PASS.

- [ ] **Step 5: Write the source header**

Create `src/zoom-loudness-meter-source.h`:

```cpp
#pragma once

// The CoreVideo Loudness Meter: a preshow readiness board, drawn as an OBS
// source so it can sit on a multiview, a projector or a producer's monitor
// without a dock being open.
//
// One row per live CoreVideo audio source: the panelist's name, their
// deviation in LU from the panel reference, and a pass/fail verdict. Bars are
// drawn with the Solid technique already in data/effects/corevideo-tiles.effect
// -- there is no new effect file, because a solid quad is all a bar is and a
// second .effect is a second thing that can go missing beside a new DLL.
// Labels are private child text sources, so a Norwegian display name renders
// correctly instead of through a hand-rolled ASCII font.

void corevideo_loudness_meter_source_register();

// Compiles/releases the shared effect. Called from plugin-main.cpp alongside
// the Tiles equivalents; libobs caches effects created from a file, so this
// costs nothing beyond the Tiles source's own load.
void corevideo_loudness_meter_load_gfx();
void corevideo_loudness_meter_unload_gfx();
```

- [ ] **Step 6: Write the source implementation**

Create `src/zoom-loudness-meter-source.cpp`:

```cpp
#include "zoom-loudness-meter-source.h"

#include "loudness-board.h"
#include "zoom-participant-audio-source.h"
#include "zoom-tiles-effect.h"

#include <graphics/graphics.h>
#include <obs-module.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define PROP_REFERENCE  "reference"
#define PROP_TOLERANCE  "tolerance_lu"
#define PROP_WIDTH      "canvas_width"
#define PROP_HEIGHT     "canvas_height"
#define PROP_RESET      "btn_reset_windows"

static const char *kMeterSourceId = "corevideo_loudness_meter_source";

// Hard ceiling on child text sources, independent of the canvas cap in
// loudness_board_visible_rows(): each row costs two private sources and OBS
// renders every one of them, so the count is bounded by construction rather
// than by whatever canvas height an operator types in.
static constexpr size_t kMeterMaxRows = 16;

// 0xAARRGGBB, the same byte order picker_color_to_argb() produces for the
// Tiles wall, so gs_effect_set_color() reads them identically. Deliberately
// flat and high-contrast: this is read at a glance across a room, and the
// spec's legibility rule for anything meter-shaped is chunky segments and
// hard contrast, never hairlines.
static constexpr uint32_t kMeterBgArgb      = 0xFF12161Cu;
static constexpr uint32_t kMeterHeaderArgb  = 0xFF1E252Fu;
static constexpr uint32_t kMeterRowArgb     = 0xFF1A2029u;
static constexpr uint32_t kMeterCentreArgb  = 0xFF556070u;
static constexpr uint32_t kMeterPassArgb    = 0xFF2FBF6Fu;
static constexpr uint32_t kMeterLoudArgb    = 0xFFE04B4Bu;
static constexpr uint32_t kMeterQuietArgb   = 0xFFE0A03Cu;
static constexpr uint32_t kMeterIdleArgb    = 0xFF3A424Eu;

// Shared with the Tiles wall by file, not by handle: libobs caches effects
// created from a file, so this second tiles_effect_load() resolves the same
// compiled effect rather than compiling it twice.
static TilesEffect s_meter_effect;
static bool s_meter_pass_failed_logged = false;

struct meter_row_widgets {
    obs_source_t *name  = nullptr;
    obs_source_t *value = nullptr;
    std::string   name_text;
    std::string   value_text;
};

struct loudness_meter_source {
    obs_source_t *source = nullptr;

    std::atomic<uint32_t> canvas_width{640};
    std::atomic<uint32_t> canvas_height{360};
    std::atomic<int>      reference{0};       // LoudnessReference
    std::atomic<int>      tolerance_milli_lu{2000};

    std::mutex         mutex;                 // guards `model` and `rows`
    LoudnessBoardModel model;
    std::string        applied_signature;
    meter_row_widgets  rows[kMeterMaxRows];

    float rebuild_accum = 0.0f;
};

// ── Text children ───────────────────────────────────────────────────────────
//
// The board needs real text: panelist display names come from Zoom and this
// project has already been burned by names like "Ronny Hofsoy, Tromso" with
// their real diacritics (the Talkback dock's 400 px tower). A hand-rolled
// bitmap font would reintroduce exactly that class of defect, so the labels
// are OBS's own text sources, created private to this source.
//
// The id is PROBED rather than assumed: OBS ships text_ft2 and text_gdiplus
// on different platforms and has renamed both across versions. A build with
// neither must lose the labels and keep the bars, loudly -- never render an
// empty board with no explanation.
static const char *meter_text_source_id()
{
    static const char *cached = nullptr;
    static bool probed = false;
    if (probed) return cached;
    probed = true;
    static const char *candidates[] = {
        "text_ft2_source_v2", "text_gdiplus_v3", "text_gdiplus_v2",
        "text_ft2_source",    "text_gdiplus",
    };
    for (const char *id : candidates) {
        // obs_get_source_output_flags() returns 0 for an id no module
        // registered; a text source always carries OBS_SOURCE_VIDEO.
        if (obs_get_source_output_flags(id) != 0) {
            cached = id;
            break;
        }
    }
    if (!cached) {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] CoreVideo Loudness Meter: no OBS text source "
             "module is available; the board will draw bars without labels");
    } else {
        blog(LOG_INFO,
             "[obs-zoom-plugin] CoreVideo Loudness Meter: labels will use "
             "text source '%s'",
             cached);
    }
    return cached;
}

static obs_source_t *make_text_child(const char *private_name, int px,
                                     uint32_t argb)
{
    const char *id = meter_text_source_id();
    if (!id) return nullptr;

    obs_data_t *settings = obs_data_create();
    obs_data_t *font     = obs_data_create();
    obs_data_set_string(font, "face", "Arial");
    obs_data_set_string(font, "style", "Bold");
    obs_data_set_int(font, "size", px);
    obs_data_set_int(font, "flags", 0);
    obs_data_set_obj(settings, "font", font);
    obs_data_set_string(settings, "text", "");
    // text_gdiplus uses "color"; text_ft2 uses "color1"/"color2". Setting all
    // three is harmless on either and avoids a per-id branch that would have
    // to be revisited every time OBS renames one.
    obs_data_set_int(settings, "color",  static_cast<long long>(argb));
    obs_data_set_int(settings, "color1", static_cast<long long>(argb));
    obs_data_set_int(settings, "color2", static_cast<long long>(argb));
    obs_data_release(font);

    obs_source_t *src = obs_source_create_private(id, private_name, settings);
    obs_data_release(settings);
    return src;
}

static void set_text_child(obs_source_t *src, const char *text)
{
    if (!src) return;
    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "text", text);
    obs_source_update(src, settings);
    obs_data_release(settings);
}

// ── Drawing ─────────────────────────────────────────────────────────────────

static void meter_fill_rect(const LoudnessBoardRect &r, uint32_t argb)
{
    if (r.w <= 0 || r.h <= 0) return;
    gs_technique_t *solid = s_meter_effect.tech_solid;
    if (!solid || !s_meter_effect.param_color) return;
    // The colour must be set BEFORE begin_pass: libobs uploads a pass's
    // parameters inside gs_technique_begin_pass() and does not re-upload them
    // for later draws in the same pass. Same rule the Tiles border uniforms
    // live under.
    gs_effect_set_color(s_meter_effect.param_color, argb);
    gs_technique_begin(solid);
    if (gs_technique_begin_pass(solid, 0)) {
        gs_matrix_push();
        gs_matrix_translate3f(static_cast<float>(r.x),
                              static_cast<float>(r.y), 0.0f);
        gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(r.w),
                       static_cast<uint32_t>(r.h));
        gs_matrix_pop();
        gs_technique_end_pass(solid);
    } else if (!s_meter_pass_failed_logged) {
        // Once only. A board that silently stops drawing looks like the
        // source went transparent, with no clue why.
        s_meter_pass_failed_logged = true;
        blog(LOG_ERROR,
             "[obs-zoom-plugin] CoreVideo Loudness Meter: "
             "gs_technique_begin_pass failed on the Solid technique; the "
             "board will not draw");
    }
    gs_technique_end(solid);
}

static uint32_t status_color(LoudnessRowStatus s)
{
    switch (s) {
    case LoudnessRowStatus::Pass:  return kMeterPassArgb;
    case LoudnessRowStatus::Loud:  return kMeterLoudArgb;
    case LoudnessRowStatus::Quiet: return kMeterQuietArgb;
    default:                       return kMeterIdleArgb;
    }
}

static std::string row_value_text(const LoudnessBoardRow &row)
{
    char buf[96];
    if (row.has_deviation) {
        if (row.has_integrated) {
            std::snprintf(buf, sizeof(buf), "%+.1f LU   %.1f LUFS   %s",
                          row.deviation_lu, row.integrated_lufs,
                          row.detail.c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "%+.1f LU   %s",
                          row.deviation_lu, row.detail.c_str());
        }
    } else {
        std::snprintf(buf, sizeof(buf), "%s", row.detail.c_str());
    }
    return std::string(buf);
}

static std::string header_text(const LoudnessBoardModel &m, size_t shown,
                               size_t total)
{
    char buf[160];
    const char *kind = "panel median";
    switch (m.reference_kind) {
    case LoudnessReference::EbuR128:   kind = "EBU R128";   break;
    case LoudnessReference::AtscA85:   kind = "ATSC A/85";  break;
    case LoudnessReference::Streaming: kind = "streaming";  break;
    case LoudnessReference::PanelMedian:
    default: break;
    }
    if (!m.has_reference) {
        std::snprintf(buf, sizeof(buf),
                      "MIC CHECK   reference: %s (waiting for a first check)",
                      kind);
    } else if (shown < total) {
        std::snprintf(buf, sizeof(buf),
                      "MIC CHECK   reference: %s  %.1f LUFS   showing %d of %d",
                      kind, m.reference_lufs, static_cast<int>(shown),
                      static_cast<int>(total));
    } else {
        std::snprintf(buf, sizeof(buf),
                      "MIC CHECK   reference: %s  %.1f LUFS",
                      kind, m.reference_lufs);
    }
    return std::string(buf);
}

// ── OBS callbacks ───────────────────────────────────────────────────────────

static const char *meter_get_name(void *)
{
    return obs_module_text("CoreVideoLoudnessMeter.Name");
}

static void meter_apply_settings(loudness_meter_source *ctx,
                                 obs_data_t *settings)
{
    uint32_t w = static_cast<uint32_t>(obs_data_get_int(settings, PROP_WIDTH));
    uint32_t h = static_cast<uint32_t>(obs_data_get_int(settings, PROP_HEIGHT));
    if (w < 160)  w = 160;
    if (w > 3840) w = 3840;
    if (h < 90)   h = 90;
    if (h > 2160) h = 2160;
    ctx->canvas_width.store(w, std::memory_order_release);
    ctx->canvas_height.store(h, std::memory_order_release);
    ctx->reference.store(static_cast<int>(
                             obs_data_get_int(settings, PROP_REFERENCE)),
                         std::memory_order_release);
    double tol = obs_data_get_double(settings, PROP_TOLERANCE);
    if (!(tol > 0.0)) tol = kLoudnessBoardDefaultToleranceLu;
    if (tol > 12.0) tol = 12.0;
    ctx->tolerance_milli_lu.store(static_cast<int>(tol * 1000.0 + 0.5),
                                  std::memory_order_release);
}

static void *meter_create(obs_data_t *settings, obs_source_t *source)
{
    auto *ctx = new loudness_meter_source();
    ctx->source = source;
    meter_apply_settings(ctx, settings);

    char private_name[64];
    for (size_t i = 0; i < kMeterMaxRows; ++i) {
        std::snprintf(private_name, sizeof(private_name),
                      "corevideo_meter_name_%d", static_cast<int>(i));
        ctx->rows[i].name = make_text_child(private_name, 20, 0xFFF2F5F8u);
        std::snprintf(private_name, sizeof(private_name),
                      "corevideo_meter_value_%d", static_cast<int>(i));
        ctx->rows[i].value = make_text_child(private_name, 20, 0xFFF2F5F8u);
    }
    return ctx;
}

static void meter_destroy(void *data)
{
    auto *ctx = static_cast<loudness_meter_source *>(data);
    for (size_t i = 0; i < kMeterMaxRows; ++i) {
        if (ctx->rows[i].name)  obs_source_release(ctx->rows[i].name);
        if (ctx->rows[i].value) obs_source_release(ctx->rows[i].value);
    }
    delete ctx;
}

static void meter_update(void *data, obs_data_t *settings)
{
    meter_apply_settings(static_cast<loudness_meter_source *>(data), settings);
}

static uint32_t meter_get_width(void *data)
{
    auto *ctx = static_cast<loudness_meter_source *>(data);
    return ctx->canvas_width.load(std::memory_order_acquire);
}

static uint32_t meter_get_height(void *data)
{
    auto *ctx = static_cast<loudness_meter_source *>(data);
    return ctx->canvas_height.load(std::memory_order_acquire);
}

static void meter_enum_active_sources(void *data,
                                      obs_source_enum_proc_t enum_callback,
                                      void *param)
{
    auto *ctx = static_cast<loudness_meter_source *>(data);
    for (size_t i = 0; i < kMeterMaxRows; ++i) {
        if (ctx->rows[i].name)  enum_callback(ctx->source, ctx->rows[i].name, param);
        if (ctx->rows[i].value) enum_callback(ctx->source, ctx->rows[i].value, param);
    }
}

// The model is rebuilt at 10 Hz, not per frame. corevideo_loudness_readings()
// takes g_sources_mtx and every source's own mutex -- the same mutex the
// audio lane holds for a whole drain -- so asking it 60 times a second would
// put the graphics thread in contention with the media path for no visible
// gain: the numbers it reports move on a 100 ms hop anyway.
static void meter_video_tick(void *data, float seconds)
{
    auto *ctx = static_cast<loudness_meter_source *>(data);
    ctx->rebuild_accum += seconds;
    if (ctx->rebuild_accum < 0.1f) return;
    ctx->rebuild_accum = 0.0f;

    const auto readings = corevideo_loudness_readings();
    const double tol =
        static_cast<double>(ctx->tolerance_milli_lu.load(
            std::memory_order_acquire)) / 1000.0;
    const auto kind = static_cast<LoudnessReference>(
        ctx->reference.load(std::memory_order_acquire));
    LoudnessBoardModel model = loudness_board_build(
        readings, kind, tol, kLoudnessBoardMinBlocks);

    std::lock_guard<std::mutex> lk(ctx->mutex);
    ctx->model = std::move(model);
}

static void meter_video_render(void *data, gs_effect_t *)
{
    auto *ctx = static_cast<loudness_meter_source *>(data);
    const int canvas_w =
        static_cast<int>(ctx->canvas_width.load(std::memory_order_acquire));
    const int canvas_h =
        static_cast<int>(ctx->canvas_height.load(std::memory_order_acquire));
    if (!s_meter_effect.valid()) return;

    LoudnessBoardModel model;
    {
        std::lock_guard<std::mutex> lk(ctx->mutex);
        model = ctx->model;
    }

    meter_fill_rect(LoudnessBoardRect{0, 0, canvas_w, canvas_h}, kMeterBgArgb);
    meter_fill_rect(LoudnessBoardRect{0, 0, canvas_w, kLoudnessBoardHeaderPx},
                    kMeterHeaderArgb);

    // The LAST slot is permanently the header's, never a panelist's, so a
    // change in row count cannot silently steal the header's text child --
    // hence the cap is kMeterMaxRows - 1 and not kMeterMaxRows.
    static constexpr size_t kMeterHeaderSlot = kMeterMaxRows - 1;
    const size_t total = model.rows.size();
    size_t shown = loudness_board_visible_rows(canvas_h, total);
    if (shown > kMeterHeaderSlot) shown = kMeterHeaderSlot;

    for (size_t i = 0; i < shown; ++i) {
        const LoudnessBoardRow &row = model.rows[i];
        const LoudnessBoardRect band =
            loudness_board_row_rect(canvas_w, canvas_h, shown, i);
        if (band.w <= 0 || band.h <= 0) continue;

        meter_fill_rect(band, kMeterRowArgb);

        // The status chip: a fat block at the left edge, which is the part
        // that reads first from across a room.
        meter_fill_rect(LoudnessBoardRect{band.x, band.y, 8, band.h},
                        status_color(row.status));

        // The zero line, drawn under the bar so a bar of zero width still
        // shows where the reference is.
        const LoudnessBoardRect zero =
            loudness_board_bar_rect(band, 0.0, kLoudnessBoardFullScaleLu);
        meter_fill_rect(LoudnessBoardRect{zero.x - 1, band.y, 2, band.h},
                        kMeterCentreArgb);

        if (row.has_deviation) {
            const LoudnessBoardRect bar = loudness_board_bar_rect(
                band, row.deviation_lu, kLoudnessBoardFullScaleLu);
            meter_fill_rect(LoudnessBoardRect{bar.x, bar.y + 4, bar.w,
                                              bar.h > 8 ? bar.h - 8 : bar.h},
                            status_color(row.status));
        }
    }

    // Labels last, over the bars. Each child is only re-settings-updated when
    // its string changes: obs_source_update() allocates and takes the source's
    // own lock, and doing it per frame per row is the churn shape this project
    // already has a live incident about.
    const std::string head = header_text(model, shown, total);
    {
        std::lock_guard<std::mutex> lk(ctx->mutex);
        if (ctx->applied_signature != model.signature) {
            ctx->applied_signature = model.signature;
            for (size_t i = 0; i < kMeterHeaderSlot; ++i) {
                const std::string name_text =
                    (i < shown) ? model.rows[i].name : std::string();
                const std::string value_text =
                    (i < shown) ? row_value_text(model.rows[i]) : std::string();
                if (ctx->rows[i].name_text != name_text) {
                    ctx->rows[i].name_text = name_text;
                    set_text_child(ctx->rows[i].name, name_text.c_str());
                }
                if (ctx->rows[i].value_text != value_text) {
                    ctx->rows[i].value_text = value_text;
                    set_text_child(ctx->rows[i].value, value_text.c_str());
                }
            }
        }
    }

    for (size_t i = 0; i < shown; ++i) {
        const LoudnessBoardRect band =
            loudness_board_row_rect(canvas_w, canvas_h, shown, i);
        if (band.w <= 0 || band.h <= 0) continue;
        const int text_y = band.y + (band.h > 24 ? (band.h - 24) / 2 : 0);
        if (ctx->rows[i].name) {
            gs_matrix_push();
            gs_matrix_translate3f(static_cast<float>(band.x + 16),
                                  static_cast<float>(text_y), 0.0f);
            obs_source_video_render(ctx->rows[i].name);
            gs_matrix_pop();
        }
        if (ctx->rows[i].value) {
            gs_matrix_push();
            gs_matrix_translate3f(static_cast<float>(band.x + band.w / 2 + 8),
                                  static_cast<float>(text_y), 0.0f);
            obs_source_video_render(ctx->rows[i].value);
            gs_matrix_pop();
        }
    }

    // The header, in the slot reserved for it above. Updated on its own
    // string comparison rather than on the board signature, because the
    // "showing N of M" count changes with the CANVAS as well as the panel.
    if (ctx->rows[kMeterHeaderSlot].name) {
        obs_source_t *header = ctx->rows[kMeterHeaderSlot].name;
        {
            std::lock_guard<std::mutex> lk(ctx->mutex);
            if (ctx->rows[kMeterHeaderSlot].name_text != head) {
                ctx->rows[kMeterHeaderSlot].name_text = head;
                set_text_child(header, head.c_str());
            }
        }
        gs_matrix_push();
        gs_matrix_translate3f(12.0f, 4.0f, 0.0f);
        obs_source_video_render(header);
        gs_matrix_pop();
    }
}

static bool meter_reset_clicked(obs_properties_t *, obs_property_t *, void *)
{
    corevideo_reset_loudness_windows();
    return false;
}

static obs_properties_t *meter_get_properties(void *)
{
    obs_properties_t *props = obs_properties_create();

    obs_property_t *ref = obs_properties_add_list(
        props, PROP_REFERENCE,
        obs_module_text("CoreVideoLoudnessMeter.Reference"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(
        ref, obs_module_text("CoreVideoLoudnessMeter.Reference.PanelMedian"),
        static_cast<int>(LoudnessReference::PanelMedian));
    obs_property_list_add_int(
        ref, obs_module_text("CoreVideoLoudnessMeter.Reference.R128"),
        static_cast<int>(LoudnessReference::EbuR128));
    obs_property_list_add_int(
        ref, obs_module_text("CoreVideoLoudnessMeter.Reference.A85"),
        static_cast<int>(LoudnessReference::AtscA85));
    obs_property_list_add_int(
        ref, obs_module_text("CoreVideoLoudnessMeter.Reference.Streaming"),
        static_cast<int>(LoudnessReference::Streaming));

    obs_properties_add_float_slider(
        props, PROP_TOLERANCE,
        obs_module_text("CoreVideoLoudnessMeter.Tolerance"), 0.5, 6.0, 0.5);
    obs_properties_add_int(props, PROP_WIDTH,
        obs_module_text("CoreVideoLoudnessMeter.Width"), 160, 3840, 10);
    obs_properties_add_int(props, PROP_HEIGHT,
        obs_module_text("CoreVideoLoudnessMeter.Height"), 90, 2160, 10);
    obs_properties_add_button(props, PROP_RESET,
        obs_module_text("CoreVideoLoudnessMeter.Reset"), meter_reset_clicked);
    return props;
}

static void meter_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, PROP_REFERENCE,
                             static_cast<int>(LoudnessReference::PanelMedian));
    obs_data_set_default_double(settings, PROP_TOLERANCE,
                                kLoudnessBoardDefaultToleranceLu);
    obs_data_set_default_int(settings, PROP_WIDTH, 640);
    obs_data_set_default_int(settings, PROP_HEIGHT, 360);
}

void corevideo_loudness_meter_source_register()
{
    obs_source_info info = {};
    info.id           = kMeterSourceId;
    info.type         = OBS_SOURCE_TYPE_INPUT;
    // CUSTOM_DRAW because it binds the plugin's own effect rather than
    // letting OBS draw one texture with the default one, exactly as the Tiles
    // wall does.
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
                        OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name     = meter_get_name;
    info.create       = meter_create;
    info.destroy      = meter_destroy;
    info.update       = meter_update;
    info.video_tick   = meter_video_tick;
    info.video_render = meter_video_render;
    info.get_width    = meter_get_width;
    info.get_height   = meter_get_height;
    info.enum_active_sources = meter_enum_active_sources;
    info.get_properties = meter_get_properties;
    info.get_defaults   = meter_get_defaults;
    obs_register_source(&info);
}

void corevideo_loudness_meter_load_gfx()
{
    tiles_effect_load(s_meter_effect);
}

void corevideo_loudness_meter_unload_gfx()
{
    tiles_effect_destroy(s_meter_effect);
    s_meter_pass_failed_logged = false;
}
```

- [ ] **Step 7: Wire it into the build and the module**

In `CMakeLists.txt`, in the `add_library(obs-zoom-plugin MODULE ...)` source list, add after `src/zoom-supersource.cpp`:

```cmake
        src/zoom-loudness-meter-source.cpp
```

In `src/plugin-main.cpp`, add the include beside the other source headers:

```cpp
#include "zoom-loudness-meter-source.h"
```

After `zoom_supersource_load_gfx();` add:

```cpp
    corevideo_loudness_meter_source_register();
    corevideo_loudness_meter_load_gfx();
```

After `zoom_supersource_unload_gfx();` in the unload path add:

```cpp
    corevideo_loudness_meter_unload_gfx();
```

In `data/locale/en-US.ini`, after the `CoreVideoAudienceAudio.Name` line, add:

```ini
CoreVideoLoudnessMeter.Name="CoreVideo Loudness Meter (readiness board)"
CoreVideoLoudnessMeter.Reference="Reference"
CoreVideoLoudnessMeter.Reference.PanelMedian="Panel median (recommended)"
CoreVideoLoudnessMeter.Reference.R128="EBU R128 (-23 LUFS)"
CoreVideoLoudnessMeter.Reference.A85="ATSC A/85 (-24 LKFS)"
CoreVideoLoudnessMeter.Reference.Streaming="Streaming (-16 LUFS)"
CoreVideoLoudnessMeter.Tolerance="Pass tolerance (LU)"
CoreVideoLoudnessMeter.Width="Width"
CoreVideoLoudnessMeter.Height="Height"
CoreVideoLoudnessMeter.Reset="Reset all mic-check windows"
```

- [ ] **Step 8: Build and run the whole suite**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: the plugin links and every test is green (N/N).

- [ ] **Step 9: Look at it in real OBS**

Install the plugin DLL (OBS closed, elevated — and if the engine changed, both binaries as a pair), then in OBS add a **CoreVideo Loudness Meter** source to a scene alongside two or more **CoreVideo Participant Audio** sources in a live meeting. Confirm, in order:

1. Rows appear with the panelists' names, in alphabetical order, and do not reshuffle as the roster ticks.
2. With nobody speaking, every row reads `no audio` and the header says `waiting for a first check`.
3. While one panelist speaks for ~20 s, their row moves `no audio` → `measuring` → a verdict, and the bar grows from the centre line.
4. The header's reference value appears once at least one panelist has a check.
5. `Reset all mic-check windows` clears every verdict back to `no audio`.
6. The OBS log carries one `labels will use text source '...'` line, not a per-frame stream of anything.

- [ ] **Step 10: Commit**

```bash
git add src/zoom-loudness-meter-source.h src/zoom-loudness-meter-source.cpp \
        src/loudness-board.h tests/loudness-board-test.cpp \
        CMakeLists.txt src/plugin-main.cpp data/locale/en-US.ini
git commit -m "feat(loudness): readiness-board meter source drawing deviation bars with the Solid technique"
```

---

### Task 7: Document the invariants in CLAUDE.md

**Files:**
- Modify: `CLAUDE.md`

This repo's standing directive is that docs-updated is part of done, and every invariant that has cost a live defect is listed in `CLAUDE.md`'s invariant map. Two of this feature's rules belong there.

- [ ] **Step 1: Add the entry**

In `CLAUDE.md`, in the "Invariants that have each caused a live-show defect" list, after the **Silence-resume fade** bullet, add:

```markdown
- **Loudness coefficients follow the RUNTIME sample rate**
  (`src/audio-loudness.h`, fed from `output_audio_frame()` in
  `src/zoom-participant-audio-source.cpp`): BS.1770-4 publishes its two
  K-weighting biquads' coefficients for 48 kHz and for no other rate, and
  this plugin has no guaranteed rate — `engine/src/engine-audio.cpp` calls
  `GetSampleRate()` per buffer and stamps the answer into
  `ShmAudioHeader::sample_rate`, and Zoom commonly delivers 32 kHz. The
  coefficients are therefore DERIVED from the analog prototype at the
  runtime rate; at 48 kHz that derivation reproduces the published table to
  fourteen digits, which is what `CoreVideoAudioLoudness` asserts. Pinned at
  48 kHz and fed 32 kHz, a 1 kHz tone whose true value is −19.98 LUFS reads
  −18.66: 1.3 LU wrong, on a meter whose whole product claim is that a 6 LU
  spread between panelists is visible, and with nothing in the number to say
  it is wrong. **Metering runs inside the drain loop**, on the audio lane
  thread, never per wakeup — a media event is a coalescing prompt, not a
  payload, so measuring "the buffer that woke us" reads low by a
  load-dependent amount that looks fine on an idle box. Chunk invariance
  (10 ms pieces == one buffer, to 1e-9) is asserted for exactly that reason.
  **Integrated loudness is scoped to a resettable per-panelist window**, not
  the session: 4 s of −20 LUFS speech inside 20 s reads −27.08 ungated and
  −20.16 with BS.1770's absolute −70 LUFS gate, which is precisely the
  mechanism a panel needs, since a panelist is silent roughly 80% of a
  panel. The board's reference is the panel **MEDIAN**, never the mean —
  one laptop mic at −35 LUFS would otherwise drag the reference far enough
  to fail everyone else.
```

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: record the loudness engine's runtime-rate, drain-loop and gating invariants"
```

---

## Self-Review

**Spec coverage (Subsystem 1 only):**

| Spec requirement | Task |
|---|---|
| Filter chain: high-shelf head + RLB high-pass | 1, 2 |
| Sample rate is a runtime variable; derive coefficients | 1, 2 (test proves 32 k ≠ 48 k and the 1.3 LU error) |
| Input is 16-bit signed interleaved; channels runtime-discovered | 2, 5 |
| `L = -0.691 + 10 log10(Σ G_i z_i)`, G = 1.0 for L/R/C | 2 |
| Momentary 400 ms | 2 |
| Short-term 3 s | 2 |
| Integrated, absolute −70 then relative −10 LU, 400 ms blocks at 100 ms hop | 3 |
| Integrated scoped to a resettable check window | 3, 5 |
| Deviation from the panel **median** as the headline | 4 |
| Presets −23 / −24 / −16, default panel median | 4 |
| Meter is a readiness board, one row per panelist, pass/fail | 4, 6 |
| Tap at the decoded PCM in `zoom-participant-audio-source.cpp` | 5 |
| Audio lane thread, integrate over the whole drain loop | 5 (and pinned by chunk invariance in 3) |
| `roster()` names cached, never per frame; `add_roster_callback` | 5 |
| `g_sources_mtx` registry mirrored; lock order preserved | 5 |
| OBS rendering source, reuse the `Solid` technique | 6 |
| Header-only pure logic + hand-registered plain-`main()` tests | 1, 3, 4 |
| True peak deferred to v2 | not implemented — correct, it is explicitly deferred by the spec |

Out of scope and deliberately absent: the face detector, Tiles auto-framing, the return overlay, and self-tile exclusion (a Subsystem 3 deployment constraint).

**Placeholder scan:** no TBD, no "add error handling", no "similar to Task N". Every code step carries complete C++; every expected numeric value is stated concretely (−3.01, −23.01, −20.00, −19.98, −18.66, −27.08, −22.96, −20.16, −20.06, coefficient tables at 32 k and 48 k, 79 px rows, x=480/w=80 bars).

**Type consistency:** `LoudnessMeter`, `LoudnessBiquadCoeffs`, `LoudnessBiquadState`, `LoudnessReading`, `LoudnessBoardRow`, `LoudnessBoardModel`, `LoudnessBoardRect`, `LoudnessReference`, `LoudnessRowStatus` are each defined once and referenced with the same names and members throughout. `loudness_meter_on_hop_complete()` is forward-declared in Task 2, given a placeholder body in Task 2 that Task 3 explicitly deletes before defining it properly — the only forward reference in the plan, and it is called out at both ends.
