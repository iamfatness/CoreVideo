# CoreVideo Tiles Participant Picker Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the tiles source's typed list of raw Zoom user IDs with a roster-driven picker: auto-fill from everyone sending video by default, with per-tile manual casting as an override.

**Architecture:** All the decision logic goes in `src/zoom-tile-fill.h`, a pure header that turns (current wall, roster, settings) into the next wall and is tested without OBS. `src/zoom-supersource.cpp` gains the properties UI and calls that resolver from two places — `update` (settings changed) and the roster callback it *already registers* (participants changed) — feeding the result into the `FeedPlan` diff that exists and works today.

**Tech Stack:** C++17, CMake, OBS Studio plugin API. Tests are plain `main()` executables returning 0/1 — **no test framework in this repo**; do not introduce gtest or Catch.

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-08-10-corevideo-tiles-participant-picker-design.md`. Read it before starting.
- **Branch:** `feat/tiles-on-main` in the worktree `C:\Users\walla\CoreVideo\cv-tiles2-wt`. Work only there.
- **Pure units must be Qt-free, OBS-free, and SDK-free.** `zoom-tile-fill.h` may include `zoom-types.h` and nothing else from the project.
- **Max tiles is 9.** Max exclude slots is 3.
- **Log prefix:** `[obs-zoom-plugin]` — match the existing `blog()` convention.
- **All user-visible strings go through `obs_module_text`** with keys in `data/locale/en-US.ini`, matching every other source in the plugin.
- **Test build loop** (verified working, 13/13 passing at time of writing):
  ```sh
  cmake -S . -B build-tests -DCOREVIDEO_BUILD_PLUGIN=OFF -DCOREVIDEO_BUILD_ENGINE=OFF -DCOREVIDEO_BUILD_SIDECAR=OFF -DBUILD_TESTING=ON
  cmake --build build-tests --config Debug
  ctest --test-dir build-tests -C Debug
  ```
- **Full plugin build loop** (needed for Tasks 2-4):
  ```powershell
  $obs = "C:/Users/walla/Documents/Codex/2026-05-14/pull-the-latest-for-this-code/build-deps/obs-studio"
  cmake -S . -B build-rel -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_PREFIX_PATH="$obs/build_x64/libobs;$obs/build_x64/frontend/api;$obs/build_x64/deps/w32-pthreads;$obs/.deps/obs-deps-2025-08-23-x64;$obs/.deps/obs-deps-qt6-2025-08-23-x64" `
    -DCMAKE_MODULE_PATH="$obs/cmake/finders" `
    -DZOOM_SDK_DIR="C:/Users/walla/Downloads/zoom-sdk-windows-7.1.5.43953/zoom-sdk-windows-7.1.5.43953/x64" `
    -DFFMPEG_ROOT="C:/ffmpeg" -DENABLE_FFMPEG_HW_ACCEL=ON
  cmake --build build-rel --config Release --parallel
  ```
