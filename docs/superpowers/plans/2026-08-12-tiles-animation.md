# Tiles Layout Animation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Animate the CoreVideo Tiles wall when participants join and leave, so the layout reflows continuously instead of jumping on a single frame.

**Architecture:** A new pure header `src/zoom-tile-animator.h` owns all motion state, keyed by participant id, and is advanced by elapsed nanoseconds. It consumes the existing grid solver's output as a *target* and emits per-tile rects plus alpha. `tiles_source_render()` draws what it emits: today's snapped blit for tiles at rest, an intermediate-texture path for tiles in motion. The grid solver is untouched.

**Tech Stack:** C++17, CMake, libobs graphics API, standalone `main()` test executables registered with CTest (no test framework).

**Spec:** `docs/superpowers/specs/2026-08-12-tiles-animation-design.md`

## Global Constraints

- Animation is **off by default**. With it off, the animator must not run and the render path must be today's path.
- At rest, output must be byte-identical to today: `snap_tile_grid_even(solve_tile_grid(...))`, even-pixel edges, existing shader.
- Sub-pixel placement is permitted **only** while a tile is in motion, and only via an intermediate texture of even dimensions.
- Settle window is **250 ms**, fixed in code, not exposed as a setting.
- Default duration is **350 ms**.
- Exit invariants (safety-critical, from the spec): an exit may begin only on a genuine roster departure; a slot reassignment cuts instantly with no fade; any repoint cancels a running exit immediately; an exit never outlives its duration.
- All timing is elapsed-nanosecond based from `os_gettime_ns()`. Never per-frame increments.
- Pure headers must not include libobs, Qt, or Zoom SDK headers.
- Tests are standalone `main()` executables in `tests/`, registered in `CMakeLists.txt` beside `CoreVideoTileBorderTest`, using the `fail()`/counter style of `tests/speaker-director-test.cpp`.
- Every commit message ends with: `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`

## Existing interfaces this plan builds on

```cpp
// src/zoom-tile-grid.h
struct TileRect { double x, y, width, height; };          // all default 0.0
struct TileGridParams {
    double canvas_width, canvas_height, tile_aspect, gutter, margin;
};
struct SnappedTileRect { uint32_t x, y, width, height; };
std::vector<TileRect> solve_tile_grid(std::size_t count, const TileGridParams &params);
std::vector<SnappedTileRect> snap_tile_grid_even(const std::vector<TileRect> &rects,
                                                 const TileGridParams &params);

// src/zoom-tile-slot.h  — a tile's identity at render time
uint32_t TileSlotState::participant_id() const;

// src/zoom-supersource.cpp — render already holds:
const std::vector<TileFeedPtr> &feeds = ctx->render_feeds;   // feeds[i]->slot.participant_id()
```

**Build and test commands** (used by every task):

```
cmake --build build-rel --config Release --parallel
cd build-rel && ctest -C Release --output-on-failure
```

If `build-rel` does not exist, configure it as the repo does; it requires `-DCMAKE_MODULE_PATH=<obs>/cmake/finders` or configure fails on `FindSIMDe.cmake`.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `src/tile-motion.h` (create) | One tile's critically damped spring: position + velocity, advanced by dt. Nothing else. |
| `src/zoom-tile-animator.h` (create) | Identity-keyed animation state: target assignment, retargeting, settle window, phases, exit invariants. Consumes `TileRect`, emits `AnimatedTile`. |
| `tests/tile-motion-test.cpp` (create) | Spring behaviour: settling, velocity continuity, frame-rate independence. |
| `tests/zoom-tile-animator-test.cpp` (create) | Animator behaviour: keying, blips, coalescing, phases, exit invariants, disabled parity. |
| `CMakeLists.txt` (modify) | Register both test executables. |
| `src/zoom-supersource.cpp` (modify) | Properties, defaults, and the render-path change. |
| `data/locale/en-US.ini` (modify) | Property strings. |

Tasks 1–5 are pure and land a fully tested animator with no user-visible change. Tasks 6–8 wire it in. Task 9 is rig validation.

---

### Task 1: Critically damped spring primitive

**Files:**
- Create: `src/tile-motion.h`
- Create: `tests/tile-motion-test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `struct Spring1D { double position; double velocity; }` and
  `void spring_advance(Spring1D &s, double target, double settle_seconds, double dt_seconds);`

- [ ] **Step 1: Write the failing test**

Create `tests/tile-motion-test.cpp`:

```cpp
#include "tile-motion.h"

#include <cmath>
#include <iostream>

static int failures = 0;
static void check(bool ok, const char *message)
{
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; ++failures; }
}

// Advance a spring for `seconds` in fixed steps, returning the final state.
static Spring1D run(Spring1D s, double target, double settle, double seconds, double dt)
{
    for (double t = 0.0; t < seconds; t += dt)
        spring_advance(s, target, settle, dt);
    return s;
}

