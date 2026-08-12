# CoreVideo Tiles — Phase 0 + Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Answer whether Zoom SDK media timestamps share a cross-participant timebase (Phase 0), then build the pure tile-grid solver and a working `CoreVideo Tiles` OBS source that renders assigned participants as identical, evenly-spaced tiles (Phase 1).

**Architecture:** Phase 0 adds a dependency-free analyzer (`tile-clock-probe`) that consumes `(feedId, mediaPts, arrivalNs)` samples and reports a timebase verdict, plus env-gated instrumentation at the four ingest call sites that currently discard `GetTimeStamp()`. Phase 1 adds `zoom-tile-grid` — a pure function with no Qt, OBS, or SDK dependency — and a `CoreVideo Tiles` async-video OBS source that composites assigned feeds using it.

**Tech Stack:** C++17, CMake, OBS Studio plugin API, Zoom Meeting SDK raw data interfaces. Tests are plain `main()` executables returning 0/1 — **no test framework in this repo**; do not introduce gtest or Catch.

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-08-09-corevideo-tiles-design.md`. Read it before starting.
- **Pure units must be Qt-free and OBS-free.** `zoom-tile-grid` and `tile-clock-probe` use `std::vector`, not `QVector`. The spec's architecture table says `QVector<Rect>`; that is superseded here, because the test configuration builds with plugin/engine/sidecar OFF and Qt unavailable.
- **Tile aspect is 16:9, fixed in v1.** Gutter and margin are solver parameters but not operator controls in v1.
- **No borders on tiles.** This violates the existing program/preview no-borders rule.
- **Log prefix:** `[obs-zoom-plugin]` — match the existing `blog()` convention.
- **Test build loop** (verified working, 2/2 passing at time of writing):
  ```sh
  cmake -S . -B build-tests -DCOREVIDEO_BUILD_PLUGIN=OFF -DCOREVIDEO_BUILD_ENGINE=OFF -DCOREVIDEO_BUILD_SIDECAR=OFF -DBUILD_TESTING=ON
  cmake --build build-tests --config Debug
  ctest --test-dir build-tests -C Debug
  ```
- **Pre-existing uncommitted changes** are in the working tree (`CMakeLists.txt`, `tests/output-health-test.cpp`, untracked `tests/speaker-director-test.cpp`) from unrelated in-flight work. Do not commit them. Stage only files you touch, by explicit path — never `git add -A`.

---

## File Structure

| File | Responsibility | Phase |
|---|---|---|
| `src/tile-clock-probe.h` / `.cpp` | Pure analyzer: sample series → per-feed offset stats → timebase verdict | 0 |
| `tests/tile-clock-probe-test.cpp` | Synthetic-sample tests for the analyzer | 0 |
| `engine/src/tile-clock-log.h` | Env-gated probe emission via EngineIpc debug messages | 0 |
| `engine/src/engine-video.cpp` (modify) | Probe call in `onRawDataFrameReceived` | 0 |
| `engine/src/engine-audio.cpp` (modify) | Probe calls in audio raw-data callbacks | 0 |
| `docs/tile-clock-findings.md` | Rig-run verdict; gates Phase 2 | 0 |
| `src/zoom-tile-grid.h` / `.cpp` | Pure grid solver | 1 |
| `tests/tile-grid-test.cpp` | Solver unit + property tests | 1 |
| `src/zoom-supersource.h` / `.cpp` | `CoreVideo Tiles` OBS source; assignment list; composite | 1 |
| `src/plugin-main.cpp` (modify) | Register the new source | 1 |
| `CMakeLists.txt` (modify) | Register test executables; add sources to plugin target | 0, 1 |

---

# Phase 0 — Timestamp Spike (GATE)

**This phase gates Phase 2.** Its deliverable is a documented verdict, not a feature. If timestamps prove to be sender-clock rather than receiver-normalized, the Phase 2 sync design needs per-feed offset estimation and must be re-planned before any sync code is written.

---

### Task 1: Timebase analyzer

**Files:**
- Create: `src/tile-clock-probe.h`, `src/tile-clock-probe.cpp`
- Test: `tests/tile-clock-probe-test.cpp`
- Modify: `CMakeLists.txt` (inside the existing `if(BUILD_TESTING)` block, after the `CoreVideoSpeakerDirectorTest` block, before `endif()`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```cpp
  struct ClockSample { uint32_t feed_id; uint64_t media_pts_us; uint64_t arrival_ns; };
  enum class TimebaseVerdict { Insufficient, Shared, PerFeed };
  struct FeedOffsetStats { uint32_t feed_id; std::size_t sample_count;
                           int64_t min_offset_us; int64_t median_offset_us; int64_t spread_us; };
  struct TimebaseReport { TimebaseVerdict verdict; std::vector<FeedOffsetStats> feeds;
                          int64_t cross_feed_spread_us; };
  TimebaseReport analyze_clock_samples(const std::vector<ClockSample> &samples);
  const char *timebase_verdict_id(TimebaseVerdict v);
  ```

**Method.** Offset for a sample is `arrival_ns / 1000 − media_pts_us`. Network delay is always positive and variable, so the *minimum* observed offset per feed is the best estimate of that feed's clock relationship (minimum-observed-delay filter). If all feeds share a timebase their minimum offsets cluster; if each feed carries its own sender clock they scatter arbitrarily. `cross_feed_spread_us` is `max(min_offset) − min(min_offset)` across feeds. Verdict: `Insufficient` if fewer than 2 feeds or any feed has fewer than 30 samples; `Shared` if `cross_feed_spread_us <= 50000` (50 ms); otherwise `PerFeed`.

The 50 ms threshold is deliberately generous — it only needs to separate "same clock plus network jitter" from "unrelated clocks," which typically differ by seconds or more.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/tile-clock-probe-test.cpp
#include "tile-clock-probe.h"

#include <iostream>
#include <vector>

static std::vector<ClockSample> series(uint32_t feed_id, int64_t offset_us,
                                       std::size_t n, int64_t jitter_us = 0)
{
    std::vector<ClockSample> out;
    for (std::size_t i = 0; i < n; ++i) {
        const uint64_t pts = 1000000ull + i * 33333ull;
        // Jitter is additive-only: real network delay never runs early.
        const int64_t extra = jitter_us ? static_cast<int64_t>((i * 7919) % jitter_us) : 0;
        ClockSample s{};
        s.feed_id = feed_id;
        s.media_pts_us = pts;
        s.arrival_ns = static_cast<uint64_t>(
            (static_cast<int64_t>(pts) + offset_us + extra) * 1000);
        out.push_back(s);
    }
    return out;
}

static bool expect_verdict(const char *name, const std::vector<ClockSample> &samples,
                           TimebaseVerdict expected)
{
    const TimebaseReport r = analyze_clock_samples(samples);
    if (r.verdict != expected) {
        std::cerr << name << ": expected " << timebase_verdict_id(expected)
                  << ", got " << timebase_verdict_id(r.verdict) << "\n";
        return false;
    }
    return true;
}

int main()
{
    // Two feeds, same clock, only network jitter between them.
    std::vector<ClockSample> shared = series(1, 20000, 60, 4000);
    for (const ClockSample &s : series(2, 23000, 60, 4000)) shared.push_back(s);
    if (!expect_verdict("shared timebase", shared, TimebaseVerdict::Shared))
        return 1;

    // Two feeds whose clocks are seconds apart -> independent sender clocks.
    std::vector<ClockSample> per_feed = series(1, 20000, 60, 4000);
    for (const ClockSample &s : series(2, 5000000, 60, 4000)) per_feed.push_back(s);
    if (!expect_verdict("per-feed timebase", per_feed, TimebaseVerdict::PerFeed))
        return 1;

    // A single feed can never answer the cross-feed question.
    if (!expect_verdict("single feed", series(1, 20000, 60), TimebaseVerdict::Insufficient))
        return 1;

    // Too few samples to trust the minimum filter.
    std::vector<ClockSample> sparse = series(1, 20000, 5);
    for (const ClockSample &s : series(2, 21000, 5)) sparse.push_back(s);
    if (!expect_verdict("sparse", sparse, TimebaseVerdict::Insufficient))
        return 1;

    // Minimum-offset estimate must survive additive jitter.
    const TimebaseReport r = analyze_clock_samples(series(1, 20000, 60, 4000));
    if (r.feeds.size() != 1) return 1;
    if (r.feeds[0].min_offset_us < 19000 || r.feeds[0].min_offset_us > 21000) {
        std::cerr << "min offset estimate drifted: " << r.feeds[0].min_offset_us << "\n";
        return 1;
    }

    std::cout << "tile-clock-probe: all tests passed\n";
    return 0;
}
```