- **Installing into OBS needs elevation and OBS fully closed.** Use `C:\Users\walla\AppData\Local\Temp\claude\C--Users-walla\ee90dd79-2091-49db-b5b0-b49223466efa\scratchpad\install-tiles-build.ps1` via `Start-Process powershell -Verb RunAs`; the owner accepts the UAC prompt. It backs up to `C:\Users\walla\CoreVideo\backup-v0.1.36` and SHA256-verifies both files.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/zoom-tile-fill.h` | Pure resolver: previous wall + roster + params → next wall | 1 |
| `tests/tile-fill-test.cpp` | Resolver unit tests | 1 |
| `CMakeLists.txt` (modify) | Register `CoreVideoTileFillTest` | 1 |
| `data/locale/en-US.ini` (modify) | User-visible strings for the picker | 2 |
| `src/zoom-supersource.cpp` (modify) | Properties UI, settings parsing, roster-driven reflow | 2, 3 |

---

### Task 1: The fill resolver

**Files:**
- Create: `src/zoom-tile-fill.h`
- Test: `tests/tile-fill-test.cpp`
- Modify: `CMakeLists.txt` (inside the existing `if(BUILD_TESTING)` block, immediately after the `CoreVideoTileSlotTest` block)

**Interfaces:**
- Consumes: `ParticipantInfo` from `src/zoom-types.h` (fields used: `user_id`, `has_video`).
- Produces:
  ```cpp
  enum class TileFillMode { Auto = 0, Manual = 1 };
  struct TileFillParams {
      TileFillMode          mode      = TileFillMode::Auto;
      std::size_t           max_tiles = 9;
      std::vector<uint32_t> excluded;
      std::vector<uint32_t> manual;
  };
  std::vector<uint32_t> resolve_tile_assignments(
      const std::vector<uint32_t> &previous,
      const std::vector<ParticipantInfo> &roster,
      const TileFillParams &params);
  ```

**Why `previous` is a parameter.** The resolver is pure, but the wall needs memory: participants already shown must keep their positions, or an SDK roster reordering reshuffles every face mid-broadcast. Passing the current wall in keeps that stability rule testable without giving the resolver state.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/tile-fill-test.cpp
// Covers resolve_tile_assignments(), which decides who appears on the tile
// wall and in what order. The stability rule (case 1) is the one that matters
// most on air: without it, any roster reordering by the SDK moves every face.

#include "zoom-tile-fill.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

ParticipantInfo person(uint32_t id, bool has_video)
{
    ParticipantInfo p;
    p.user_id = id;
    p.display_name = "User " + std::to_string(id);
    p.has_video = has_video;
    return p;
}

bool expect(const char *name, const std::vector<uint32_t> &got,
            const std::vector<uint32_t> &want)
{
    if (got == want) return true;
    std::cerr << name << ": expected [";
    for (uint32_t id : want) std::cerr << id << " ";
    std::cerr << "], got [";
    for (uint32_t id : got) std::cerr << id << " ";
    std::cerr << "]\n";
    return false;
}

TileFillParams auto_params()
{
    TileFillParams p;
    p.mode = TileFillMode::Auto;
    p.max_tiles = 9;
    return p;
}

// A newcomer must land at the end. If the resolver rebuilt from roster order
// instead, 30 would sort into the middle and two faces would swap positions.
bool test_auto_keeps_incumbent_order()
{
    const std::vector<uint32_t> previous{20, 10};
    const std::vector<ParticipantInfo> roster{
        person(10, true), person(20, true), person(30, true)};
    return expect("incumbent order",
                  resolve_tile_assignments(previous, roster, auto_params()),
                  {20, 10, 30});
}

bool test_auto_drops_camera_off_and_closes_gap()
{
    const std::vector<uint32_t> previous{10, 20, 30};
    const std::vector<ParticipantInfo> roster{
        person(10, true), person(20, false), person(30, true)};
    return expect("camera off drops",
                  resolve_tile_assignments(previous, roster, auto_params()),
                  {10, 30});
}

// Incumbents win the contest for scarce slots; the newest arrival is cut.
bool test_auto_honors_max_tiles()
{
    TileFillParams params = auto_params();
    params.max_tiles = 2;
    const std::vector<uint32_t> previous{10, 20};
    const std::vector<ParticipantInfo> roster{
        person(10, true), person(20, true), person(30, true)};
    return expect("max tiles",
                  resolve_tile_assignments(previous, roster, params), {10, 20});
}

bool test_auto_honors_excludes_and_ignores_zeros()
{
    TileFillParams params = auto_params();
    params.excluded = {20, 0, 0};  // empty exclude slots are zeros
    const std::vector<ParticipantInfo> roster{
        person(10, true), person(20, true), person(30, true)};
    return expect("excludes", resolve_tile_assignments({}, roster, params),
                  {10, 30});
}

// A departed participant must not linger just because they were on the wall.
bool test_auto_empty_roster_clears_the_wall()
{
    return expect("empty roster",
                  resolve_tile_assignments({10, 20}, {}, auto_params()), {});
}

bool test_manual_passes_through_and_drops_holes()
{
    TileFillParams params;
    params.mode = TileFillMode::Manual;
    params.max_tiles = 9;
    params.manual = {30, 0, 10, 0};  // 0 == "None" in the dropdowns
    const std::vector<ParticipantInfo> roster{person(10, true), person(30, true)};
    return expect("manual passthrough",
                  resolve_tile_assignments({}, roster, params), {30, 10});
}

// Casting is a decision, not a liveness query: a cast participant with their
// camera off keeps the slot the operator gave them.
bool test_manual_keeps_camera_off_participant()
{
    TileFillParams params;
    params.mode = TileFillMode::Manual;
    params.max_tiles = 9;
    params.manual = {10, 20};
    const std::vector<ParticipantInfo> roster{
        person(10, true), person(20, false)};
    return expect("manual keeps camera-off",
                  resolve_tile_assignments({}, roster, params), {10, 20});
}

// Two dropdowns pointed at the same person must not open two subscriptions.
bool test_duplicates_collapse_in_both_modes()
{
    TileFillParams manual;
    manual.mode = TileFillMode::Manual;
    manual.max_tiles = 9;
    manual.manual = {10, 10, 20};
    if (!expect("manual duplicates",
                resolve_tile_assignments({}, {}, manual), {10, 20}))
        return false;

    const std::vector<ParticipantInfo> roster{person(10, true)};
    return expect("auto duplicates",
                  resolve_tile_assignments({10, 10}, roster, auto_params()),
                  {10});
}

}  // namespace

int main()
{
    if (!test_auto_keeps_incumbent_order()) return 1;
    if (!test_auto_drops_camera_off_and_closes_gap()) return 1;
    if (!test_auto_honors_max_tiles()) return 1;
    if (!test_auto_honors_excludes_and_ignores_zeros()) return 1;
    if (!test_auto_empty_roster_clears_the_wall()) return 1;
    if (!test_manual_passes_through_and_drops_holes()) return 1;
    if (!test_manual_keeps_camera_off_participant()) return 1;
    if (!test_duplicates_collapse_in_both_modes()) return 1;

    std::cout << "tile-fill: all tests passed\n";
    return 0;
}
```