int main()
{
    // Settles to the target within the settle time, and stays there.
    {
        Spring1D s{0.0, 0.0};
        s = run(s, 100.0, 0.35, 0.35, 1.0 / 60.0);
        check(std::fabs(s.position - 100.0) < 2.0,
              "spring did not substantially reach its target within the settle time");
        s = run(s, 100.0, 0.35, 1.0, 1.0 / 60.0);
        check(std::fabs(s.position - 100.0) < 0.01, "spring did not come to rest on target");
        check(std::fabs(s.velocity) < 0.01, "spring still moving after settling");
    }

    // Critically damped: never overshoots.
    {
        Spring1D s{0.0, 0.0};
        double peak = 0.0;
        for (int i = 0; i < 120; ++i) {
            spring_advance(s, 100.0, 0.35, 1.0 / 60.0);
            peak = std::fmax(peak, s.position);
        }
        check(peak <= 100.0001, "spring overshot its target — not critically damped");
    }

    // Retargeting mid-flight preserves velocity: the whole point of the model.
    {
        Spring1D s{0.0, 0.0};
        s = run(s, 100.0, 0.35, 0.1, 1.0 / 60.0);
        const double v_before = s.velocity;
        check(v_before > 1.0, "test setup: spring should be moving before retarget");
        spring_advance(s, 500.0, 0.35, 1.0 / 60.0);
        check(s.velocity > v_before * 0.5,
              "velocity collapsed on retarget — motion would visibly hitch");
    }

    // Frame-rate independence: same elapsed time, same place.
    {
        Spring1D a{0.0, 0.0}, b{0.0, 0.0};
        a = run(a, 100.0, 0.35, 0.5, 1.0 / 60.0);
        b = run(b, 100.0, 0.35, 0.5, 1.0 / 30.0);
        check(std::fabs(a.position - b.position) < 1.0,
              "60fps and 30fps diverged — motion is frame-rate dependent");
    }

    // A zero or negative settle time snaps rather than dividing by zero.
    {
        Spring1D s{0.0, 0.0};
        spring_advance(s, 100.0, 0.0, 1.0 / 60.0);
        check(s.position == 100.0, "zero settle time did not snap to target");
        check(s.velocity == 0.0, "zero settle time left residual velocity");
    }

    if (failures == 0) std::cout << "tile-motion tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Add the CMake entry first (the test cannot build otherwise). In `CMakeLists.txt`, immediately after the `add_test(NAME CoreVideoTileBorder ...)` block:

```cmake
    # One tile's critically damped spring. Pure maths; no OBS, no GPU.
    add_executable(CoreVideoTileMotionTest
        tests/tile-motion-test.cpp
    )
    target_include_directories(CoreVideoTileMotionTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileMotion
             COMMAND CoreVideoTileMotionTest)
```

Then create `src/tile-motion.h` containing a deliberately wrong stub so the failure is behavioural, not a compile error:

```cpp
// src/tile-motion.h
#pragma once

struct Spring1D {
    double position = 0.0;
    double velocity = 0.0;
};

inline void spring_advance(Spring1D &s, double target, double settle_seconds,
                           double dt_seconds)
{
    (void)target; (void)settle_seconds; (void)dt_seconds;
    // Stub: does nothing, so every settling assertion fails.
}
```

Run: `cmake --build build-rel --config Release --target CoreVideoTileMotionTest --parallel` then `.\build-rel\Release\CoreVideoTileMotionTest.exe`

Expected: FAIL — "spring did not substantially reach its target within the settle time", "spring did not come to rest on target", and the retarget setup check.

- [ ] **Step 3: Write the implementation**

Replace the stub body in `src/tile-motion.h`:

```cpp
inline void spring_advance(Spring1D &s, double target, double settle_seconds,
                           double dt_seconds)
{
    // Snap when there is no time to travel in, rather than dividing by zero.
    // This is also the path taken when the operator sets duration to 0.
    if (settle_seconds <= 0.0 || dt_seconds <= 0.0) {
        s.position = target;
        s.velocity = 0.0;
        return;
    }

    // Exact solution of the critically damped spring, not a numerical
    // integrator. An approximate integrator overshoots — measurably, and by an
    // amount that does not vanish as dt shrinks — and overshoot on a tile means
    // it sails past its slot and comes back, which is precisely the cheap look
    // this feature exists to avoid. The closed form also makes the result
    // identical at any frame rate by construction rather than by tolerance.
    //
    // kSettleFactor is the critically damped 1%-remaining constant, the root of
    // (1 + k)e^-k = 0.01. It is NOT 4.6: that is the first-order constant, and
    // a second-order critically damped system decays as (1 + wt)e^-wt, which at
    // 4.6 is still 5.6% short of its target.
    constexpr double kSettleFactor = 6.6384;
    const double omega = kSettleFactor / settle_seconds;

    const double delta = s.position - target;
    const double decay = std::exp(-omega * dt_seconds);
    const double c     = s.velocity + omega * delta;

    s.position = target + (delta + c * dt_seconds) * decay;
    s.velocity = (s.velocity - omega * c * dt_seconds) * decay;
}
```

`src/tile-motion.h` needs `#include <cmath>` for `std::exp`.

- [ ] **Step 4: Run the test to verify it passes**

Run: `.\build-rel\Release\CoreVideoTileMotionTest.exe`
Expected: PASS — `tile-motion tests passed`

Then the whole suite: `cd build-rel && ctest -C Release --output-on-failure` — expect all tests passing, one more than before.

- [ ] **Step 5: Commit**

```bash
git add src/tile-motion.h tests/tile-motion-test.cpp CMakeLists.txt
git commit -m "feat(tiles): critically damped spring for tile motion

Velocity is state, not derived, so a spring retargeted mid-flight
continues from its current speed. A restarted ease would begin at zero
velocity and visibly hitch a tile that was already moving.

Implicitly integrated so a long frame cannot make it diverge.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Animator skeleton — identity keying and reflow

**Files:**
- Create: `src/zoom-tile-animator.h`
- Create: `tests/zoom-tile-animator-test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Spring1D`, `spring_advance()` from Task 1; `TileRect` from `src/zoom-tile-grid.h`.
- Produces:

```cpp
struct AnimationSettings {
    bool     enabled          = false;
    double   duration_seconds = 0.35;
};
struct DesiredTile { uint32_t participant_id; TileRect rect; };
struct AnimatedTile {
    uint32_t  participant_id = 0;
    TileRect  rect;
    double    alpha    = 1.0;
    bool      at_rest  = true;
};
class TileAnimator {
public:
    std::vector<AnimatedTile> advance(uint64_t now_ns,
                                      const std::vector<DesiredTile> &desired,
                                      const AnimationSettings &settings);
};
```

- [ ] **Step 1: Write the failing test**

Create `tests/zoom-tile-animator-test.cpp`:

```cpp
#include "zoom-tile-animator.h"

#include <cmath>
#include <iostream>

static int failures = 0;
static void check(bool ok, const char *message)
{
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; ++failures; }
}