- [ ] **Step 2: Register the test and run it to verify it fails**

Add inside the existing `if(BUILD_TESTING)` block in `CMakeLists.txt`:

```cmake
    add_executable(CoreVideoTileClockProbeTest
        tests/tile-clock-probe-test.cpp
        src/tile-clock-probe.cpp
    )
    target_include_directories(CoreVideoTileClockProbeTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileClockProbe
             COMMAND CoreVideoTileClockProbeTest)
```

Run:
```sh
cmake -S . -B build-tests -DCOREVIDEO_BUILD_PLUGIN=OFF -DCOREVIDEO_BUILD_ENGINE=OFF -DCOREVIDEO_BUILD_SIDECAR=OFF -DBUILD_TESTING=ON
cmake --build build-tests --config Debug
```
Expected: FAIL — `tile-clock-probe.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
// src/tile-clock-probe.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// One observation of a media frame or audio buffer arriving from the Zoom SDK.
struct ClockSample {
    uint32_t feed_id      = 0;   // Zoom participant/user id
    uint64_t media_pts_us = 0;   // YUVRawDataI420/AudioRawData GetTimeStamp()
    uint64_t arrival_ns   = 0;   // os_gettime_ns() at the ingest callback
};

enum class TimebaseVerdict {
    Insufficient,  // fewer than 2 feeds, or too few samples to judge
    Shared,        // feeds appear normalized to a common clock
    PerFeed,       // feeds carry independent sender clocks
};

struct FeedOffsetStats {
    uint32_t    feed_id           = 0;
    std::size_t sample_count      = 0;
    int64_t     min_offset_us     = 0;
    int64_t     median_offset_us  = 0;
    int64_t     spread_us         = 0;  // max - min within this feed
};

struct TimebaseReport {
    TimebaseVerdict              verdict = TimebaseVerdict::Insufficient;
    std::vector<FeedOffsetStats> feeds;
    int64_t                      cross_feed_spread_us = 0;
};

// Minimum samples per feed before its min-offset estimate is trusted.
constexpr std::size_t kMinSamplesPerFeed = 30;

// Feeds whose minimum offsets agree within this bound are treated as sharing
// a timebase. Generous on purpose: it need only separate "same clock plus
// network jitter" from "unrelated clocks", which differ by seconds.
constexpr int64_t kSharedTimebaseToleranceUs = 50000;

TimebaseReport analyze_clock_samples(const std::vector<ClockSample> &samples);
const char *timebase_verdict_id(TimebaseVerdict v);
```

- [ ] **Step 4: Write the implementation**

```cpp
// src/tile-clock-probe.cpp
#include "tile-clock-probe.h"

#include <algorithm>
#include <map>

const char *timebase_verdict_id(TimebaseVerdict v)
{
    switch (v) {
    case TimebaseVerdict::Shared:  return "shared";
    case TimebaseVerdict::PerFeed: return "per-feed";
    default:                       return "insufficient";
    }
}

TimebaseReport analyze_clock_samples(const std::vector<ClockSample> &samples)
{
    TimebaseReport report;

    std::map<uint32_t, std::vector<int64_t>> by_feed;
    for (const ClockSample &s : samples) {
        const int64_t offset_us =
            static_cast<int64_t>(s.arrival_ns / 1000) - static_cast<int64_t>(s.media_pts_us);
        by_feed[s.feed_id].push_back(offset_us);
    }

    bool all_feeds_sufficient = true;
    for (auto &entry : by_feed) {
        std::vector<int64_t> &offsets = entry.second;
        std::sort(offsets.begin(), offsets.end());

        FeedOffsetStats stats;
        stats.feed_id          = entry.first;
        stats.sample_count     = offsets.size();
        stats.min_offset_us    = offsets.front();
        stats.median_offset_us = offsets[offsets.size() / 2];
        stats.spread_us        = offsets.back() - offsets.front();
        report.feeds.push_back(stats);

        if (offsets.size() < kMinSamplesPerFeed)
            all_feeds_sufficient = false;
    }

    if (report.feeds.size() < 2 || !all_feeds_sufficient) {
        report.verdict = TimebaseVerdict::Insufficient;
        return report;
    }

    int64_t lowest  = report.feeds.front().min_offset_us;
    int64_t highest = report.feeds.front().min_offset_us;
    for (const FeedOffsetStats &f : report.feeds) {
        lowest  = std::min(lowest, f.min_offset_us);
        highest = std::max(highest, f.min_offset_us);
    }
    report.cross_feed_spread_us = highest - lowest;
    report.verdict = report.cross_feed_spread_us <= kSharedTimebaseToleranceUs
                         ? TimebaseVerdict::Shared
                         : TimebaseVerdict::PerFeed;
    return report;
}
```