- [ ] **Step 2: Register the test and run it to verify it fails**

Add inside the existing `if(BUILD_TESTING)` block in `CMakeLists.txt`, immediately after the `add_test(NAME CoreVideoTileSlot ...)` lines:

```cmake
    add_executable(CoreVideoTileFillTest
        tests/tile-fill-test.cpp
    )
    target_include_directories(CoreVideoTileFillTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileFill
             COMMAND CoreVideoTileFillTest)
```

Run:
```sh
cmake -S . -B build-tests -DCOREVIDEO_BUILD_PLUGIN=OFF -DCOREVIDEO_BUILD_ENGINE=OFF -DCOREVIDEO_BUILD_SIDECAR=OFF -DBUILD_TESTING=ON
cmake --build build-tests --config Debug
```
Expected: FAIL — `zoom-tile-fill.h` does not exist.

- [ ] **Step 3: Write the resolver**

```cpp
// src/zoom-tile-fill.h
#pragma once

#include "zoom-types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// How the tile wall decides who it shows.
enum class TileFillMode {
    Auto   = 0,  // everyone sending video, minus exclusions
    Manual = 1,  // exactly who the operator cast, in slot order
};

struct TileFillParams {
    TileFillMode          mode      = TileFillMode::Auto;
    std::size_t           max_tiles = 9;
    std::vector<uint32_t> excluded;  // Auto only; zeros are empty slots
    std::vector<uint32_t> manual;    // Manual only; zeros are empty slots
};

// Decides the wall's next assignment list.
//
// `previous` is what is on the wall right now, and it is what makes the result
// stable: participants who are still eligible keep their existing positions,
// and only new arrivals are appended. Rebuilding purely from roster order would
// let any SDK reordering move every face at once, which on air is
// indistinguishable from a bug.
//
// Duplicate and zero ids are dropped in both modes. An empty result is legal
// and means an empty wall.
inline std::vector<uint32_t> resolve_tile_assignments(
    const std::vector<uint32_t> &previous,
    const std::vector<ParticipantInfo> &roster,
    const TileFillParams &params)
{
    std::vector<uint32_t> out;
    if (params.max_tiles == 0) return out;
    out.reserve(params.max_tiles);

    const auto push_unique = [&out](uint32_t id) {
        if (id == 0) return;
        if (std::find(out.begin(), out.end(), id) != out.end()) return;
        out.push_back(id);
    };

    if (params.mode == TileFillMode::Manual) {
        // The roster is deliberately not consulted: an operator who cast a
        // tile keeps it even while that participant's camera is off.
        for (const uint32_t id : params.manual) push_unique(id);
        if (out.size() > params.max_tiles) out.resize(params.max_tiles);
        return out;
    }

    const auto eligible = [&params, &roster](uint32_t id) {
        if (id == 0) return false;
        if (std::find(params.excluded.begin(), params.excluded.end(), id) !=
            params.excluded.end())
            return false;
        const auto it = std::find_if(
            roster.begin(), roster.end(),
            [id](const ParticipantInfo &p) { return p.user_id == id; });
        return it != roster.end() && it->has_video;
    };

    // Incumbents first, in their existing order...
    for (const uint32_t id : previous)
        if (eligible(id)) push_unique(id);
    // ...then newcomers, in roster order.
    for (const ParticipantInfo &p : roster)
        if (eligible(p.user_id)) push_unique(p.user_id);

    if (out.size() > params.max_tiles) out.resize(params.max_tiles);
    return out;
}
```