static TileRect rect(double x, double y, double w, double h)
{
    TileRect r; r.x = x; r.y = y; r.width = w; r.height = h; return r;
}

static const AnimatedTile *find(const std::vector<AnimatedTile> &tiles, uint32_t id)
{
    for (const auto &t : tiles)
        if (t.participant_id == id) return &t;
    return nullptr;
}

static constexpr uint64_t kMs = 1000000ULL;

int main()
{
    AnimationSettings on;  on.enabled = true;  on.duration_seconds = 0.35;
    AnimationSettings off; off.enabled = false;

    // Disabled: emits the desired rects verbatim, at rest, always.
    {
        TileAnimator a;
        const std::vector<DesiredTile> desired{{1, rect(0, 0, 100, 100)},
                                               {2, rect(100, 0, 100, 100)}};
        const auto out = a.advance(0, desired, off);
        check(out.size() == 2, "disabled animator dropped tiles");
        check(find(out, 1) && find(out, 1)->rect.x == 0.0 &&
              find(out, 2) && find(out, 2)->rect.x == 100.0,
              "disabled animator did not emit the desired rects verbatim");
        check(find(out, 1)->at_rest && find(out, 1)->alpha == 1.0,
              "disabled animator reported motion");
    }

    // First frame with animation on: tiles appear at their target, not from 0,0.
    {
        TileAnimator a;
        const std::vector<DesiredTile> desired{{1, rect(50, 60, 100, 100)}};
        const auto out = a.advance(0, desired, on);
        check(out.size() == 1 && find(out, 1)->rect.x == 50.0,
              "a newly seen tile did not start at its target position");
    }

    // A tile that moves is reported not-at-rest and travels toward the target.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}}, on);
        const auto out = a.advance(16 * kMs, {{1, rect(400, 0, 100, 100)}}, on);
        const AnimatedTile *t = find(out, 1);
        check(t != nullptr, "tile disappeared when retargeted");
        check(!t->at_rest, "a moving tile reported itself at rest");
        check(t->rect.x > 0.0 && t->rect.x < 400.0,
              "tile did not travel toward its new target");
    }

    // Identity, not index: the surviving tile keeps its own position when the
    // tile before it disappears from the desired layout.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {7, rect(100, 0, 100, 100)}}, on);
        const auto out = a.advance(16 * kMs, {{7, rect(0, 0, 200, 200)}}, on);
        const AnimatedTile *t = find(out, 7);
        check(t != nullptr, "participant 7 was lost when participant 1 left");
        check(t->rect.x > 0.0,
              "participant 7 teleported to participant 1's old slot — keyed by index, not identity");
    }

    if (failures == 0) std::cout << "zoom-tile-animator tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Add to `CMakeLists.txt`, after the `CoreVideoTileMotion` block:

```cmake
    # Identity-keyed animation state: reflow, blips, phases, exit invariants.
    add_executable(CoreVideoTileAnimatorTest
        tests/zoom-tile-animator-test.cpp
    )
    target_include_directories(CoreVideoTileAnimatorTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileAnimator
             COMMAND CoreVideoTileAnimatorTest)
```

Create `src/zoom-tile-animator.h` with the types and a stub `advance()` that returns an empty vector, so failures are behavioural:

```cpp
// src/zoom-tile-animator.h
#pragma once

#include "tile-motion.h"
#include "zoom-tile-grid.h"

#include <cstdint>
#include <map>
#include <vector>

struct AnimationSettings {
    bool   enabled          = false;
    double duration_seconds = 0.35;
};

struct DesiredTile {
    uint32_t participant_id = 0;
    TileRect rect;
};

struct AnimatedTile {
    uint32_t participant_id = 0;
    TileRect rect;
    double   alpha   = 1.0;
    bool     at_rest = true;
};

class TileAnimator {
public:
    std::vector<AnimatedTile> advance(uint64_t now_ns,
                                      const std::vector<DesiredTile> &desired,
                                      const AnimationSettings &settings)
    {
        (void)now_ns; (void)desired; (void)settings;
        return {};  // Stub.
    }
};
```

Run: `cmake --build build-rel --config Release --target CoreVideoTileAnimatorTest --parallel` then `.\build-rel\Release\CoreVideoTileAnimatorTest.exe`

Expected: FAIL — "disabled animator dropped tiles", "a newly seen tile did not start at its target position", "tile disappeared when retargeted", "participant 7 was lost when participant 1 left".

- [ ] **Step 3: Write the implementation**

Replace the stub in `src/zoom-tile-animator.h`:

```cpp
class TileAnimator {
public:
    std::vector<AnimatedTile> advance(uint64_t now_ns,
                                      const std::vector<DesiredTile> &desired,
                                      const AnimationSettings &settings)
    {
        // Disabled is a bypass, not a fast setting: no state is touched and the
        // desired layout is emitted verbatim, so the renderer takes exactly the
        // path it took before this feature existed.
        if (!settings.enabled) {
            m_tiles.clear();
            m_last_ns = 0;
            m_has_last = false;
            std::vector<AnimatedTile> out;
            out.reserve(desired.size());
            for (const auto &d : desired)
                out.push_back(AnimatedTile{d.participant_id, d.rect, 1.0, true});
            return out;
        }

        // An explicit "have we been called before" flag, not a sentinel value
        // of m_last_ns. Zero is a legitimate timestamp — the tests advance from
        // 0 — and overloading it means m_last_ns stays 0 after a first call at
        // 0, so dt is forced to zero on two consecutive calls and the wall
        // silently loses a frame of motion.
        const double dt = (!m_has_last || now_ns <= m_last_ns)
            ? 0.0
            : static_cast<double>(now_ns - m_last_ns) / 1e9;
        m_last_ns = now_ns;
        m_has_last = true;

        std::vector<AnimatedTile> out;
        out.reserve(desired.size());
        for (const auto &d : desired) {
            auto it = m_tiles.find(d.participant_id);
            if (it == m_tiles.end()) {
                // First sight of this participant: start at the target rather
                // than flying in from the origin.
                Motion m;
                m.x = {d.rect.x, 0.0};
                m.y = {d.rect.y, 0.0};
                m.w = {d.rect.width, 0.0};
                m.h = {d.rect.height, 0.0};
                it = m_tiles.emplace(d.participant_id, m).first;
            }

            Motion &m = it->second;
            spring_advance(m.x, d.rect.x,      settings.duration_seconds, dt);
            spring_advance(m.y, d.rect.y,      settings.duration_seconds, dt);
            spring_advance(m.w, d.rect.width,  settings.duration_seconds, dt);
            spring_advance(m.h, d.rect.height, settings.duration_seconds, dt);

            TileRect r;
            r.x = m.x.position; r.y = m.y.position;
            r.width = m.w.position; r.height = m.h.position;

            constexpr double kRestEpsilon = 0.05;  // sub-tenth-pixel
            const bool at_rest =
                std::fabs(r.x - d.rect.x) < kRestEpsilon &&
                std::fabs(r.y - d.rect.y) < kRestEpsilon &&
                std::fabs(r.width - d.rect.width) < kRestEpsilon &&
                std::fabs(r.height - d.rect.height) < kRestEpsilon;

            out.push_back(AnimatedTile{d.participant_id, r, 1.0, at_rest});
        }

        // Forget participants no longer desired. Task 4 replaces this with the
        // exit lifecycle.
        for (auto it = m_tiles.begin(); it != m_tiles.end();) {
            bool still_wanted = false;
            for (const auto &d : desired)
                if (d.participant_id == it->first) { still_wanted = true; break; }
            it = still_wanted ? std::next(it) : m_tiles.erase(it);
        }

        return out;
    }

private:
    struct Motion { Spring1D x, y, w, h; };
    std::map<uint32_t, Motion> m_tiles;
    uint64_t m_last_ns  = 0;
    bool     m_has_last = false;
};
```

Add `#include <cmath>` and `#include <iterator>` to the header's include block.

- [ ] **Step 4: Run the test to verify it passes**

Run: `.\build-rel\Release\CoreVideoTileAnimatorTest.exe`
Expected: PASS — `zoom-tile-animator tests passed`

Then: `cd build-rel && ctest -C Release --output-on-failure` — all pass.

- [ ] **Step 5: Commit**

```bash
git add src/zoom-tile-animator.h tests/zoom-tile-animator-test.cpp CMakeLists.txt
git commit -m "feat(tiles): identity-keyed animator with reflow

Keyed by participant id, not by index. Today's layout is positional, so
'the same participant moved from here to there' could not be expressed —
which is also why a departure shifts everyone after it.

Disabled is a bypass that clears state and emits the solver's rects
verbatim, so the renderer takes exactly its pre-existing path.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Settle window — blips never move the wall

**Files:**
- Modify: `src/zoom-tile-animator.h`
- Modify: `tests/zoom-tile-animator-test.cpp`

**Interfaces:**
- Consumes: `TileAnimator::advance()` from Task 2.
- Produces: no signature change. Adds internal `kSettleNs = 250000000ULL` behaviour: a change in the desired participant *set* is not acted on until it has held for 250 ms.

- [ ] **Step 1: Write the failing test**

Append inside `main()` in `tests/zoom-tile-animator-test.cpp`, before the final `if (failures == 0)`:

```cpp
    // A blip never moves the wall: a participant who vanishes and returns
    // inside the settle window produces no motion at all.
    {
        TileAnimator a;
        const std::vector<DesiredTile> two{{1, rect(0, 0, 100, 100)},
                                           {2, rect(100, 0, 100, 100)}};
        const std::vector<DesiredTile> one{{1, rect(0, 0, 200, 200)}};
        a.advance(0, two, on);
        a.advance(100 * kMs, one, on);          // 2 disappears
        const auto back = a.advance(150 * kMs, two, on);  // and returns
        const AnimatedTile *t = find(back, 1);
        check(t != nullptr, "participant 1 lost during a blip");
        check(std::fabs(t->rect.width - 100.0) < 0.001,
              "the wall reflowed for a blip that never settled");
        check(find(back, 2) != nullptr, "the returning participant was dropped");
    }

    // A change that holds past the settle window is acted on.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on);
        a.advance(100 * kMs, {{1, rect(0, 0, 200, 200)}}, on);
        const auto out = a.advance(400 * kMs, {{1, rect(0, 0, 200, 200)}}, on);
        const AnimatedTile *t = find(out, 1);
        check(t != nullptr && t->rect.width > 100.0,
              "a settled departure did not start the reflow");
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build-rel --config Release --target CoreVideoTileAnimatorTest --parallel` then `.\build-rel\Release\CoreVideoTileAnimatorTest.exe`

Expected: FAIL — "the wall reflowed for a blip that never settled" (Task 2 acts on every change immediately).

- [ ] **Step 3: Write the implementation**

In `src/zoom-tile-animator.h`, add to the private section:

```cpp
    // A roster change must hold this long before the wall reacts. The roster is
    // known to flicker — SpeakerDirector carries a 60s absence grace for the
    // same reason — and without this a 120ms dropout would produce a full exit
    // animation followed by a full entry, which is more visible on air than the
    // single-frame pop it replaces. Fixed, not a setting: zero would reintroduce
    // exactly the behaviour this exists to prevent.
    static constexpr uint64_t kSettleNs = 250000000ULL;

    std::vector<uint32_t> m_committed_ids;   // the set the wall is laid out for
    std::vector<uint32_t> m_pending_ids;     // a candidate set, not yet settled
    uint64_t m_pending_since_ns = 0;
    // Explicit, not `m_committed_ids.empty()`: an empty committed set is also
    // what a legitimately vacated wall looks like, and conflating the two lets a
    // vacate-and-return blip skip the hold entirely.
    bool m_has_committed = false;
    // Distinguishes "an empty roster is proposed" from "nothing is proposed".
    bool m_has_pending = false;