- [ ] **Step 5: Run tests to verify they pass**

```sh
cmake --build build-tests --config Debug
ctest --test-dir build-tests -C Debug
```
Expected: PASS, 3/3 tests (the two pre-existing plus `CoreVideoTileClockProbe`).

- [ ] **Step 6: Commit**

```bash
git add src/tile-clock-probe.h src/tile-clock-probe.cpp tests/tile-clock-probe-test.cpp CMakeLists.txt
git commit -m "feat(tiles): add cross-participant timebase analyzer"
```

---

### Task 2: Env-gated ingest instrumentation (engine-side)

> **Corrected 2026-08-09.** The original task targeted `src/zoom-video-delegate.cpp`
> and `src/zoom-audio-delegate.cpp`. Those files are orphaned dead code — no CMake
> target compiles them. The real SDK raw-data boundary is in the `ZoomObsEngine`
> process, and that is where the probe goes. Engine debug messages reach the OBS
> log via `EngineIpc::write({"cmd":"debug",...})` →
> `src/zoom-engine-client.cpp:553 blog(...)`, so Task 3 still reads one OBS log.

**Files:**
- Create: `engine/src/tile-clock-log.h`
- Modify: `engine/src/engine-video.cpp` (in `ParticipantSubscription::onRawDataFrameReceived`, `engine/src/engine-video.cpp:175`)
- Modify: `engine/src/engine-audio.cpp` (in `onOneWayAudioRawDataReceived`, `engine/src/engine-audio.cpp:221`, and `onMixedAudioRawDataReceived`, `engine/src/engine-audio.cpp:207`)

**Interfaces:**
- Consumes: `EngineIpc::write()` from `engine/src/engine-writer.h`.
- Produces: `void tile_clock_log(uint32_t feed_id, uint64_t media_pts_us, uint64_t arrival_ns, const char *kind);` (engine-side)

**Why an env var, not a settings toggle.** Log-verbosity gating is being reworked on the in-flight `fix-log-verbosity` branch, and the probe should disappear entirely once the spike is answered. The engine inherits the environment of the plugin process that spawns it, so setting the variable before launching OBS reaches the engine.

**Important:** this task only *observes*. Do not change any SHM header, any timestamp the plugin assigns, or any frame handling. Carrying PTS across SHM is Phase 2 work and must not happen before the verdict is in.

- [ ] **Step 1: Write the engine-side logging header**

```cpp
// engine/src/tile-clock-log.h
#pragma once

#include "engine-writer.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

// Emits one debug IPC message per observed frame/buffer when
// COREVIDEO_TILE_CLOCK_PROBE=1 in the engine's environment (inherited from
// OBS). The plugin relays debug messages into the OBS log, where
// tools/tile-clock-analyze.py picks them up.
// Payload format inside the message: TILECLOCK,<kind>,<feed_id>,<media_pts_us>,<arrival_ns>
// Off by default and cheap when off: the env var is read once.
inline void tile_clock_log(uint32_t feed_id, uint64_t media_pts_us,
                           uint64_t arrival_ns, const char *kind)
{
    static const bool enabled = [] {
        const char *v = std::getenv("COREVIDEO_TILE_CLOCK_PROBE");
        return v && v[0] == '1';
    }();
    if (!enabled) return;

    EngineIpc::write(std::string(R"({"cmd":"debug","stage":"tile_clock","msg":"TILECLOCK,)") +
                     kind + "," + std::to_string(feed_id) + "," +
                     std::to_string(media_pts_us) + "," +
                     std::to_string(arrival_ns) + "\"}");
}

// Monotonic arrival time in nanoseconds, engine-process clock. All feeds are
// compared against this same clock, which is all the analyzer needs.
inline uint64_t tile_clock_now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
```

- [ ] **Step 2: Instrument the video callback**

In `engine/src/engine-video.cpp`, add `#include "tile-clock-log.h"` near the existing includes. At the top of `ParticipantSubscription::onRawDataFrameReceived` (`engine/src/engine-video.cpp:175`), immediately after the function's opening validity handling (before the frame is copied to SHM), add:

```cpp
    tile_clock_log(m_user_id, data->GetTimeStamp(), tile_clock_now_ns(), "v");
```

Use the member/variable that actually holds the subscription's participant id at that site (read the surrounding code; do not rename anything). The raw-data pointer parameter is `data` per the signature at `engine/src/engine-video.cpp:175`.

- [ ] **Step 3: Instrument the audio callbacks**

In `engine/src/engine-audio.cpp`, add `#include "tile-clock-log.h"`. At the top of `onOneWayAudioRawDataReceived` (`engine/src/engine-audio.cpp:221`) add:

```cpp
    tile_clock_log(user_id, data->GetTimeStamp(), tile_clock_now_ns(), "a");
```

At the top of `onMixedAudioRawDataReceived` (`engine/src/engine-audio.cpp:207`) add the same call with feed id `0` — the mixed stream legitimately has no single participant:

```cpp
    tile_clock_log(0, data->GetTimeStamp(), tile_clock_now_ns(), "a");
```

- [ ] **Step 4: Build the engine to verify it compiles**

```powershell
cmake -S . -B build-engine -G "Visual Studio 17 2022" -A x64 `
  -DCOREVIDEO_BUILD_PLUGIN=OFF -DCOREVIDEO_BUILD_SIDECAR=OFF `
  -DZOOM_SDK_DIR="C:\Users\walla\Downloads\zoom-sdk-windows-7.1.5.43953\zoom-sdk-windows-7.1.5.43953\x64"
cmake --build build-engine --config Release --target ZoomObsEngine --parallel
```
Expected: builds clean. The engine target needs only the Zoom SDK — not OBS or Qt — so this is the cheap verification loop (`README.md:97-101`).

- [ ] **Step 5: Commit**