- [ ] **Step 4: Run tests to verify they pass**

```sh
cmake --build build-tests --config Debug
ctest --test-dir build-tests -C Debug --output-on-failure
```
Expected: PASS, 14/14 tests (the 13 existing plus `CoreVideoTileFill`).

- [ ] **Step 5: Commit**

```bash
git add src/zoom-tile-fill.h tests/tile-fill-test.cpp CMakeLists.txt
git commit -m "feat(tiles): resolve wall assignments from the live roster"
```

---

### Task 2: Properties UI

**Files:**
- Modify: `data/locale/en-US.ini` (append to the end)
- Modify: `src/zoom-supersource.cpp` — `tiles_source_get_properties` (`src/zoom-supersource.cpp:751`) and `tiles_source_get_defaults` (`src/zoom-supersource.cpp:745`)

**Interfaces:**
- Consumes: `TileFillMode` from Task 1; `ZoomEngineClient::instance().roster()` returning `std::vector<ParticipantInfo>`.
- Produces: the settings keys Task 3 reads — `fill_mode` (int), `max_tiles` (int), `exclude_1`…`exclude_3` (int), `tile_1`…`tile_9` (int). The `participants` string array is gone.

**Pattern to follow:** `src/zoom-participant-audio-source.cpp:331-362` builds exactly this kind of roster combo — a `None` entry at 0, then `Name (id)` labels with `[video]` / `[talking]` markers, plus a refresh button whose callback returns `true` to force a rebuild. Mirror it.

**Why the tiles source stops hardcoding English.** Every other source routes labels through `obs_module_text`; tiles was the exception. Since this task rewrites the whole property set anyway, the strings move to the locale file rather than adding nine more hardcoded ones.

- [ ] **Step 1: Add the locale strings**

Append to `data/locale/en-US.ini`:

```ini
CoreVideoTiles.FillMode="Fill mode"
CoreVideoTiles.FillModeAuto="Auto - everyone with video"
CoreVideoTiles.FillModeManual="Manual - choose per tile"
CoreVideoTiles.MaxTiles="Maximum tiles"
CoreVideoTiles.Exclude="Never show"
CoreVideoTiles.Tile="Tile"
CoreVideoTiles.NoParticipant="- none -"
CoreVideoTiles.CanvasWidth="Canvas width"
CoreVideoTiles.CanvasHeight="Canvas height"
CoreVideoTiles.RefreshParticipants="Refresh participant list"
```