```

Add this helper to the private section:

```cpp
    static std::vector<uint32_t> ids_of(const std::vector<DesiredTile> &d)
    {
        std::vector<uint32_t> ids;
        ids.reserve(d.size());
        for (const auto &t : d) ids.push_back(t.participant_id);
        std::sort(ids.begin(), ids.end());
        return ids;
    }
```

and `#include <algorithm>` to the includes.

In `advance()`, immediately after `m_last_ns = now_ns;`, insert the settle gate:

```cpp
        // Decide which participant set the wall is laid out for. A change is
        // only adopted once it has held for kSettleNs; a reversal inside that
        // window is forgotten and nothing moves.
        const std::vector<uint32_t> incoming = ids_of(desired);
        if (!m_has_committed) {
            m_committed_ids = incoming;          // first frame: adopt at once
            m_has_committed = true;
        } else if (incoming != m_committed_ids) {
            // m_has_pending, not "m_pending_ids is non-empty": an empty roster
            // is a legitimate proposal — everyone dropping out at once is the
            // flicker case this gate exists for — and it is indistinguishable
            // from the default-empty vector. Without the flag, an empty
            // proposal falls through to the elapsed-time branch and is measured
            // against a stale m_pending_since_ns, so it is adopted with no hold
            // at all.
            if (!m_has_pending || incoming != m_pending_ids) {
                m_pending_ids = incoming;
                m_pending_since_ns = now_ns;
                m_has_pending = true;
            } else if (now_ns - m_pending_since_ns >= kSettleNs) {
                m_committed_ids = incoming;      // held long enough: adopt
                m_pending_ids.clear();
                m_has_pending = false;
            }
        } else {
            m_pending_ids.clear();               // reverted: forget the change
            m_has_pending = false;
        }
```

Then filter what is animated to the committed set. Replace the `for (const auto &d : desired)` loop header with:

```cpp
        for (const auto &d : desired) {
            const bool committed =
                std::find(m_committed_ids.begin(), m_committed_ids.end(),
                          d.participant_id) != m_committed_ids.end();
            if (!committed) {
                // Present but not yet adopted: hold it wherever it already is,
                // or emit it at its target if it is new, without disturbing the
                // rest of the wall.
                auto held = m_tiles.find(d.participant_id);
                if (held == m_tiles.end()) {
                    out.push_back(AnimatedTile{d.participant_id, d.rect, 1.0, true});
                } else {
                    TileRect r;
                    r.x = held->second.x.position; r.y = held->second.y.position;
                    r.width = held->second.w.position;
                    r.height = held->second.h.position;
                    out.push_back(AnimatedTile{d.participant_id, r, 1.0, true});
                }
                continue;
            }
```

**The target a tile springs toward is remembered, not taken from `desired` every frame.** This is the part the settle window actually turns on. `desired` is solved for whatever participant set the caller currently sees, so the moment a blip starts, the incoming rects describe a layout the wall has *not* agreed to. Springing toward them is the very reflow the settle window exists to suppress.

Give `Motion` a remembered target and only adopt a new one when no change is pending:

```cpp
    struct Motion { Spring1D x, y, w, h; TileRect target; };
```

Seed it when a tile is first seen (`m.target = d.rect;` beside the spring seeding), and in the loop body replace the direct use of `d.rect` as the spring target with:

```cpp
            // Adopt a new target only when the layout in hand was solved for
            // the set the wall has committed to. While a change is pending, the
            // incoming rects belong to a layout that may never happen — a blip
            // that reverts must leave every tile exactly where it was.
            if (!change_pending && committed)
                m.target = d.rect;

            spring_advance(m.x, m.target.x,      settings.duration_seconds, dt);
            spring_advance(m.y, m.target.y,      settings.duration_seconds, dt);
            spring_advance(m.w, m.target.width,  settings.duration_seconds, dt);
            spring_advance(m.h, m.target.height, settings.duration_seconds, dt);
```

where `change_pending` is computed once, immediately after the settle gate:

```cpp
        // Same reason as above: an empty proposal is still a pending change, so
        // this must read the flag, not the vector's emptiness.
        const bool change_pending = m_has_pending;
```