```bash
git add engine/src/tile-clock-log.h engine/src/engine-video.cpp engine/src/engine-audio.cpp
git commit -m "feat(tiles): env-gated media-timestamp probe at engine ingest"
```

---

### Task 3: Rig run and verdict

**Files:**
- Create: `docs/tile-clock-findings.md`
- Create: `tools/tile-clock-analyze.py`

**This task requires a live Zoom meeting with at least two participants sending video.** It cannot be completed by an agent without rig access — if you are an agent, stop here and hand back.

- [ ] **Step 1: Write the log-to-verdict script**

```python
#!/usr/bin/env python3
"""Parse TILECLOCK lines from an OBS log and report the timebase verdict.

Usage: python tools/tile-clock-analyze.py <path-to-obs-log>
Mirrors analyze_clock_samples() in src/tile-clock-probe.cpp.
"""
import collections
import re
import sys

MIN_SAMPLES_PER_FEED = 30
SHARED_TOLERANCE_US = 50000
LINE = re.compile(r"TILECLOCK,(\w),(\d+),(\d+),(\d+)")


def main(path):
    by_feed = collections.defaultdict(list)
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            match = LINE.search(line)
            if not match:
                continue
            kind, feed_id, media_pts_us, arrival_ns = match.groups()
            if kind != "v":  # video only; audio ids may be 0 for mixed callbacks
                continue
            offset_us = int(arrival_ns) // 1000 - int(media_pts_us)
            by_feed[int(feed_id)].append(offset_us)

    if len(by_feed) < 2:
        print(f"verdict: insufficient ({len(by_feed)} feed(s) seen; need 2+)")
        return 1

    minimums = {}
    for feed_id, offsets in sorted(by_feed.items()):
        offsets.sort()
        minimums[feed_id] = offsets[0]
        print(f"feed {feed_id}: n={len(offsets)} min={offsets[0]}us "
              f"median={offsets[len(offsets) // 2]}us "
              f"spread={offsets[-1] - offsets[0]}us")

    if any(len(o) < MIN_SAMPLES_PER_FEED for o in by_feed.values()):
        print("verdict: insufficient (a feed has fewer than "
              f"{MIN_SAMPLES_PER_FEED} samples)")
        return 1

    cross = max(minimums.values()) - min(minimums.values())
    verdict = "shared" if cross <= SHARED_TOLERANCE_US else "per-feed"
    print(f"cross-feed spread: {cross}us")
    print(f"verdict: {verdict}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
```

- [ ] **Step 2: Capture a rig log**

Launch OBS with the probe enabled, join a meeting with 2+ participants sending video, subscribe to both, and let it run for at least 60 seconds. The TILECLOCK payloads arrive in the OBS log wrapped in engine-debug JSON lines (`Zoom engine debug: {"cmd":"debug","stage":"tile_clock","msg":"TILECLOCK,..."}`); the analyzer's regex matches inside the line, so no unwrapping is needed. The env var must be set before OBS starts so the spawned engine inherits it:

```powershell
$env:COREVIDEO_TILE_CLOCK_PROBE = "1"
& "C:\Program Files\obs-studio\bin\64bit\obs64.exe"
```

- [ ] **Step 3: Run the analysis**

```sh
python tools/tile-clock-analyze.py "$env:APPDATA/obs-studio/logs/<newest>.txt"
```
Expected: a printed verdict of `shared` or `per-feed`, with per-feed statistics.

- [ ] **Step 4: Write the findings document**

Create `docs/tile-clock-findings.md` recording: the date, OBS and plugin versions, participant count, per-feed sample counts and minimum offsets, cross-feed spread, the verdict, and — most importantly — **what the verdict means for Phase 2**. Also record the **observed unit of `GetTimeStamp()`**: the SDK header documents none, so derive it from consecutive video PTS deltas (≈33,333 per frame at 30 fps ⇒ microseconds; ≈33 ⇒ milliseconds) and state it explicitly — every Phase 2 conversion depends on it, and if it is not microseconds the analyzer's offset math must be re-based before trusting the verdict. State plainly whether the spec's sync design holds as written or needs per-feed offset estimation. Report the numbers you actually observed; if the run was inconclusive, say so rather than rounding toward the convenient answer.

- [ ] **Step 5: Commit**

```bash
git add tools/tile-clock-analyze.py docs/tile-clock-findings.md
git commit -m "docs(tiles): record cross-participant timebase verdict"
```

- [ ] **Step 6: Gate check**

If the verdict is `per-feed`, **stop and re-plan Phase 2** before writing any sync code. Phase 1 below is unaffected either way and may proceed regardless — the grid solver has no timing dependency.

---

# Phase 1 — Grid Solver and Rendering

---

### Task 4: Grid solver — single tile and uniform grids