- [ ] **Step 2: Add the property-name helpers and the roster list builder**

Insert these above `tiles_source_get_defaults` in `src/zoom-supersource.cpp`:

```cpp
// Property keys. Tile and exclude slots are numbered from 1 to match their
// labels, so scene files stay readable.
static constexpr const char *PROP_FILL_MODE = "fill_mode";
static constexpr const char *PROP_MAX_TILES = "max_tiles";
static constexpr std::size_t kMaxTileSlots  = 9;
static constexpr std::size_t kMaxExcludes   = 3;

static std::string tile_prop_name(std::size_t slot)
{
    return "tile_" + std::to_string(slot);
}

static std::string exclude_prop_name(std::size_t slot)
{
    return "exclude_" + std::to_string(slot);
}

// One participant chooser, built the same way every other CoreVideo source
// builds one: a "none" entry at 0, then the live roster.
static void add_roster_entries(obs_property_t *list)
{
    obs_property_list_add_int(list, obs_module_text("CoreVideoTiles.NoParticipant"), 0);
    for (const auto &p : ZoomEngineClient::instance().roster()) {
        std::string label = p.display_name.empty()
            ? "ID " + std::to_string(p.user_id)
            : p.display_name + " (" + std::to_string(p.user_id) + ")";
        if (p.has_video) label += " [video]";
        obs_property_list_add_int(list, label.c_str(),
                                  static_cast<long long>(p.user_id));
    }
}

// Only one of the two control groups is ever relevant, so hide the other
// rather than leaving dead dropdowns on screen.
static bool tiles_fill_mode_modified(obs_properties_t *props, obs_property_t *,
                                     obs_data_t *settings)
{
    const bool manual = obs_data_get_int(settings, PROP_FILL_MODE) ==
                        static_cast<long long>(TileFillMode::Manual);

    obs_property_set_visible(obs_properties_get(props, PROP_MAX_TILES), !manual);
    for (std::size_t i = 1; i <= kMaxExcludes; ++i) {
        obs_property_set_visible(
            obs_properties_get(props, exclude_prop_name(i).c_str()), !manual);
    }
    for (std::size_t i = 1; i <= kMaxTileSlots; ++i) {
        obs_property_set_visible(
            obs_properties_get(props, tile_prop_name(i).c_str()), manual);
    }
    return true;  // properties changed: redraw the dialog
}
```

Add `#include "zoom-tile-fill.h"` and `#include "zoom-engine-client.h"` to the include block at the top of the file if they are not already present.

- [ ] **Step 3: Replace the property set**

Replace the body of `tiles_source_get_properties` (`src/zoom-supersource.cpp:751`) — the `obs_properties_add_editable_list(props, "participants", ...)` call and everything through the two canvas ints — with:

```cpp
static obs_properties_t *tiles_source_get_properties(void *)
{
    obs_properties_t *props = obs_properties_create();

    obs_property_t *mode = obs_properties_add_list(props, PROP_FILL_MODE,
        obs_module_text("CoreVideoTiles.FillMode"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode, obs_module_text("CoreVideoTiles.FillModeAuto"),
                              static_cast<long long>(TileFillMode::Auto));
    obs_property_list_add_int(mode, obs_module_text("CoreVideoTiles.FillModeManual"),
                              static_cast<long long>(TileFillMode::Manual));
    obs_property_set_modified_callback(mode, tiles_fill_mode_modified);

    obs_properties_add_int(props, PROP_MAX_TILES,
        obs_module_text("CoreVideoTiles.MaxTiles"), 1,
        static_cast<int>(kMaxTileSlots), 1);

    for (std::size_t i = 1; i <= kMaxExcludes; ++i) {
        const std::string name = exclude_prop_name(i);
        const std::string label =
            std::string(obs_module_text("CoreVideoTiles.Exclude")) + " " +
            std::to_string(i);
        add_roster_entries(obs_properties_add_list(props, name.c_str(),
            label.c_str(), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT));
    }

    for (std::size_t i = 1; i <= kMaxTileSlots; ++i) {
        const std::string name = tile_prop_name(i);
        const std::string label =
            std::string(obs_module_text("CoreVideoTiles.Tile")) + " " +
            std::to_string(i);
        add_roster_entries(obs_properties_add_list(props, name.c_str(),
            label.c_str(), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT));
    }

    obs_properties_add_int(props, "canvas_width",
        obs_module_text("CoreVideoTiles.CanvasWidth"),
        kMinCanvasW, kMaxCanvasW, 2);
    obs_properties_add_int(props, "canvas_height",
        obs_module_text("CoreVideoTiles.CanvasHeight"),
        kMinCanvasH, kMaxCanvasH, 2);

    obs_properties_add_button(props, "btn_refresh",
        obs_module_text("CoreVideoTiles.RefreshParticipants"),
        [](obs_properties_t *, obs_property_t *, void *) -> bool { return true; });

    return props;
}
```