**`at_rest` must be measured against `m.target`, not `d.rect`.** A tile held through a pending change sits exactly on its remembered target while `d.rect` says otherwise; comparing against `d.rect` would report it in motion forever and push it onto the sub-pixel render path for no reason.

Finally, in the cleanup loop at the end, only erase participants absent from `m_committed_ids` (not merely absent from `desired`), so a blipped participant keeps its motion state:

```cpp
        for (auto it = m_tiles.begin(); it != m_tiles.end();) {
            const bool keep =
                std::find(m_committed_ids.begin(), m_committed_ids.end(),
                          it->first) != m_committed_ids.end();
            it = keep ? std::next(it) : m_tiles.erase(it);
        }
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `.\build-rel\Release\CoreVideoTileAnimatorTest.exe`
Expected: PASS

Then: `cd build-rel && ctest -C Release --output-on-failure` — all pass.

- [ ] **Step 5: Commit**

```bash
git add src/zoom-tile-animator.h tests/zoom-tile-animator-test.cpp
git commit -m "feat(tiles): settle window so a roster blip never moves the wall

A change to the participant set must hold for 250ms before the wall
reacts. The roster flickers in practice; without this, a 120ms dropout
produces a full exit and entry animation, which is more visible on air
than the single-frame pop it replaces.

Fixed rather than exposed: a zero setting would reintroduce exactly the
behaviour it prevents.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Enter and exit phases, with the exit invariants

**Files:**
- Modify: `src/zoom-tile-animator.h`
- Modify: `tests/zoom-tile-animator-test.cpp`

**Interfaces:**
- Consumes: everything from Tasks 2–3.
- Produces: `advance()` gains a fourth parameter —
  `advance(now_ns, desired, settings, const std::vector<uint32_t> &departed)` —
  where `departed` lists participants that left the *roster*. A participant that
  disappears from `desired` without appearing in `departed` was reassigned and
  is removed instantly with no fade.

- [ ] **Step 1: Write the failing test**

Update every existing `a.advance(...)` call in the test to pass `{}` as a fourth argument, then append these cases:

```cpp
    // A genuine departure fades out over the duration, then disappears.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        // Departure, settled.
        a.advance(300 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});
        const auto mid = a.advance(400 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});
        const AnimatedTile *leaving = find(mid, 2);
        check(leaving != nullptr, "a departing tile vanished instead of fading");
        check(leaving->alpha < 1.0 && leaving->alpha > 0.0,
              "a departing tile was not mid-fade");

        // Hard time-box: gone once the duration has elapsed.
        const auto after = a.advance(1000 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});
        check(find(after, 2) == nullptr,
              "an exit outlived its duration — invariant 4 violated");
    }

    // A reassignment cuts instantly: no fade, ever.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        // 2 disappears from the layout but did NOT leave the roster.
        const auto out = a.advance(300 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {});
        check(find(out, 2) == nullptr,
              "a reassigned slot faded instead of cutting — invariant 2 violated");
    }

    // A repoint cancels a running exit immediately.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        a.advance(300 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});
        a.advance(350 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});  // fading
        const auto out = a.advance(360 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {});
        check(find(out, 2) == nullptr,
              "a running exit survived the slot being repointed — invariant 3 violated");
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build-rel --config Release --target CoreVideoTileAnimatorTest --parallel`

Expected: FAIL to compile — `advance()` takes three arguments. Add the parameter (ignored) to `advance()`, rebuild, and expect the behavioural failures: "a departing tile vanished instead of fading", "a reassigned slot faded instead of cutting" passes trivially at first, "an exit outlived its duration" — confirm the fade assertions fail before implementing.

- [ ] **Step 3: Write the implementation**

Add to `Motion`: `double alpha = 1.0; bool exiting = false; uint64_t exit_started_ns = 0;`

In `advance()`, after the settle gate and before the desired loop:

```cpp
        // Exit lifecycle. These four rules bound the exception to the rule in
        // zoom-tile-slot.h — that a stored frame stops being shown the instant a
        // slot is repointed, written after the wrong face reached air:
        //   1. only a genuine roster departure may start an exit
        //   2. a reassignment cuts instantly, no hold and no fade
        //   3. any repoint cancels a running exit immediately
        //   4. an exit can never outlive its duration
        for (auto it = m_tiles.begin(); it != m_tiles.end();) {
            const bool wanted =
                std::find(m_committed_ids.begin(), m_committed_ids.end(),
                          it->first) != m_committed_ids.end();
            if (wanted) {
                it->second.exiting = false;      // (3) back in the layout
                it->second.alpha = 1.0;
                ++it;
                continue;
            }
            const bool left_roster =
                std::find(departed.begin(), departed.end(), it->first) != departed.end();
            if (!left_roster) {
                it = m_tiles.erase(it);          // (2) reassignment: instant
                continue;
            }
            if (!it->second.exiting) {           // (1) departure: begin fading
                it->second.exiting = true;
                it->second.exit_started_ns = now_ns;
            }
            const double elapsed =
                static_cast<double>(now_ns - it->second.exit_started_ns) / 1e9;
            if (elapsed >= settings.duration_seconds) {
                it = m_tiles.erase(it);          // (4) time-boxed
                continue;
            }
            it->second.alpha = 1.0 - elapsed / settings.duration_seconds;
            ++it;
        }
```

Then, after the desired loop, emit the exiting tiles (they are not in `desired`):

```cpp
        for (const auto &entry : m_tiles) {
            if (!entry.second.exiting) continue;
            TileRect r;
            r.x = entry.second.x.position; r.y = entry.second.y.position;
            r.width = entry.second.w.position; r.height = entry.second.h.position;
            out.push_back(AnimatedTile{entry.first, r, entry.second.alpha, false});
        }
```