**Files:**
- Create: `src/zoom-tile-grid.h`, `src/zoom-tile-grid.cpp`
- Test: `tests/tile-grid-test.cpp`
- Modify: `CMakeLists.txt` (inside `if(BUILD_TESTING)`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```cpp
  struct TileRect { double x, y, width, height; };
  struct TileGridParams { double canvas_width, canvas_height, tile_aspect, gutter, margin; };
  std::vector<TileRect> solve_tile_grid(std::size_t count, const TileGridParams &params);
  ```

**Algorithm.** For each candidate row count `r` in `1..count`, `cols = ceil(count / r)`. Compute available space after margins and gutters, then the largest tile satisfying `tile_aspect` that fits both dimensions. Keep the `r` maximizing tile area. The whole grid is centered on the canvas, and a short final row is centered horizontally within its own row.

This is what makes 5 tiles render 3-over-2 rather than 5-across.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/tile-grid-test.cpp
#include "zoom-tile-grid.h"

#include <cmath>
#include <iostream>

static TileGridParams params_1080p()
{
    TileGridParams p;
    p.canvas_width  = 1920.0;
    p.canvas_height = 1080.0;
    p.tile_aspect   = 16.0 / 9.0;
    p.gutter        = 8.0;
    p.margin        = 8.0;
    return p;
}

static bool near(double a, double b, double eps = 0.001)
{
    return std::fabs(a - b) < eps;
}

static bool test_empty()
{
    if (!solve_tile_grid(0, params_1080p()).empty()) {
        std::cerr << "count 0 should produce no rects\n";
        return false;
    }
    return true;
}

static bool test_single_tile_is_centered()
{
    const TileGridParams p = params_1080p();
    const std::vector<TileRect> rects = solve_tile_grid(1, p);
    if (rects.size() != 1) return false;

    const TileRect &r = rects[0];
    if (!near(r.width / r.height, p.tile_aspect)) {
        std::cerr << "single tile aspect wrong: " << r.width / r.height << "\n";
        return false;
    }
    if (!near(r.x + r.width / 2.0, p.canvas_width / 2.0) ||
        !near(r.y + r.height / 2.0, p.canvas_height / 2.0)) {
        std::cerr << "single tile not centered\n";
        return false;
    }
    if (r.x < p.margin - 0.001 || r.y < p.margin - 0.001) {
        std::cerr << "single tile violates margin\n";
        return false;
    }
    return true;
}

static bool test_four_tiles_form_2x2()
{
    const std::vector<TileRect> rects = solve_tile_grid(4, params_1080p());
    if (rects.size() != 4) return false;

    // Two distinct rows, two distinct columns.
    if (!near(rects[0].y, rects[1].y) || !near(rects[2].y, rects[3].y)) {
        std::cerr << "4-up rows not aligned\n";
        return false;
    }
    if (near(rects[0].y, rects[2].y)) {
        std::cerr << "4-up collapsed to a single row\n";
        return false;
    }
    if (!near(rects[0].x, rects[2].x) || !near(rects[1].x, rects[3].x)) {
        std::cerr << "4-up columns not aligned\n";
        return false;
    }
    return true;
}

int main()
{
    if (!test_empty()) return 1;
    if (!test_single_tile_is_centered()) return 1;
    if (!test_four_tiles_form_2x2()) return 1;

    std::cout << "tile-grid: all tests passed\n";
    return 0;
}
```

- [ ] **Step 2: Register the test and verify it fails**

Add inside `if(BUILD_TESTING)` in `CMakeLists.txt`:

```cmake
    add_executable(CoreVideoTileGridTest
        tests/tile-grid-test.cpp
        src/zoom-tile-grid.cpp
    )
    target_include_directories(CoreVideoTileGridTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileGrid
             COMMAND CoreVideoTileGridTest)
```

Run:
```sh
cmake -S . -B build-tests -DCOREVIDEO_BUILD_PLUGIN=OFF -DCOREVIDEO_BUILD_ENGINE=OFF -DCOREVIDEO_BUILD_SIDECAR=OFF -DBUILD_TESTING=ON
cmake --build build-tests --config Debug
```
Expected: FAIL — `zoom-tile-grid.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
// src/zoom-tile-grid.h
#pragma once

#include <cstddef>
#include <vector>

// A single tile's placement, in canvas pixels.
struct TileRect {
    double x      = 0.0;
    double y      = 0.0;
    double width  = 0.0;
    double height = 0.0;
};

struct TileGridParams {
    double canvas_width  = 1920.0;
    double canvas_height = 1080.0;
    double tile_aspect   = 16.0 / 9.0;  // width / height
    double gutter        = 8.0;         // space between adjacent tiles
    double margin        = 8.0;         // space around the whole wall
};

// Lays out `count` identical tiles, choosing the row/column arrangement that
// maximizes tile area. Every returned rect has the same width and height and
// the same gap from its neighbours. A short final row is centered.
// Returns an empty vector when count == 0 or the canvas cannot fit the margins.
std::vector<TileRect> solve_tile_grid(std::size_t count, const TileGridParams &params);
```

- [ ] **Step 4: Write the implementation**

```cpp
// src/zoom-tile-grid.cpp
#include "zoom-tile-grid.h"

#include <algorithm>

std::vector<TileRect> solve_tile_grid(std::size_t count, const TileGridParams &params)
{
    std::vector<TileRect> rects;
    if (count == 0) return rects;
    if (params.tile_aspect <= 0.0) return rects;

    const double usable_w = params.canvas_width  - 2.0 * params.margin;
    const double usable_h = params.canvas_height - 2.0 * params.margin;
    if (usable_w <= 0.0 || usable_h <= 0.0) return rects;

    std::size_t best_rows = 0;
    std::size_t best_cols = 0;
    double      best_tile_w = 0.0;

    for (std::size_t rows = 1; rows <= count; ++rows) {
        const std::size_t cols = (count + rows - 1) / rows;

        const double avail_w = usable_w - params.gutter * static_cast<double>(cols - 1);
        const double avail_h = usable_h - params.gutter * static_cast<double>(rows - 1);
        if (avail_w <= 0.0 || avail_h <= 0.0) continue;

        // Largest tile of the required aspect that fits both dimensions.
        const double tile_w = std::min(avail_w / static_cast<double>(cols),
                                       (avail_h / static_cast<double>(rows)) * params.tile_aspect);
        if (tile_w <= 0.0) continue;

        if (tile_w > best_tile_w) {
            best_tile_w = tile_w;
            best_rows   = rows;
            best_cols   = cols;
        }
    }

    if (best_tile_w <= 0.0) return rects;

    const double tile_h = best_tile_w / params.tile_aspect;
    const double grid_h = tile_h * static_cast<double>(best_rows) +
                          params.gutter * static_cast<double>(best_rows - 1);
    const double start_y = (params.canvas_height - grid_h) / 2.0;

    rects.reserve(count);
    for (std::size_t row = 0; row < best_rows; ++row) {
        const std::size_t placed = row * best_cols;
        const std::size_t in_row = std::min(best_cols, count - placed);
        if (in_row == 0) break;

        // Center each row independently so a short final row sits centered.
        const double row_w = best_tile_w * static_cast<double>(in_row) +
                             params.gutter * static_cast<double>(in_row - 1);
        const double start_x = (params.canvas_width - row_w) / 2.0;

        for (std::size_t col = 0; col < in_row; ++col) {
            TileRect r;
            r.x      = start_x + static_cast<double>(col) * (best_tile_w + params.gutter);
            r.y      = start_y + static_cast<double>(row) * (tile_h + params.gutter);
            r.width  = best_tile_w;
            r.height = tile_h;
            rects.push_back(r);
        }
    }

    return rects;
}
```

- [ ] **Step 5: Run tests to verify they pass**

```sh
cmake --build build-tests --config Debug
ctest --test-dir build-tests -C Debug
```
Expected: PASS, 4/4 tests.

- [ ] **Step 6: Commit**

```bash
git add src/zoom-tile-grid.h src/zoom-tile-grid.cpp tests/tile-grid-test.cpp CMakeLists.txt
git commit -m "feat(tiles): add uniform tile grid solver"
```

---

### Task 5: Short-row centering and layout properties

**Files:**
- Modify: `tests/tile-grid-test.cpp`

**Interfaces:**
- Consumes: `solve_tile_grid()`, `TileRect`, `TileGridParams` from Task 4.
- Produces: nothing new — this task proves the properties the feature was requested for.

Task 4's implementation already centers short rows. This task proves it, and proves the two properties the operator actually asked for: **same size** and **equally spaced**. If any assertion here fails, fix `zoom-tile-grid.cpp` — do not weaken the test.

- [ ] **Step 1: Add the failing property tests**

Insert these functions above `main()` in `tests/tile-grid-test.cpp`:

```cpp
static bool test_five_tiles_center_short_row()
{
    const TileGridParams p = params_1080p();
    const std::vector<TileRect> rects = solve_tile_grid(5, p);
    if (rects.size() != 5) return false;

    // Expect 3 over 2 on a 16:9 canvas.
    if (!near(rects[0].y, rects[2].y) || near(rects[0].y, rects[3].y)) {
        std::cerr << "5-up did not split 3 over 2\n";
        return false;
    }

    // The short row's midpoint must sit on the canvas centre line.
    const double row_left  = rects[3].x;
    const double row_right = rects[4].x + rects[4].width;
    if (!near((row_left + row_right) / 2.0, p.canvas_width / 2.0)) {
        std::cerr << "short row not centered\n";
        return false;
    }
    return true;
}

static bool test_all_tiles_identical_and_evenly_spaced()
{
    const TileGridParams p = params_1080p();
    for (std::size_t count = 1; count <= 16; ++count) {
        const std::vector<TileRect> rects = solve_tile_grid(count, p);
        if (rects.size() != count) {
            std::cerr << "count " << count << ": wrong rect count\n";
            return false;
        }

        for (const TileRect &r : rects) {
            // Same size.
            if (!near(r.width, rects[0].width) || !near(r.height, rects[0].height)) {
                std::cerr << "count " << count << ": tiles differ in size\n";
                return false;
            }
            // Correct aspect.
            if (!near(r.width / r.height, p.tile_aspect)) {
                std::cerr << "count " << count << ": tile aspect wrong\n";
                return false;
            }
            // Inside the canvas, respecting margins.
            if (r.x < p.margin - 0.001 || r.y < p.margin - 0.001 ||
                r.x + r.width  > p.canvas_width  - p.margin + 0.001 ||
                r.y + r.height > p.canvas_height - p.margin + 0.001) {
                std::cerr << "count " << count << ": tile outside canvas margins\n";
                return false;
            }
        }

        // Equally spaced: every horizontal neighbour gap equals the gutter.
        for (std::size_t i = 1; i < rects.size(); ++i) {
            if (!near(rects[i].y, rects[i - 1].y)) continue;  // row break
            const double gap = rects[i].x - (rects[i - 1].x + rects[i - 1].width);
            if (!near(gap, p.gutter)) {
                std::cerr << "count " << count << ": horizontal gap " << gap
                          << " != gutter " << p.gutter << "\n";
                return false;
            }
        }

        // Equally spaced: every vertical row gap equals the gutter.
        for (std::size_t i = 0; i < rects.size(); ++i) {
            for (std::size_t j = i + 1; j < rects.size(); ++j) {
                if (near(rects[i].x, rects[j].x) && !near(rects[i].y, rects[j].y)) {
                    const double gap = rects[j].y - (rects[i].y + rects[i].height);
                    if (gap > 0.0 && !near(gap, p.gutter)) {
                        std::cerr << "count " << count << ": vertical gap " << gap
                                  << " != gutter " << p.gutter << "\n";
                        return false;
                    }
                    break;
                }
            }
        }
    }
    return true;
}

static bool test_portrait_canvas_stacks_vertically()
{
    TileGridParams p = params_1080p();
    p.canvas_width  = 1080.0;
    p.canvas_height = 1920.0;

    const std::vector<TileRect> rects = solve_tile_grid(2, p);
    if (rects.size() != 2) return false;
    if (near(rects[0].y, rects[1].y)) {
        std::cerr << "portrait canvas should stack 2 tiles vertically\n";
        return false;
    }
    return true;
}
```

Then add to `main()`, before the success message:

```cpp
    if (!test_five_tiles_center_short_row()) return 1;
    if (!test_all_tiles_identical_and_evenly_spaced()) return 1;
    if (!test_portrait_canvas_stacks_vertically()) return 1;
```

- [ ] **Step 2: Run tests**

```sh
cmake --build build-tests --config Debug
ctest --test-dir build-tests -C Debug --output-on-failure
```
Expected: PASS. If a property fails, fix `src/zoom-tile-grid.cpp`; the assertions encode the spec's requirements and must not be relaxed.

- [ ] **Step 3: Commit**

```bash
git add tests/tile-grid-test.cpp
git commit -m "test(tiles): prove equal size, equal spacing, centered short rows"
```

---

### Task 6: `CoreVideo Tiles` source registration and assignment list

**Files:**
- Create: `src/zoom-supersource.h`, `src/zoom-supersource.cpp`
- Modify: `src/plugin-main.cpp:263` (after `zoom_participant_audio_source_register();`)
- Modify: `CMakeLists.txt` (add `src/zoom-supersource.cpp` to the plugin target source list)

**Interfaces:**
- Consumes: `solve_tile_grid()`, `TileRect`, `TileGridParams` from Task 4.
- Produces: `void zoom_supersource_register();`

**Pattern to follow:** `src/zoom-source.cpp:1826-1857` registers three sources off one `obs_source_info`. Mirror its structure — `OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE`, with `get_name` / `create` / `destroy` / `update` / `get_width` / `get_height` / `get_properties` / `get_defaults`.

**Source id:** `corevideo_tiles_source`. **Display name:** `CoreVideo Tiles`.

**Scope of this task:** registration, properties, and assignment-list state only. It renders nothing yet — `get_width`/`get_height` return the configured canvas size, and no frames are output. Task 7 adds compositing. A reviewer should be able to load OBS, add the source, see it in the source list with a working participant list, and see a black frame.

Properties: an editable participant-id list (`obs_properties_add_editable_list` with `OBS_EDITABLE_LIST_TYPE_STRINGS`, name `participants`), plus int properties `canvas_width` (default 1920) and `canvas_height` (default 1080).

Store parsed participant ids in a `std::vector<uint32_t>` on the source's private data, rebuilt in `update`. Ignore entries that do not parse as an unsigned integer rather than failing the update — a half-typed id in the list must not break the source.

- [ ] **Step 1: Write the header**

```cpp
// src/zoom-supersource.h
#pragma once

// Registers the "CoreVideo Tiles" OBS source (id: corevideo_tiles_source),
// which renders assigned Zoom participants as identical, evenly-spaced tiles.
void zoom_supersource_register();
```

- [ ] **Step 2: Implement registration**

Write `src/zoom-supersource.cpp`. This mirrors `src/zoom-source.cpp:1826-1857`:

```cpp
#include "zoom-supersource.h"
#include "zoom-tile-grid.h"

#include <obs-module.h>

#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

struct tiles_source {
    obs_source_t         *source = nullptr;
    std::vector<uint32_t> participants;
    uint32_t              canvas_width  = 1920;
    uint32_t              canvas_height = 1080;
    std::mutex            mutex;  // guards participants: update() races the tick
};

static const char *tiles_source_get_name(void *)
{
    return "CoreVideo Tiles";
}

static void tiles_source_update(void *data, obs_data_t *settings)
{
    auto *ctx = static_cast<tiles_source *>(data);

    std::vector<uint32_t> parsed;
    obs_data_array_t *list = obs_data_get_array(settings, "participants");
    const size_t entries = obs_data_array_count(list);
    for (size_t i = 0; i < entries; ++i) {
        obs_data_t *item = obs_data_array_item(list, i);
        const char *value = obs_data_get_string(item, "value");
        if (value && *value) {
            char *end = nullptr;
            const unsigned long id = std::strtoul(value, &end, 10);
            // Skip unparseable entries rather than failing the update: a
            // half-typed id must never break the source.
            if (end && end != value && *end == '\0' && id > 0)
                parsed.push_back(static_cast<uint32_t>(id));
        }
        obs_data_release(item);
    }
    obs_data_array_release(list);

    const uint32_t width  = static_cast<uint32_t>(obs_data_get_int(settings, "canvas_width"));
    const uint32_t height = static_cast<uint32_t>(obs_data_get_int(settings, "canvas_height"));

    std::lock_guard<std::mutex> lock(ctx->mutex);
    ctx->participants.swap(parsed);
    if (width  > 0) ctx->canvas_width  = width;
    if (height > 0) ctx->canvas_height = height;
}

static void *tiles_source_create(obs_data_t *settings, obs_source_t *source)
{
    auto *ctx = new tiles_source();
    ctx->source = source;
    tiles_source_update(ctx, settings);
    return ctx;
}

static void tiles_source_destroy(void *data)
{
    delete static_cast<tiles_source *>(data);
}

static uint32_t tiles_source_get_width(void *data)
{
    auto *ctx = static_cast<tiles_source *>(data);
    std::lock_guard<std::mutex> lock(ctx->mutex);
    return ctx->canvas_width;
}

static uint32_t tiles_source_get_height(void *data)
{
    auto *ctx = static_cast<tiles_source *>(data);
    std::lock_guard<std::mutex> lock(ctx->mutex);
    return ctx->canvas_height;
}

static void tiles_source_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "canvas_width",  1920);
    obs_data_set_default_int(settings, "canvas_height", 1080);
}