- [ ] **Step 4: Set the defaults**

Replace `tiles_source_get_defaults` (`src/zoom-supersource.cpp:745`) with:

```cpp
static void tiles_source_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "canvas_width",  1920);
    obs_data_set_default_int(settings, "canvas_height", 1080);
    obs_data_set_default_int(settings, PROP_FILL_MODE,
                             static_cast<long long>(TileFillMode::Auto));
    obs_data_set_default_int(settings, PROP_MAX_TILES,
                             static_cast<long long>(kMaxTileSlots));
    // Every chooser defaults to "none"; Auto mode fills the wall on its own.
    for (std::size_t i = 1; i <= kMaxExcludes; ++i)
        obs_data_set_default_int(settings, exclude_prop_name(i).c_str(), 0);
    for (std::size_t i = 1; i <= kMaxTileSlots; ++i)
        obs_data_set_default_int(settings, tile_prop_name(i).c_str(), 0);
}
```

- [ ] **Step 5: Build the plugin**

Run the full plugin build loop from Global Constraints.
Expected: builds clean. `tiles_source_update` still references the removed `participants` array at this point — that is Task 3's job and it still compiles, because the key simply returns an empty array.

- [ ] **Step 6: Commit**

```bash
git add data/locale/en-US.ini src/zoom-supersource.cpp
git commit -m "feat(tiles): pick participants from the roster instead of typing ids"
```

---

### Task 3: Wire settings and roster changes into the wall

**Files:**
- Modify: `src/zoom-supersource.cpp` — `struct tiles_source` (`src/zoom-supersource.cpp:98`), `tiles_source_update` (`src/zoom-supersource.cpp:606`), `tiles_source_create` (`src/zoom-supersource.cpp:656`)

**Interfaces:**
- Consumes: `resolve_tile_assignments`, `TileFillParams`, `TileFillMode` from Task 1; the settings keys from Task 2; `plan_feeds_locked` (`src/zoom-supersource.cpp:291`) and `execute_feed_plan` (`src/zoom-supersource.cpp:321`), both unchanged.
- Produces: `static void apply_assignments(tiles_source *ctx)`.

**The roster callback already exists.** `tiles_source_create` registers one that calls `resubscribe_silent_feeds`, guarded by `TilesCallbackGate` for lifetime safety. This task extends that existing callback — it does **not** add a second one, and `tiles_source_destroy` already clears the gate and removes the callback, so no teardown changes are needed.

**Locking order matters.** `ZoomEngineClient::roster()` takes the engine client's own lock, so it must be called *before* `ctx->mutex` is taken, never under it. The established order is: `engine_mutex` → read roster → `ctx->mutex` → resolve and plan → release `ctx->mutex` → execute.

- [ ] **Step 1: Add the cached fill params to the source struct**

In `struct tiles_source` (`src/zoom-supersource.cpp:98`), add to the block guarded by `mutex`, immediately after `std::vector<uint32_t> participants;`:

```cpp
    // The settings the resolver needs. Cached because the roster callback runs
    // with no obs_data_t in hand — it only knows the participants changed.
    TileFillParams fill_params;
```

- [ ] **Step 2: Add the apply function**

Insert immediately above `tiles_source_update` (`src/zoom-supersource.cpp:606`):

```cpp
// Recomputes the wall from the cached settings plus the live roster, and
// performs whatever engine work the change implies. Safe to call from the
// settings path and from the roster callback.
static void apply_assignments(tiles_source *ctx)
{
    std::lock_guard<std::mutex> engine_lock(ctx->engine_mutex);

    // Fetched before ctx->mutex: roster() takes the engine client's lock, and
    // taking them in the other order anywhere would invite a deadlock.
    const std::vector<ParticipantInfo> roster =
        ZoomEngineClient::instance().roster();

    FeedPlan plan;
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        std::vector<uint32_t> next =
            resolve_tile_assignments(ctx->participants, roster, ctx->fill_params);
        if (ctx->participants == next) return;
        ctx->participants.swap(next);
        plan = plan_feeds_locked(ctx);
    }
    execute_feed_plan(plan);
}
```

- [ ] **Step 3: Rewrite update to parse the new settings**

Replace `tiles_source_update` (`src/zoom-supersource.cpp:606-654`) in full:

```cpp
static void tiles_source_update(void *data, obs_data_t *settings)
{
    auto *ctx = static_cast<tiles_source *>(data);

    TileFillParams params;
    params.mode = obs_data_get_int(settings, PROP_FILL_MODE) ==
                          static_cast<long long>(TileFillMode::Manual)
                      ? TileFillMode::Manual
                      : TileFillMode::Auto;

    // Clamp: scene files are hand-editable and obs_data_get_int returns int64.
    const int64_t raw_max = obs_data_get_int(settings, PROP_MAX_TILES);
    params.max_tiles = static_cast<std::size_t>(
        std::min<int64_t>(std::max<int64_t>(raw_max, 1),
                          static_cast<int64_t>(kMaxTileSlots)));

    // A chooser value outside the 32-bit Zoom id range cannot name a real
    // participant, so it is dropped rather than wrapped by the cast.
    const auto read_id = [settings](const std::string &key) -> uint32_t {
        const int64_t raw = obs_data_get_int(settings, key.c_str());
        if (raw <= 0 || raw > 0xFFFFFFFFll) return 0;
        return static_cast<uint32_t>(raw);
    };

    for (std::size_t i = 1; i <= kMaxExcludes; ++i)
        params.excluded.push_back(read_id(exclude_prop_name(i)));
    for (std::size_t i = 1; i <= kMaxTileSlots; ++i)
        params.manual.push_back(read_id(tile_prop_name(i)));

    const int64_t raw_w = obs_data_get_int(settings, "canvas_width");
    const int64_t raw_h = obs_data_get_int(settings, "canvas_height");
    const uint32_t width = static_cast<uint32_t>(
        std::min<int64_t>(std::max<int64_t>(raw_w, kMinCanvasW), kMaxCanvasW)) & ~1u;
    const uint32_t height = static_cast<uint32_t>(
        std::min<int64_t>(std::max<int64_t>(raw_h, kMinCanvasH), kMaxCanvasH)) & ~1u;
    ctx->canvas_width.store(width, std::memory_order_release);
    ctx->canvas_height.store(height, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->fill_params = std::move(params);
    }
    apply_assignments(ctx);
}
```

- [ ] **Step 4: Reflow the wall when the roster changes**

In `tiles_source_create` (`src/zoom-supersource.cpp:664`), change the existing roster callback body so it recomputes the wall before retrying silent feeds:

```cpp
    ZoomEngineClient::instance().add_roster_callback(ctx,
        [ctx, gate = ctx->gate]() {
            std::lock_guard<std::mutex> callback_lock(gate->mtx);
            if (!gate->alive) return;
            // Someone joined, left, or toggled their camera: in Auto mode the
            // wall's membership may have changed. Reflow first, then retry any
            // slot that is still silent under its current assignment.
            apply_assignments(ctx);
            resubscribe_silent_feeds(ctx);
        });
```