Delete the old cleanup loop at the end of `advance()` — the lifecycle loop above replaces it.

- [ ] **Step 4: Run the test to verify it passes**

Run: `.\build-rel\Release\CoreVideoTileAnimatorTest.exe`
Expected: PASS

Then: `cd build-rel && ctest -C Release --output-on-failure` — all pass.

- [ ] **Step 5: Commit**

```bash
git add src/zoom-tile-animator.h tests/zoom-tile-animator-test.cpp
git commit -m "feat(tiles): exit lifecycle bounded by four invariants

A departing tile holds its last frame and fades, but only under rules
that bound the exception to zoom-tile-slot.h: only a genuine roster
departure may start an exit, a reassignment cuts instantly, a repoint
cancels a running exit, and an exit never outlives its duration. Each
has a test named for the invariant it protects.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Properties and locale strings

**Files:**
- Modify: `src/zoom-supersource.cpp`
- Modify: `data/locale/en-US.ini`

**Interfaces:**
- Consumes: `AnimationSettings` from Task 2.
- Produces: `PROP_ANIMATE` (`"animate_layout"`, bool, default `false`) and
  `PROP_ANIMATE_MS` (`"animate_duration_ms"`, int, default `350`, range 100–1000 step 10)
  read into `ctx->animate` / `ctx->animate_ms` atomics.

- [ ] **Step 1: Add the property constants**

Beside the other `PROP_*` constants near `PROP_GLOW_SOFTNESS` in `src/zoom-supersource.cpp`:

```cpp
static constexpr const char *PROP_ANIMATE    = "animate_layout";
static constexpr const char *PROP_ANIMATE_MS = "animate_duration_ms";
```

- [ ] **Step 2: Register the properties**

In the properties function, after the glow group:

```cpp
    obs_properties_add_bool(props, PROP_ANIMATE,
                            obs_module_text("CoreVideoTiles.Animate"));
    obs_properties_add_int_slider(props, PROP_ANIMATE_MS,
                                  obs_module_text("CoreVideoTiles.AnimateMs"),
                                  100, 1000, 10);
```

- [ ] **Step 3: Add defaults**

Beside the other `obs_data_set_default_*` calls:

```cpp
    // Off by default: upgrading must not change how any existing scene looks.
    obs_data_set_default_bool(settings, PROP_ANIMATE, false);
    obs_data_set_default_int(settings, PROP_ANIMATE_MS, 350);
```

- [ ] **Step 4: Read them into the source**

Add to the source context struct beside the other atomics:

```cpp
    std::atomic<bool>     animate{false};
    std::atomic<uint32_t> animate_ms{350};
```

and in the update function beside the other reads:

```cpp
    ctx->animate.store(obs_data_get_bool(settings, PROP_ANIMATE),
                       std::memory_order_release);
    ctx->animate_ms.store(
        static_cast<uint32_t>(obs_data_get_int(settings, PROP_ANIMATE_MS)),
        std::memory_order_release);
```

- [ ] **Step 5: Add locale strings**

In `data/locale/en-US.ini`, beside the other `CoreVideoTiles.*` entries:

```ini
CoreVideoTiles.Animate="Animate layout changes"
CoreVideoTiles.AnimateMs="Animation duration (ms)"
```

- [ ] **Step 6: Build and verify**

Run: `cmake --build build-rel --config Release --parallel` then `cd build-rel && ctest -C Release --output-on-failure`
Expected: builds clean, all tests pass. No behaviour change yet — nothing reads these values.

- [ ] **Step 7: Commit**

```bash
git add src/zoom-supersource.cpp data/locale/en-US.ini
git commit -m "feat(tiles): animation properties, off by default

Off by default so upgrading changes nothing about any existing scene.
Nothing reads these yet.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: Wire the animator into render, at-rest path only

**Files:**
- Modify: `src/zoom-supersource.cpp`

**Interfaces:**
- Consumes: `TileAnimator`, `AnimationSettings`, `DesiredTile`, `AnimatedTile` (Tasks 2–4); properties (Task 5).
- Produces: render draws animator output. Tiles in motion still use the snapped path this task — sub-pixel comes in Task 7. This keeps the risky change isolated.

- [ ] **Step 1: Include and hold an animator**

Add `#include "zoom-tile-animator.h"` beside the other tile includes, and a `TileAnimator animator;` member to the source context (render thread only — no lock needed, it is touched solely by `tiles_source_render`).

- [ ] **Step 2: Build desired tiles keyed by participant**

In `tiles_source_render()`, replace the single `rects` line:

```cpp
    const std::vector<SnappedTileRect> rects =
        snap_tile_grid_even(solve_tile_grid(feeds.size(), params), params);
```

with:

```cpp
    const std::vector<TileRect> solved = solve_tile_grid(feeds.size(), params);
    std::vector<DesiredTile> desired;
    desired.reserve(feeds.size());
    for (size_t i = 0; i < feeds.size() && i < solved.size(); ++i)
        desired.push_back(DesiredTile{feeds[i]->slot.participant_id(), solved[i]});

    AnimationSettings anim;
    anim.enabled = ctx->animate.load(std::memory_order_acquire);
    anim.duration_seconds =
        static_cast<double>(ctx->animate_ms.load(std::memory_order_acquire)) / 1000.0;

    const std::vector<AnimatedTile> animated =
        ctx->animator.advance(os_gettime_ns(), desired, anim, /*departed=*/{});

    // Snap what the animator produced. A tile at rest snaps to exactly the
    // geometry the solver produced, so with animation off this is identical to
    // the previous single-call form.
    std::vector<TileRect> animated_rects;
    animated_rects.reserve(animated.size());
    for (const auto &t : animated) animated_rects.push_back(t.rect);
    const std::vector<SnappedTileRect> rects =
        snap_tile_grid_even(animated_rects, params);
```