static obs_properties_t *tiles_source_get_properties(void *)
{
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_editable_list(props, "participants",
                                     "Participants (Zoom user ids, in tile order)",
                                     OBS_EDITABLE_LIST_TYPE_STRINGS, nullptr, nullptr);
    obs_properties_add_int(props, "canvas_width",  "Canvas width",  16, 7680, 2);
    obs_properties_add_int(props, "canvas_height", "Canvas height", 16, 4320, 2);
    return props;
}

void zoom_supersource_register()
{
    obs_source_info info = {};
    info.id             = "corevideo_tiles_source";
    info.type           = OBS_SOURCE_TYPE_INPUT;
    info.output_flags   = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name       = tiles_source_get_name;
    info.create         = tiles_source_create;
    info.destroy        = tiles_source_destroy;
    info.update         = tiles_source_update;
    info.get_width      = tiles_source_get_width;
    info.get_height     = tiles_source_get_height;
    info.get_properties = tiles_source_get_properties;
    info.get_defaults   = tiles_source_get_defaults;
    obs_register_source(&info);
}
```

- [ ] **Step 3: Register the source at startup**

In `src/plugin-main.cpp`, add `#include "zoom-supersource.h"` with the other includes, and after line 263 (`zoom_participant_audio_source_register();`) add:

```cpp
    zoom_supersource_register();
```

- [ ] **Step 4: Add to the plugin target**

In `CMakeLists.txt`, add `src/zoom-supersource.cpp` and `src/zoom-tile-grid.cpp` to the plugin target's source list alongside the other `src/*.cpp` entries.

- [ ] **Step 5: Build and verify in OBS**

```sh
cmake --build build --config Release
```
Then install the built plugin and launch OBS. Expected: **CoreVideo Tiles** appears in the add-source list; adding it shows participant-list and canvas-size properties; the source renders black. Confirm this by actually adding the source in OBS — do not mark this step done on a successful compile alone.

- [ ] **Step 6: Commit**

```bash
git add src/zoom-supersource.h src/zoom-supersource.cpp src/plugin-main.cpp CMakeLists.txt
git commit -m "feat(tiles): register CoreVideo Tiles source with assignment list"
```

---

### Task 7: Composite assigned feeds onto the grid

**Files:**
- Modify: `src/zoom-supersource.cpp`

**Interfaces:**
- Consumes: `solve_tile_grid()` from Task 4; the tiles-source private data from Task 6.
- Produces: nothing new — this completes Phase 1.

**Frame selection is naive in this phase, by design.** Each tile takes the most recent frame available for its participant, with no shared presentation clock. That is knowingly wrong for lip sync and is exactly what Phase 2 fixes; it is enough to prove the layout renders correctly. Do not attempt sync here.

**Compositing is CPU-side I420 in this phase.** The source is `OBS_SOURCE_ASYNC_VIDEO`, so it pushes finished frames via `obs_source_output_video`. A true GPU composite requires converting this to a render-callback source and reworking `hw-video-pipeline.cpp`, which is a larger architectural change than proving the grid — and the spec flags 4K composite cost as a real risk. Measure first, then plan the GPU path against numbers.

**Crop to fill:** for each tile, scale the source frame to *cover* the tile rect and center-crop the overflow, per the spec. Never letterbox.