- [ ] **Step 5: Build and run the full suite**

```sh
cmake --build build-rel --config Release --parallel
ctest --test-dir build-rel -C Release --output-on-failure
```
Expected: builds clean, 23/23 tests pass (22 existing plus `CoreVideoTileFill`).

- [ ] **Step 6: Commit**

```bash
git add src/zoom-supersource.cpp
git commit -m "feat(tiles): reflow the wall when the roster or settings change"
```

---

### Task 4: Live verification in OBS

**Files:**
- Modify: `docs/tile-clock-findings.md` (create if absent — Phase 0 Task 3 may not have run yet)

**Interfaces:**
- Consumes: everything from Tasks 1-3.
- Produces: nothing in code. This task exists because a picker that compiles is not a picker that works, and the previous version of this UI passed every build while being unusable.

**This task requires a live Zoom meeting with at least three participants sending video.** If you are an agent without rig access, stop here and hand back.

- [ ] **Step 1: Install the build**

Close OBS completely, then run the elevated install script from Global Constraints and have the owner accept the UAC prompt. Confirm both SHA256 lines report a match before continuing — the engine changed in earlier work, so the DLL alone is not enough.

- [ ] **Step 2: Verify Auto mode**

Launch OBS, join a meeting with 3+ participants sending video, add a **CoreVideo Tiles** source. Confirm, without opening the properties dialog a second time:

1. The wall populates by itself with everyone sending video.
2. Tiles are identical in size with visibly uniform gaps.
3. A participant turning their camera off drops out and the wall closes up.
4. That participant turning their camera back on returns them to the wall.
5. Turning a camera off and on again does **not** reorder the other tiles.

Item 5 is the stability rule from Task 1. If it fails, the resolver is being passed an empty `previous` — check that `apply_assignments` reads `ctx->participants` and not a fresh vector.

- [ ] **Step 3: Verify the exclude slots**

Open properties, set `Never show 1` to one of the participants currently on the wall, and close the dialog. Confirm they leave the wall immediately and the remaining tiles close up.

- [ ] **Step 4: Verify Manual mode**

Switch `Fill mode` to Manual. Confirm the max-tiles and exclude controls disappear and nine `Tile N` choosers appear. Cast three participants in a deliberate order, and confirm:

1. The wall shows exactly those three, in that order.
2. Asking one of them to turn their camera off leaves their tile in place (rendering neutral) rather than dropping it.
3. A fifth participant assigned across five tiles produces 3-over-2 with the short row centered.

- [ ] **Step 5: Record what you saw**

Append a "Participant picker verification" section to `docs/tile-clock-findings.md` recording the date, plugin build, participant count, and the result of each check above — including anything that did not behave as described. Report what you observed, not what the plan predicted.

- [ ] **Step 6: Commit**

```bash
git add docs/tile-clock-findings.md
git commit -m "docs(tiles): record participant picker rig verification"
```

---

## Definition of Done

- `ctest --test-dir build-rel -C Release` passes, including `CoreVideoTileFill`.
- Adding a **CoreVideo Tiles** source with no configuration fills the wall from the live roster.
- No workflow requires typing a Zoom user ID anywhere.
- Camera on/off changes membership without reordering the surviving tiles.
- Manual mode casts and orders tiles explicitly and keeps camera-off assignees.
- `docs/tile-clock-findings.md` records a real rig run.

## Explicitly Not Done After This Plan

Name-tile text for no-video participants, the subscription-count-against-cap indicator, drag-to-reorder, and per-tile crop overrides. Phase 2 sync work is untouched — this changes which participants are assigned, never how their frames are timed.

The concurrent-stream cap remains the real gate on shipping Tiles: `Maximum tiles` defaulting to 9 is a UI ceiling, not a guarantee the engine can deliver nine live feeds.