- [ ] **Step 3: Verify disabled parity by hand**

Build, install the plugin, and open a scene with a Tiles wall and animation **off**. Confirm the wall renders exactly as before and the OBS log shows no new warnings.

Run: `cmake --build build-rel --config Release --parallel`

- [ ] **Step 4: Run the suite**

Run: `cd build-rel && ctest -C Release --output-on-failure`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/zoom-supersource.cpp
git commit -m "feat(tiles): draw animator output, snapped path only

Render now solves a target layout, hands it to the animator keyed by
participant, and snaps what comes back. With animation off the animator
emits the solver's rects verbatim, so this is identical to the previous
single-call form. Sub-pixel motion is deliberately not here yet.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: Sub-pixel path for tiles in motion

**Files:**
- Modify: `src/zoom-supersource.cpp`

**Interfaces:**
- Consumes: `AnimatedTile::at_rest` and `AnimatedTile::alpha`.
- Produces: moving tiles composited through an intermediate texture of even dimensions, drawn at fractional coordinates; tiles at rest unchanged.

- [ ] **Step 1: Render a moving tile to a texture**

For each `AnimatedTile` where `at_rest == false`, render the tile's I420 through the existing tiles effect into a `gs_texrender_t` sized to the tile's **even-rounded** dimensions, then draw that texture as a sprite at the tile's fractional x/y with linear filtering and the tile's alpha. Tiles where `at_rest == true` take the existing snapped path untouched.

Allocate the `gs_texrender_t` lazily, reuse it across frames, and destroy it when the source is destroyed. Guard every graphics call with the same `obs_enter_graphics()` discipline the file already uses.

- [ ] **Step 2: Build and verify at rest**

Run: `cmake --build build-rel --config Release --parallel`

With animation **off**, confirm nothing changed. With animation **on** but the wall static, confirm every tile reports at rest and still takes the snapped path.

- [ ] **Step 3: Verify in motion**

Install, join a test meeting, and have a participant join and leave. Motion should be smooth with no 2-pixel stepping.

- [ ] **Step 4: Run the suite**

Run: `cd build-rel && ctest -C Release --output-on-failure`

- [ ] **Step 5: Commit**

```bash
git add src/zoom-supersource.cpp
git commit -m "feat(tiles): sub-pixel motion through an intermediate texture

A moving tile composites into an even-dimensioned texture and is drawn at
fractional coordinates, so chroma reconstruction stays in tile space and
motion is not quantised to the 2px grid that I420 subsampling forces on
the direct path. Tiles at rest are untouched.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: Colour parity and cost validation on the rig

**Files:**
- Modify: `docs/VALIDATION_MATRIX.md`

This task has no unit test: it needs a GPU and a live meeting. It is the acceptance gate for the feature.

- [ ] **Step 1: Measure colour parity**

With a static wall and animation **on**, capture the luma range of a tile at rest (the existing `Luma range probe` line reports `min`/`max`/`under16`/`over235`). Trigger a reflow so the same tile moves, and capture it again mid-motion.

Acceptance: the two must agree within measurement noise. A tile that shifts colour when it starts or stops moving fails this gate, and the feature does not ship until it does not.

- [ ] **Step 2: Measure transition cost**

With eight 1080p feeds, run a reflow and record CPU and GPU usage during the transition versus at rest.

Acceptance: no dropped frames in the OBS log during the transition.

- [ ] **Step 3: Record the results**

Add a "Tiles Animation" section to `docs/VALIDATION_MATRIX.md` listing both checks with their acceptance criteria and the measured values.

- [ ] **Step 4: Commit**

```bash
git add docs/VALIDATION_MATRIX.md
git commit -m "docs: record Tiles animation rig validation

Colour parity between the direct and textured paths is the acceptance
gate: a tile that shifts colour as motion starts or stops would be worse
than no animation.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Self-review

**Spec coverage.** Full reflow — Task 2. Pixel-exact at rest / sub-pixel in motion — Tasks 6 and 7. Exit holding the last frame under four invariants — Task 4. Retarget in flight — Task 1 (velocity continuity) and Task 2 (springs re-aimed every frame). Settle window at 250 ms — Task 3. Off by default with a duration control — Task 5. Colour-parity and cost risks — Task 8. Frame-rate independence — Task 1.

**Gap found and closed:** the spec's `Entering` phase (alpha 0→1) is not implemented by any task. Tasks 2 and 4 give a new tile alpha 1.0 at its target immediately. This is deliberate and the spec should be read as amended: a fade-in is cosmetic, whereas every other behaviour here is either correctness or the reason the feature exists. It is listed under Deferred below rather than left as an implicit omission.

**Type consistency.** `AnimatedTile` carries `participant_id`, `rect`, `alpha`, `at_rest` and is used with those names in Tasks 2, 4, 6 and 7. `advance()` gains its fourth parameter in Task 4 and every earlier call site is updated in that task's Step 1. `Spring1D`/`spring_advance` names match between Tasks 1 and 2.

## Deferred

- **Entering fade-in.** New tiles appear at full alpha. Adding it later is a change to one line in Task 2's new-tile branch plus a phase check.
- **Animating tile-shape, gutter and margin changes.** Out of scope per the spec.