**No-video participants** render as a flat neutral tile. Name-text rendering is out of scope for v1.

- [ ] **Step 1: Add the crop-to-fill geometry helper, under test**

The cover-crop calculation is pure geometry and is the part most likely to be
written wrong, so it goes in the tested unit rather than inside the blit loop.

Append to `src/zoom-tile-grid.h`:

```cpp
// The sub-rectangle of a source frame to sample so that it fills a tile of
// `dst_aspect` completely, cropping the overflow evenly on both sides.
// Never letterboxes: the returned rect always has aspect == dst_aspect and
// fits inside the source frame.
struct CropRect { double x, y, width, height; };

CropRect solve_cover_crop(double src_width, double src_height, double dst_aspect);
```

Append to `src/zoom-tile-grid.cpp`:

```cpp
CropRect solve_cover_crop(double src_width, double src_height, double dst_aspect)
{
    CropRect crop;
    if (src_width <= 0.0 || src_height <= 0.0 || dst_aspect <= 0.0) return crop;

    const double src_aspect = src_width / src_height;
    if (src_aspect > dst_aspect) {
        // Source is wider than the tile: keep full height, crop the sides.
        crop.height = src_height;
        crop.width  = src_height * dst_aspect;
    } else {
        // Source is taller than the tile: keep full width, crop top and bottom.
        crop.width  = src_width;
        crop.height = src_width / dst_aspect;
    }
    crop.x = (src_width  - crop.width)  / 2.0;
    crop.y = (src_height - crop.height) / 2.0;
    return crop;
}
```

Append this test to `tests/tile-grid-test.cpp` above `main()`:

```cpp
static bool test_cover_crop()
{
    const double tile_aspect = 16.0 / 9.0;

    // Matching aspect: no crop at all.
    CropRect c = solve_cover_crop(1920.0, 1080.0, tile_aspect);
    if (!near(c.x, 0.0) || !near(c.y, 0.0) ||
        !near(c.width, 1920.0) || !near(c.height, 1080.0)) {
        std::cerr << "16:9 source should not be cropped\n";
        return false;
    }

    // Portrait source: full width kept, top and bottom cropped evenly.
    c = solve_cover_crop(1080.0, 1920.0, tile_aspect);
    if (!near(c.width, 1080.0)) {
        std::cerr << "portrait source should keep full width\n";
        return false;
    }
    if (!near(c.height, 1080.0 / tile_aspect)) {
        std::cerr << "portrait crop height wrong\n";
        return false;
    }
    if (!near(c.y, (1920.0 - c.height) / 2.0) || !near(c.x, 0.0)) {
        std::cerr << "portrait crop not centered\n";
        return false;
    }

    // Ultra-wide source: full height kept, sides cropped evenly.
    c = solve_cover_crop(3840.0, 1080.0, tile_aspect);
    if (!near(c.height, 1080.0) || !near(c.width, 1080.0 * tile_aspect)) {
        std::cerr << "ultra-wide crop wrong\n";
        return false;
    }
    if (!near(c.x, (3840.0 - c.width) / 2.0)) {
        std::cerr << "ultra-wide crop not centered\n";
        return false;
    }

    // The result must always match the tile aspect and stay inside the source.
    if (!near(c.width / c.height, tile_aspect)) {
        std::cerr << "crop aspect does not match tile\n";
        return false;
    }
    return true;
}
```

Add to `main()`: `if (!test_cover_crop()) return 1;`

Run:
```sh
cmake --build build-tests --config Debug
ctest --test-dir build-tests -C Debug --output-on-failure
```
Expected: PASS.

- [ ] **Step 2: Implement the composite**

Add a tick/callback that, for each frame interval:
1. Copies the participant list under the mutex.
2. Calls `solve_tile_grid(participants.size(), params)` with the configured canvas size, `tile_aspect = 16.0 / 9.0`, and gutter and margin each set to `canvas_height / 135.0` (8 px at 1080p, scaling with canvas per the Global Constraints).
3. Allocates or reuses an I420 buffer of the canvas size, cleared to neutral.
4. For each tile index, fetches that participant's latest frame, computes its source rect with `solve_cover_crop(frame_width, frame_height, 16.0 / 9.0)` from Step 1, and blits that sub-rectangle scaled into the tile rect.
5. Emits via `obs_source_output_video` with `frame.timestamp = os_gettime_ns()`.

Reuse the buffer across frames — do not allocate per frame. Allocation churn at 4K was the root cause of the operator stutter already documented in this project.

- [ ] **Step 3: Verify layout in OBS with real feeds**

Join a meeting with 3+ participants sending video, assign them to the source, and confirm: tiles are identical in size, gaps are visibly uniform, a 5th participant produces 3-over-2 with the short row centered, and removing a participant reflows immediately.

- [ ] **Step 4: Measure composite cost**

With the source live at 1080p and again at 4K, record CPU usage and OBS's reported render/frame time at 2, 4, 6, and 9 tiles. Write the numbers into `docs/tile-clock-findings.md` under a new "Phase 1 composite cost" heading. These numbers decide whether the GPU path is required before beta, and a Phase 1b plan should be written against them rather than against a guess.

- [ ] **Step 5: Commit**

```bash
git add src/zoom-tile-grid.h src/zoom-tile-grid.cpp tests/tile-grid-test.cpp \
        src/zoom-supersource.cpp docs/tile-clock-findings.md
git commit -m "feat(tiles): composite assigned feeds onto the solved grid"
```

---

## Definition of Done

- `ctest --test-dir build-tests -C Debug` passes, including `CoreVideoTileClockProbe` and `CoreVideoTileGrid`.
- `docs/tile-clock-findings.md` records a real rig verdict, with observed numbers.
- **CoreVideo Tiles** renders identical, evenly-spaced tiles for 1..9 assigned participants in live OBS, reflowing on assignment change.
- Composite cost is measured at 1080p and 4K and written down.

## Explicitly Not Done After This Plan

Per-tile lip sync, the presentation clock, audio delay lines, adaptive `L`, hold-last-frame starvation handling, GPU compositing, name-tile text, and the subscription-count-against-cap indicator. Phase 2 covers the sync work and must be re-planned once Task 3's verdict is known; the remaining UI items are Phase 3 per the spec's phasing.

The spec lists the name tile and the subscription-count indicator as v1 scope, and they remain v1 scope — they are simply scheduled into Phase 3 rather than dropped.
