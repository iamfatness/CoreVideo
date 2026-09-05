# Framing Consumers (Subsystem 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the two consumers of the subject detector — opt-in per-tile auto-framing on the Tiles wall, and a geometric framing-advice overlay for the Virtual-Camera return feed — on top of a durable self/return-identity exclusion that stops the return feed feeding itself.

**Architecture:** Three pure header-only decision units (`zoom-self-identity.h`, `zoom-auto-frame.h`, `zoom-framing-advice.h` + `zoom-framing-overlay-layout.h`) hold every rule, and are exhaustively unit-tested on the CPU. Two thin OBS integrations consume them: the existing Tiles source gains an auto-frame path that replaces the operator's slot crop at the single existing crop insertion point (`src/zoom-supersource.cpp:1957`), and a new custom-draw source `corevideo_framing_overlay` draws solid-quad symbols over the active speaker in the return scene. The detector is reached through a one-function provider seam (`src/zoom-subject-source.h`) that defaults to "no subject", so all of this builds, ships and tests before the detector engine exists.

**Tech Stack:** C++17, libobs (custom-draw sources, `obs_data` settings, `obs_get_base_effect(OBS_EFFECT_SOLID)`), Qt only where already present, CMake + CTest with plain `int main()` tests.

**Spec:** `docs/superpowers/specs/2026-09-05-panelist-feedback-design.md`

---

## Resolution rule — read before designing anything here

**Auto-framing consumes whatever resolution the shared feed already is, and must NEVER request a resolution upgrade.** This feature adds **no new subscription pressure at all** and therefore cannot reintroduce the 2026-08-17 throttle (a 720p wall oversubscribed Zoom's raw-data envelope and took a live meeting to 0.3–0.45× real time).

The rationale is already recorded in place, at `src/zoom-supersource.cpp:459`, and this plan builds on it verbatim:

> "Shared feeds are NOT degraded by this. The engine holds one subscription per participant with an upgrade-only resolution policy (`EngineVideo::subscribe`: a request at or below the active resolution attaches to the existing feed as a no-op). So a participant already on a 720p/1080p program output keeps that quality, and the tile simply reuses the high-quality feed. 360p is paid only for participants the wall alone is showing."

Consequences, all of which the plan reflects:

1. **No new subscription pressure.** Nothing in this plan calls `subscribe()`, changes `tile_feed_subscribe`, or touches a `VideoResolution`.
2. **No new resolution control.** The existing ISO and program-output controls **are** the resolution lever, and they already encode operator intent. Adding a second one would let two mechanisms fight over the same envelope.
3. **A soft auto-framed tile is a diagnosis, not a defect:** that participant is neither ISO'd nor the active speaker, so nobody is paying for their pixels. The remedy is to ISO them, which the operator can already do. Do not engineer around this.
4. **The crop math must be resolution-invariant.** It reads `tex_w`/`tex_h` from the feed and never assumes a value; the same subject at 640×360 and at 1920×1080 must produce the *same normalized* crop. Task 6 pins exactly that.

In the real production configuration the active speaker feed pulls 1080p and any ISO'd participant has a constant high-resolution feed, and Tiles reuses both automatically — so the panelists actually being checked already have real pixels for the crop math.

Detection itself is unaffected either way: the detector downscales to a ~320 px long edge, and a 640×360 source is ample for that.

**Do not conflate this with the 640×360 overlay requirement.** That one is about the physical screen space a gallery tile occupies on a panelist's display, and is unrelated to subscription resolution.

## Global Constraints

- **Consumed detector contract — use these exact field names, do not redefine them:**
  ```cpp
  struct SubjectFrame {
      bool     found;
      float    box_x, box_y, box_w, box_h;          // normalized 0..1 of source
      float    eye_l_x, eye_l_y, eye_r_x, eye_r_y;  // normalized 0..1
      float    confidence;
      uint64_t detected_ns;
  };
  ```
- **Smoothing is already done by the detector** (deadband, rate limit, dropout hold). Never re-implement it here. `found == false` already means "held past the dropout window", so it can be trusted directly as "no subject".
- **No test framework.** Plain `int main()` with a local `check()`. Never gtest, never Catch.
- Test target `CoreVideo<Thing>Test`, ctest name `CoreVideo<Thing>`, hand-registered in the root `CMakeLists.txt` inside `if(BUILD_TESTING)`. There is no `tests/CMakeLists.txt`.
- **No headless GPU harness exists and one has been ruled against** ("an offscreen Qt harness certified it three times and was wrong three times"). Every decision must be extracted into a pure header and unit-tested there. For in-product visual checks use the existing env-var pattern (`COREVIDEO_TALKBACK_LAYOUT_TEST`, `src/zoom-talkback-panel.cpp:95`).
- **Settings use the per-source `obs_data` + `std::atomic` pattern** (see `PROP_ANIMATE` end-to-end: declared `src/zoom-supersource.cpp:2247`, defaulted `:3115`, property `:3360`, stored to an atomic `:2736`, read once per frame in render). **Never** the global-ini `ZoomPluginSettings` mechanism.
- **`SpeakerDirector` is poll-only** — there is no observer list. Poll `snapshot(now_ms)` / `directed_speaker_id()`.
- New pure logic goes in a **header-only file under `src/`** with a "why this exists" comment at the top.
- Build/run: `cmake --build build --config Release --parallel 8` then `ctest -C Release --output-on-failure`, N/N green.
- Auto-frame is **OFF by default**. The overlay source is inert until placed in a scene.
- **Nothing in this plan requests a resolution upgrade.** No task calls `subscribe()`, edits `tile_feed_subscribe`, or names a `VideoResolution`. See the resolution rule above.
- **Spotlight is explicitly NOT required** and must not be assumed anywhere.
- Update `CLAUDE.md` in the same change as substantive work (standing repo directive); Task 11 covers the final pass.

## The four crop-insertion-point constraints (each gets a test)

`src/zoom-supersource.cpp:1957` is the **only** place the sampled rectangle is decided. Every task that touches it must hold:

1. Result aspect must equal `params.tile_aspect` exactly, or the tile letterboxes.
2. The rect must stay inside `[0, tex_w] × [0, tex_h]`; zero width/height falls back to the placeholder.
3. `crop_uv` must be computed from the **truncated integers** actually handed to `gs_draw_sprite_subregion()`, not from the doubles, or borders misregister.
4. Framing state must be snapshotted under `ctx->mutex` alongside `render_slot_crop` (`:1286`) so a framing pass lands as a unit.

## File Structure

**Created:**
- `src/zoom-self-identity.h` — pure: who is us. Name matching, roster flagging, the return-seat registry.
- `src/zoom-subject-source.h` — the detector seam: one installable provider returning a smoothed `SubjectFrame` for a participant id. Defaults to "not found".
- `src/zoom-auto-frame.h` — pure: `SubjectFrame` + source size + tile aspect → an exact-aspect, in-bounds `CropRect`.
- `src/zoom-framing-advice.h` — pure: `SubjectFrame` + thresholds + previous condition → one `FramingCondition`.
- `src/zoom-framing-overlay-layout.h` — pure: `FramingCondition` + canvas size → a list of solid quads, legible at 640×360.
- `src/zoom-framing-overlay.h` / `src/zoom-framing-overlay.cpp` — the `corevideo_framing_overlay` OBS source.
- `tests/self-identity-test.cpp`, `tests/subject-source-test.cpp`, `tests/auto-frame-test.cpp`, `tests/framing-advice-test.cpp`, `tests/framing-overlay-layout-test.cpp`.

**Modified:**
- `src/zoom-types.h` — two bools on `ParticipantInfo`.
- `engine/src/main.cpp` — stamp `is_self` from `GetMySelfUser()`, emit it.
- `src/zoom-engine-client.cpp` — parse `is_self`, apply identity flags before the director sees the roster.
- `src/zoom-tile-fill.h` — identity-flagged participants are never eligible.
- `src/speaker-director.cpp` / `.h` — identity-flagged participants are excluded and dethroned.
- `src/zoom-supersource.cpp` — auto-frame properties/atomics/tick/snapshot and the crop insertion point.
- `src/zoom-plugin.cpp` — register the overlay source.
- `data/locale/en-US.ini` — new strings.
- `CMakeLists.txt` — five new test targets plus the new plugin source.
- `tests/tile-fill-test.cpp`, `tests/speaker-director-test.cpp` — exclusion coverage.
- `CLAUDE.md`, `CHANGELOG.md`.

---

## Task 1: Durable identity — the pure rules

**Why first:** without it the vcam return feed is a real feedback loop, not a hypothetical one. No such code exists today: `ParticipantInfo` has no self field, the engine builds the roster from `GetParticipantsList()` with zero filtering, and the bot therefore appears in every roster, picker, tile candidate set and speaker candidate set. The only current defence is operator combo boxes keyed by a meeting-scoped `user_id` that does **not** survive a rejoin. Worse, talkback deliberately unmutes the bot, making it a fully eligible active-speaker candidate.

**Files:**
- Create: `src/zoom-self-identity.h`
- Create: `tests/self-identity-test.cpp`
- Modify: `src/zoom-types.h:43-56`
- Modify: `CMakeLists.txt` (inside `if(BUILD_TESTING)`)

**Interfaces:**
- Consumes: `ParticipantInfo` from `src/zoom-types.h`.
- Produces: `ParticipantInfo::is_self`, `ParticipantInfo::is_return_identity`; `normalize_identity_name()`, `ReturnIdentityRegistry::instance()`, `apply_identity_flags()`.

- [ ] **Step 1: Add the two fields to `ParticipantInfo`**

In `src/zoom-types.h`, inside `struct ParticipantInfo` after `is_sharing_screen`:

```cpp
    // True if this entry is the SDK identity this plugin joined as — the bot
    // itself. Stamped by the engine from GetMySelfUser() on every roster
    // rebuild, so it is re-derived after a rejoin even though user_id changes.
    // Every consumer that picks somebody to show must refuse this.
    bool        is_self = false;
    // True if this entry is the separate Zoom client carrying the OBS Virtual
    // Camera return feed. Matched by display name (ReturnIdentityRegistry),
    // because that seat is a different account whose user_id also changes on
    // every rejoin. Showing it on the wall, or directing to it, closes a
    // video feedback loop.
    bool        is_return_identity = false;
```

- [ ] **Step 2: Write the failing test**

Create `tests/self-identity-test.cpp`:

```cpp
// Durable self / return-identity flagging. Pure rules only: name
// normalization, registry round-trip, and the roster-flagging pass that both
// consumers depend on.
#include "zoom-self-identity.h"

#include <iostream>
#include <string>
#include <vector>

static int g_failures = 0;

static void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

static ParticipantInfo person(uint32_t id, const char *name)
{
    ParticipantInfo p;
    p.user_id = id;
    p.display_name = name;
    p.has_video = true;
    return p;
}

int main()
{
    // Normalization: case, surrounding space, and interior runs of space all
    // collapse, because an operator types "Return Feed" and Zoom shows
    // "return  feed " after a rename.
    check(normalize_identity_name("  Return  FEED ") == "return feed",
          "normalize collapses case and space");
    check(normalize_identity_name("") == "", "normalize of empty is empty");
    check(normalize_identity_name("   ") == "", "normalize of blank is empty");

    // Registry round-trip. Empty entries are dropped so a blank property
    // never matches every unnamed participant.
    ReturnIdentityRegistry::instance().set_names({"Return Feed", "  ", ""});
    const std::vector<std::string> names =
        ReturnIdentityRegistry::instance().names();
    check(names.size() == 1, "blank return-seat names are dropped");
    check(names.size() == 1 && names[0] == "return feed",
          "registry stores the normalized form");

    // Flagging: engine-reported self stays set, the return seat is matched by
    // name, and everyone else is untouched.
    std::vector<ParticipantInfo> roster = {
        person(11, "CoreVideo Bot"),
        person(22, "return feed"),
        person(33, "Dr. Panelist"),
    };
    roster[0].is_self = true;  // as the engine stamped it
    apply_identity_flags(roster);

    check(roster[0].is_self && !roster[0].is_return_identity,
          "engine-stamped self survives the pass");
    check(roster[1].is_return_identity && !roster[1].is_self,
          "the return seat is flagged by name");
    check(!roster[2].is_self && !roster[2].is_return_identity,
          "a panelist is flagged as neither");

    // The pass is idempotent and it CLEARS a stale return flag when the
    // operator renames the seat — otherwise a mistyped name would poison the
    // roster until the next rejoin.
    ReturnIdentityRegistry::instance().set_names({"Different Seat"});
    apply_identity_flags(roster);
    check(!roster[1].is_return_identity,
          "renaming the return seat clears the old flag");
    check(roster[0].is_self, "the self flag is never cleared by this pass");

    // With no names configured nothing but engine-reported self is flagged.
    ReturnIdentityRegistry::instance().set_names({});
    apply_identity_flags(roster);
    check(!roster[0].is_return_identity && !roster[1].is_return_identity &&
          !roster[2].is_return_identity,
          "an empty registry flags no return identity");

    // A participant may be both: the operator pointed the return seat at the
    // bot's own name. Both flags stand; every consumer refuses on either.
    ReturnIdentityRegistry::instance().set_names({"CoreVideo Bot"});
    apply_identity_flags(roster);
    check(roster[0].is_self && roster[0].is_return_identity,
          "self and return identity can coexist");

    check(identity_excluded(roster[0]), "self is identity-excluded");
    check(!identity_excluded(roster[2]), "a panelist is not identity-excluded");

    if (g_failures == 0) std::cout << "self-identity: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 3: Register the test target and run it to verify it fails**

In `CMakeLists.txt`, inside `if(BUILD_TESTING)` (put it directly after the `CoreVideoSpeakerDirectorTest` block, around line 663):

```cmake
    # Durable self / return-identity flagging. The vcam return feed is a real
    # feedback loop without this, so the rules are pinned rather than trusted.
    add_executable(CoreVideoSelfIdentityTest
        tests/self-identity-test.cpp
    )
    target_include_directories(CoreVideoSelfIdentityTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoSelfIdentity
             COMMAND CoreVideoSelfIdentityTest)
```

Run: `cmake --build build --config Release --parallel 8 --target CoreVideoSelfIdentityTest`
Expected: FAIL — `Cannot open include file: 'zoom-self-identity.h'`

- [ ] **Step 4: Write the header**

Create `src/zoom-self-identity.h`:

```cpp
#pragma once

// Why this exists
// ---------------
// Nothing in this plugin used to know which roster entry is US. The engine
// builds its roster from GetParticipantsList() with no filtering, so the bot
// appears in every roster, every picker, every tile candidate set and every
// active-speaker candidate set. The only defence was a handful of operator
// exclude combo boxes keyed by a MEETING-SCOPED user_id, which does not
// survive a rejoin. Talkback then makes it worse on purpose: it unmutes the
// bot, so during a talkback key the bot is a fully eligible speaker.
//
// Add a Virtual-Camera return feed on top of that and you get a real video
// feedback loop, not a hypothetical one. So identity is derived durably:
//
//   * is_self            - stamped by the engine from GetMySelfUser() on every
//                          roster rebuild, so it is re-derived after a rejoin.
//   * is_return_identity - matched by DISPLAY NAME against operator-configured
//                          names, because the return seat is a separate Zoom
//                          account whose user_id also changes on every rejoin.
//
// Both are recomputed from scratch on every roster message. Neither is a
// remembered id, which is precisely what made the old combo boxes fail.

#include "zoom-types.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

// Lower-cases, trims, and collapses interior whitespace runs to one space.
// Operators type "Return Feed"; Zoom renders "return  feed " after a rename.
inline std::string normalize_identity_name(const std::string &raw)
{
    std::string out;
    out.reserve(raw.size());
    bool pending_space = false;
    for (const char c : raw) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isspace(uc)) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(static_cast<char>(std::tolower(uc)));
    }
    return out;
}

// The display names of the Zoom seats carrying our own return feed.
//
// A process-wide registry rather than a parameter because the roster arrives
// on the engine-client reader thread, while the name is owned by the overlay
// source's per-source settings. Written rarely (a settings change), read on
// every roster message, so a plain mutex is right — this is not a hot path.
class ReturnIdentityRegistry {
public:
    static ReturnIdentityRegistry &instance()
    {
        static ReturnIdentityRegistry s_instance;
        return s_instance;
    }

    // Stores the normalized form. Empty and whitespace-only entries are
    // dropped: a blank property must not match every unnamed participant.
    void set_names(const std::vector<std::string> &names)
    {
        std::vector<std::string> normalized;
        normalized.reserve(names.size());
        for (const std::string &n : names) {
            std::string norm = normalize_identity_name(n);
            if (norm.empty()) continue;
            if (std::find(normalized.begin(), normalized.end(), norm) !=
                normalized.end())
                continue;
            normalized.push_back(std::move(norm));
        }
        std::lock_guard<std::mutex> lock(m_mtx);
        m_names = std::move(normalized);
    }

    std::vector<std::string> names() const
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_names;
    }

private:
    ReturnIdentityRegistry() = default;
    mutable std::mutex m_mtx;
    std::vector<std::string> m_names;
};

// Recomputes is_return_identity for every entry from the current registry.
//
// is_self is NEVER touched: the engine owns it and has better information
// (GetMySelfUser()) than any name comparison could. is_return_identity is
// always overwritten, including back to false, so retyping the seat name
// releases the participant it used to match instead of poisoning the roster
// until the next rejoin.
inline void apply_identity_flags(std::vector<ParticipantInfo> &roster)
{
    const std::vector<std::string> names =
        ReturnIdentityRegistry::instance().names();
    for (ParticipantInfo &p : roster) {
        if (names.empty()) {
            p.is_return_identity = false;
            continue;
        }
        const std::string norm = normalize_identity_name(p.display_name);
        p.is_return_identity =
            !norm.empty() &&
            std::find(names.begin(), names.end(), norm) != names.end();
    }
}

// The one predicate every consumer asks. Kept as a function so a future third
// identity kind lands in one place rather than in each consumer's condition.
inline bool identity_excluded(const ParticipantInfo &p)
{
    return p.is_self || p.is_return_identity;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```
cmake --build build --config Release --parallel 8 --target CoreVideoSelfIdentityTest
ctest -C Release -R CoreVideoSelfIdentity --output-on-failure
```
Expected: PASS, `self-identity: all checks passed`

- [ ] **Step 6: Commit**

```bash
git add src/zoom-self-identity.h src/zoom-types.h tests/self-identity-test.cpp CMakeLists.txt
git commit -m "feat: durable self/return-identity rules for framing consumers"
```

---

## Task 2: Wire identity end to end (engine stamps it, plugin applies it)

**Files:**
- Modify: `engine/src/main.cpp:600-680` (`user_to_info`, `send_roster`)
- Modify: `src/zoom-engine-client.cpp:1774-1800` (the `"participants"` handler)
- Modify: `tests/self-identity-test.cpp`

**Interfaces:**
- Consumes: `apply_identity_flags()`, `ParticipantInfo::is_self` (Task 1).
- Produces: a roster in which `is_self`/`is_return_identity` are true before `SpeakerDirector::update_roster()` or any tile resolve sees it.

- [ ] **Step 1: Write the failing test — flags must be applied before the director is fed**

Append to `tests/self-identity-test.cpp`, immediately before the final `if (g_failures == 0)` block:

```cpp
    // Ordering contract with zoom-engine-client.cpp: flags are applied to the
    // whole roster BEFORE it is handed on. Anything downstream that filters on
    // identity therefore never sees an unflagged roster, not even for one
    // message. This reproduces that sequence.
    {
        ReturnIdentityRegistry::instance().set_names({"Return Feed"});
        std::vector<ParticipantInfo> incoming = {
            person(101, "CoreVideo Bot"),
            person(102, "Return Feed"),
            person(103, "Panelist"),
        };
        incoming[0].is_self = true;

        apply_identity_flags(incoming);

        std::vector<ParticipantInfo> handed_on;
        for (const ParticipantInfo &p : incoming)
            if (!identity_excluded(p)) handed_on.push_back(p);

        check(handed_on.size() == 1 && handed_on[0].user_id == 103,
              "only the panelist survives the identity filter");
    }
```

- [ ] **Step 2: Run it to verify it fails**

Run:
```
cmake --build build --config Release --parallel 8 --target CoreVideoSelfIdentityTest
ctest -C Release -R CoreVideoSelfIdentity --output-on-failure
```
Expected: FAIL — `only the panelist survives the identity filter` (the registry was left holding `CoreVideo Bot` by the earlier block, so `Return Feed` is not matched).

Fix by making the new block's `set_names` authoritative — it already is; if the check passes immediately, confirm by temporarily commenting out the `apply_identity_flags(incoming);` line, re-running to see the failure, and restoring it.

- [ ] **Step 3: Stamp `is_self` in the engine**

In `engine/src/main.cpp`, in `user_to_info()`, after `info.is_sharing_screen = ...`:

```cpp
        // Which roster entry is US. GetMySelfUser() is the participants
        // controller's own answer, re-asked on every rebuild, so this is
        // correct again immediately after a rejoin even though user_id
        // changed. The plugin cannot derive this: it never sees the SDK.
        if (m_ctrl) {
            ZOOMSDK::IUserInfo *self = m_ctrl->GetMySelfUser();
            info.is_self = self && self->GetUserID() == info.user_id;
        }
```

In `send_roster()`, extend the per-participant JSON — add `is_self` to the concatenation, immediately after the `is_sharing_screen` term and before the closing `"}"`:

```cpp
                R"(,"is_self":)" + (p.is_self ? "true" : "false") + "}";
```

(i.e. the `is_sharing_screen` line loses its trailing `+ "}"`, which moves to this new line.)

- [ ] **Step 4: Parse and apply it in the plugin**

In `src/zoom-engine-client.cpp`, in the `cmd == "participants"` handler, after `p.is_sharing_screen = po.value("is_sharing_screen").toBool();`:

```cpp
                p.is_self = po.value("is_self").toBool();
```

Then, immediately before the `SpeakerDirector::instance().update_roster(...)` call in that same lambda:

```cpp
            // Identity flags are applied to the WHOLE roster before anything
            // downstream sees it, so no consumer ever observes an unflagged
            // roster — not even for one message. See zoom-self-identity.h for
            // why this is derived per message instead of remembered by id.
            apply_identity_flags(m_roster);
```

Do the same in the `cmd == "active_speaker"` handler, before its `update_roster(...)` call, so a rename arriving between roster messages cannot leave a stale flag.

Add the include near the other project includes at the top of the file:

```cpp
#include "zoom-self-identity.h"
```

- [ ] **Step 5: Run the tests and build the plugin**

Run:
```
cmake --build build --config Release --parallel 8
ctest -C Release --output-on-failure
```
Expected: PASS, N/N green.

- [ ] **Step 6: Commit**

```bash
git add engine/src/main.cpp src/zoom-engine-client.cpp tests/self-identity-test.cpp
git commit -m "feat: engine stamps is_self; plugin flags identity on every roster"
```

---

## Task 3: Tile assignment refuses our own identities

**Files:**
- Modify: `src/zoom-tile-fill.h` (the `eligible` lambda)
- Modify: `tests/tile-fill-test.cpp`

**Interfaces:**
- Consumes: `identity_excluded()` (Task 1).
- Produces: `resolve_tile_assignments()` that can never place us on the wall in either mode.

- [ ] **Step 1: Write the failing test**

Append to `tests/tile-fill-test.cpp`, before its final return:

```cpp
    // Identity exclusion. The bot and the Virtual-Camera return seat are never
    // shown on the wall, in EITHER mode: a Manual cast is an operator decision
    // the resolver otherwise honours unconditionally, but casting the return
    // feed onto the wall that feeds it is a video feedback loop, so this one
    // operator decision is overruled.
    {
        std::vector<ParticipantInfo> roster(3);
        roster[0].user_id = 1; roster[0].has_video = true;
        roster[0].is_self = true;
        roster[1].user_id = 2; roster[1].has_video = true;
        roster[1].is_return_identity = true;
        roster[2].user_id = 3; roster[2].has_video = true;

        TileFillParams params;
        params.mode = TileFillMode::Auto;
        params.max_tiles = 9;
        const std::vector<uint32_t> auto_out =
            resolve_tile_assignments({}, roster, params);
        check(auto_out.size() == 1 && auto_out[0] == 3,
              "Auto shows only the panelist");

        // An incumbent that becomes identity-excluded is dropped, not kept.
        const std::vector<uint32_t> after =
            resolve_tile_assignments({1, 3}, roster, params);
        check(after.size() == 1 && after[0] == 3,
              "an identity-excluded incumbent loses its slot");

        params.mode = TileFillMode::Manual;
        params.manual = {1, 2, 3};
        const std::vector<uint32_t> manual_out =
            resolve_tile_assignments({}, roster, params);
        check(manual_out.size() == 1 && manual_out[0] == 3,
              "Manual refuses a cast of our own identities");
    }
```

If `tests/tile-fill-test.cpp` has no `check()` helper of that shape, add the same local helper this plan uses in Task 1 at the top of the file and route the new block through it.

- [ ] **Step 2: Run it to verify it fails**

Run:
```
cmake --build build --config Release --parallel 8 --target CoreVideoTileFillTest
ctest -C Release -R CoreVideoTileFill --output-on-failure
```
Expected: FAIL — `Auto shows only the panelist` (all three are returned today).

- [ ] **Step 3: Implement**

In `src/zoom-tile-fill.h`, add the include below the existing `#include "zoom-types.h"`:

```cpp
#include "zoom-self-identity.h"
```

Change the `eligible` lambda's tail from

```cpp
        return it != roster.end() && it->has_video;
```

to

```cpp
        // Identity exclusion is checked here, not in params.excluded, because
        // params.excluded is operator-chosen and meeting-scoped while these
        // flags are re-derived on every roster message and survive a rejoin.
        return it != roster.end() && it->has_video && !identity_excluded(*it);
```

Then, in the Manual branch, replace the loop

```cpp
        for (const uint32_t id : params.manual) push_unique(id);
```

with

```cpp
        // The roster is deliberately not consulted for presence — an operator
        // who cast a tile keeps it even with the camera off — but it IS
        // consulted for identity: casting our own return feed onto the wall
        // that feeds it closes a video loop, and that overrules the cast.
        for (const uint32_t id : params.manual) {
            const auto it = std::find_if(
                roster.begin(), roster.end(),
                [id](const ParticipantInfo &p) { return p.user_id == id; });
            if (it != roster.end() && identity_excluded(*it)) continue;
            push_unique(id);
        }
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```
cmake --build build --config Release --parallel 8 --target CoreVideoTileFillTest
ctest -C Release -R CoreVideoTileFill --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/zoom-tile-fill.h tests/tile-fill-test.cpp
git commit -m "fix: tile assignment never places our own identities on the wall"
```

---

## Task 4: Speaker direction refuses our own identities

**Files:**
- Modify: `src/speaker-director.cpp:72-77` (`participant_excluded_locked`)
- Modify: `tests/speaker-director-test.cpp`

**Interfaces:**
- Consumes: `identity_excluded()` (Task 1).
- Produces: a `SpeakerDirector` that never promotes, and actively dethrones, an identity-excluded participant — including while talkback has the bot unmuted.

- [ ] **Step 1: Write the failing test**

Append to `tests/speaker-director-test.cpp`, before its final return:

```cpp
    // Identity exclusion, which is stronger than the operator exclude list:
    // it is re-derived from the roster on every message, so it survives a
    // rejoin, and it holds while talkback has the bot unmuted and talking —
    // the exact window in which the bot is otherwise a perfectly eligible
    // candidate.
    {
        SpeakerDirector &d = SpeakerDirector::instance();
        d.reset();
        d.configure(/*sensitivity_ms=*/0, /*hold_ms=*/0, /*require_video=*/false,
                    /*excluded=*/{});

        std::vector<ParticipantInfo> roster(2);
        roster[0].user_id = 1;
        roster[0].has_video = true;
        roster[0].is_talking = true;   // talkback unmuted us and we are keyed
        roster[0].is_self = true;
        roster[1].user_id = 2;
        roster[1].has_video = true;

        d.update_roster(roster, /*raw_speaker_id=*/1, /*now_ms=*/1000);
        check(d.directed_speaker_id() != 1,
              "the bot is never promoted even as the raw speaker");

        // And an incumbent that becomes identity-excluded is dethroned.
        d.reset();
        d.configure(0, 0, false, {});
        std::vector<ParticipantInfo> clean(2);
        clean[0].user_id = 1; clean[0].has_video = true; clean[0].is_talking = true;
        clean[1].user_id = 2; clean[1].has_video = true;
        d.update_roster(clean, 1, 2000);
        check(d.directed_speaker_id() == 1, "a clean participant is promoted");

        clean[0].is_return_identity = true;  // operator named the return seat
        d.update_roster(clean, 1, 3000);
        check(d.directed_speaker_id() != 1,
              "an incumbent that becomes the return identity is dethroned");
    }
```

- [ ] **Step 2: Run it to verify it fails**

Run:
```
cmake --build build --config Release --parallel 8 --target CoreVideoSpeakerDirectorTest
ctest -C Release -R CoreVideoSpeakerDirector --output-on-failure
```
Expected: FAIL — `the bot is never promoted even as the raw speaker`.

- [ ] **Step 3: Implement**

In `src/speaker-director.cpp`, add the include with the others at the top:

```cpp
#include "zoom-self-identity.h"
```

Replace `participant_excluded_locked()` in full:

```cpp
bool SpeakerDirector::participant_excluded_locked(uint32_t participant_id) const
{
    // Two sources of exclusion, deliberately folded into one predicate so
    // BOTH get the dethroning behaviour in enforce_incumbent_eligibility_locked()
    // rather than only the operator list.
    //
    // 1. The operator's exclude combo boxes: meeting-scoped ids, lost on a
    //    rejoin. Fine, because an operator is present to re-pick.
    // 2. Identity: us, and the Virtual-Camera return seat. Re-derived from
    //    the roster on every message (zoom-self-identity.h), so it survives a
    //    rejoin — and it must, because talkback deliberately unmutes the bot,
    //    which without this makes the bot an eligible speaker whose picture is
    //    then fed back into the meeting.
    if (std::find(m_excluded_participant_ids.begin(),
                  m_excluded_participant_ids.end(),
                  participant_id) != m_excluded_participant_ids.end())
        return true;

    const auto it = std::find_if(m_roster.begin(), m_roster.end(),
        [participant_id](const ParticipantInfo &p) {
            return p.user_id == participant_id;
        });
    return it != m_roster.end() && identity_excluded(*it);
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```
cmake --build build --config Release --parallel 8 --target CoreVideoSpeakerDirectorTest
ctest -C Release -R CoreVideoSpeakerDirector --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Run the whole suite**

Run: `ctest -C Release --output-on-failure`
Expected: N/N green.

- [ ] **Step 6: Commit**

```bash
git add src/speaker-director.cpp tests/speaker-director-test.cpp
git commit -m "fix: speaker director excludes and dethrones our own identities"
```

---

## Task 5: The detector seam

**Why:** the detector engine is a separate plan by another author. This is the one function both consumers call, with a default that keeps everything here buildable, shippable and testable before the detector exists.

**Files:**
- Create: `src/zoom-subject-source.h`
- Create: `tests/subject-source-test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: the detector plan's `SubjectFrame` (fields fixed in Global Constraints).
- Produces: `SubjectProvider` (typedef), `set_subject_provider()`, `subject_for(uint32_t participant_id)` — every later task calls **only** `subject_for()`.

- [ ] **Step 1: Write the failing test**

Create `tests/subject-source-test.cpp`:

```cpp
// The seam between the framing consumers and the subject detector. The
// detector is a separate plan; this pins the contract the consumers rely on so
// they can be built and tested before it lands.
#include "zoom-subject-source.h"

#include <iostream>
#include <string>

static int g_failures = 0;

static void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

int main()
{
    // With no detector installed, every participant reads as "no subject".
    // That is the state the plugin ships in until the detector engine exists,
    // and it must be the SAFE state: auto-frame falls back to the operator's
    // crop, and the overlay says "step into frame" rather than pointing
    // somewhere arbitrary.
    const SubjectFrame none = subject_for(42);
    check(!none.found, "no provider means not found");
    check(none.confidence == 0.0f, "no provider means zero confidence");

    set_subject_provider([](uint32_t id) {
        SubjectFrame f{};
        f.found = id == 7;
        f.box_x = 0.4f; f.box_y = 0.2f; f.box_w = 0.2f; f.box_h = 0.3f;
        f.eye_l_x = 0.45f; f.eye_l_y = 0.3f;
        f.eye_r_x = 0.55f; f.eye_r_y = 0.3f;
        f.confidence = 0.9f;
        f.detected_ns = 123456789ULL;
        return f;
    });

    check(subject_for(7).found, "the installed provider is consulted");
    check(!subject_for(8).found, "the provider decides per participant");
    check(subject_for(7).detected_ns == 123456789ULL,
          "the frame is returned verbatim");

    // Uninstalling restores the safe default, so a detector shutting down
    // cannot leave consumers holding a dangling std::function.
    set_subject_provider(nullptr);
    check(!subject_for(7).found, "clearing the provider restores not-found");

    if (g_failures == 0) std::cout << "subject-source: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register the target and run it to verify it fails**

In `CMakeLists.txt`, inside `if(BUILD_TESTING)`, after the `CoreVideoSelfIdentityTest` block:

```cmake
    # The detector seam. Pins that "no detector installed" is the safe state.
    add_executable(CoreVideoSubjectSourceTest
        tests/subject-source-test.cpp
    )
    target_include_directories(CoreVideoSubjectSourceTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoSubjectSource
             COMMAND CoreVideoSubjectSourceTest)
```

Run: `cmake --build build --config Release --parallel 8 --target CoreVideoSubjectSourceTest`
Expected: FAIL — `Cannot open include file: 'zoom-subject-source.h'`

- [ ] **Step 3: Write the header**

Create `src/zoom-subject-source.h`:

```cpp
#pragma once

// Why this exists
// ---------------
// The subject detector (libfacedetection behind a worker thread) is a separate
// subsystem with its own plan and its own ship date. Its two consumers — Tiles
// auto-framing and the framing-advice overlay — need exactly one thing from
// it: the current smoothed subject for a participant. So that one thing is a
// seam.
//
// Consequences that are deliberate:
//   * The consumers build, ship and unit-test with no detector at all.
//   * The default answer is "not found", which is every consumer's safe path:
//     auto-frame falls back to the operator's crop, the overlay says "step
//     into frame".
//   * SMOOTHING IS NOT DONE HERE. The detector owns deadband, rate limit and
//     dropout hold, and `found == false` already means "held past the dropout
//     window". Re-smoothing on this side would double the lag and is wrong.
//
// Thread-safety: the provider is installed once when the detector starts and
// cleared when it stops; readers are the OBS graphics thread (tiles tick) and
// the overlay's tick. A shared_ptr snapshot under a mutex is used rather than
// a raw std::function copy so a reader in flight cannot observe a half-assigned
// function object while the detector shuts down.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

// SubjectFrame is defined ONCE, in src/subject-frame.h, which is the published
// contract shared with the subject-detector plan. Do NOT redefine it here --
// two definitions with different member defaults would be an ODR violation the
// moment both headers meet in one translation unit.
//
// src/subject-frame.h is a standalone pure header: <cstdint> only, no detector
// dependency. If the subject-detector plan has not run yet, create it first
// from the contract below (it is ~12 lines and nothing else in that plan is
// needed), so this plan keeps its property of building before the detector
// exists.
#include "subject-frame.h"

using SubjectProvider = std::function<SubjectFrame(uint32_t participant_id)>;

namespace subject_source_detail {

inline std::mutex &mutex()
{
    static std::mutex s_mtx;
    return s_mtx;
}

inline std::shared_ptr<const SubjectProvider> &slot()
{
    static std::shared_ptr<const SubjectProvider> s_provider;
    return s_provider;
}

}  // namespace subject_source_detail

// Installs (or, with nullptr, removes) the detector. Called by the detector
// subsystem only.
inline void set_subject_provider(SubjectProvider provider)
{
    std::shared_ptr<const SubjectProvider> next;
    if (provider)
        next = std::make_shared<const SubjectProvider>(std::move(provider));
    std::lock_guard<std::mutex> lock(subject_source_detail::mutex());
    subject_source_detail::slot() = std::move(next);
}

// The current smoothed subject for a participant, or a not-found frame.
inline SubjectFrame subject_for(uint32_t participant_id)
{
    std::shared_ptr<const SubjectProvider> provider;
    {
        std::lock_guard<std::mutex> lock(subject_source_detail::mutex());
        provider = subject_source_detail::slot();
    }
    if (!provider) return SubjectFrame{};
    return (*provider)(participant_id);
}
```

**Note for the detector's author:** if the detector plan defines `SubjectFrame` in its own header, delete the definition above and `#include` theirs here instead. The field names are fixed by the shared contract, so only the location moves.

- [ ] **Step 4: Run the test to verify it passes**

Run:
```
cmake --build build --config Release --parallel 8 --target CoreVideoSubjectSourceTest
ctest -C Release -R CoreVideoSubjectSource --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/zoom-subject-source.h tests/subject-source-test.cpp CMakeLists.txt
git commit -m "feat: subject detector seam with a safe not-found default"
```

---

## Task 6: Auto-frame crop math (pure)

**Files:**
- Create: `src/zoom-auto-frame.h`
- Create: `tests/auto-frame-test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `SubjectFrame` + `subject_for()` (Task 5), `CropRect` from `src/zoom-tile-grid.h`.
- Produces: `struct AutoFrameParams { double max_zoom; double eyeline_fraction; double target_box_height; }`, `struct AutoFrameResult { bool valid; CropRect crop; }`, `AutoFrameResult solve_auto_frame(const SubjectFrame&, double src_width, double src_height, double dst_aspect, const AutoFrameParams&)`.

- [ ] **Step 1: Write the failing test**

Create `tests/auto-frame-test.cpp`:

```cpp
// Auto-frame crop math, pinned the way tests/tile-shape-test.cpp pins the
// shader's crop arithmetic: exact aspect, inside the source, and crop_uv
// recomputed from the TRUNCATED integers so borders stay in register. There is
// no headless GPU harness in this repo and there is not going to be one, so
// this file is the only thing standing between a framing bug and a live show.
#include "zoom-auto-frame.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

static int g_failures = 0;

static void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

static bool near(double a, double b, double eps)
{
    return std::fabs(a - b) <= eps;
}

// A subject centred at x=0.5, eyes at y=0.30, box 0.20 wide x 0.30 tall.
static SubjectFrame centred_subject()
{
    SubjectFrame f{};
    f.found = true;
    f.box_x = 0.40f; f.box_y = 0.20f; f.box_w = 0.20f; f.box_h = 0.30f;
    f.eye_l_x = 0.46f; f.eye_l_y = 0.30f;
    f.eye_r_x = 0.54f; f.eye_r_y = 0.30f;
    f.confidence = 0.95f;
    f.detected_ns = 1;
    return f;
}

int main()
{
    const AutoFrameParams p;  // defaults are the shipped behaviour

    // ── Not found: no crop, and the caller must fall back ────────────────────
    {
        SubjectFrame gone{};
        const AutoFrameResult r =
            solve_auto_frame(gone, 640.0, 360.0, 16.0 / 9.0, p);
        check(!r.valid, "a not-found subject yields no auto-frame");
    }

    // ── 360p is the DEFAULT tile subscription: it must work exactly here ─────
    {
        const double src_w = 640.0, src_h = 360.0, aspect = 16.0 / 9.0;
        const AutoFrameResult r =
            solve_auto_frame(centred_subject(), src_w, src_h, aspect, p);
        check(r.valid, "a found subject yields an auto-frame at 360p");
        check(near(r.crop.width / r.crop.height, aspect, 1e-9),
              "the 360p auto-frame rect has EXACTLY the tile aspect");
        check(r.crop.x >= 0.0 && r.crop.y >= 0.0 &&
              r.crop.x + r.crop.width <= src_w &&
              r.crop.y + r.crop.height <= src_h,
              "the 360p auto-frame rect stays inside the source");

        // The eyeline lands on the upper-third line of the CROP.
        const double eye_y = 0.30 * src_h;
        check(near((eye_y - r.crop.y) / r.crop.height, p.eyeline_fraction, 1e-9),
              "the eyeline sits on the upper-third line of the crop");
        // ...and the subject is centred horizontally in the crop.
        const double centre_x = 0.5 * src_w;
        check(near((centre_x - r.crop.x) / r.crop.width, 0.5, 1e-9),
              "the subject is centred horizontally in the crop");
    }

    // ── Zoom limit ───────────────────────────────────────────────────────────
    {
        // A tiny box would ask for a huge magnification; max_zoom caps it.
        SubjectFrame tiny = centred_subject();
        tiny.box_h = 0.02f;
        const double src_w = 1920.0, src_h = 1080.0, aspect = 16.0 / 9.0;
        const AutoFrameResult r =
            solve_auto_frame(tiny, src_w, src_h, aspect, AutoFrameParams{});
        check(r.valid, "a tiny box still frames");
        // At 16:9 into a 16:9 source the largest crop is the whole frame, so
        // the smallest legal crop height is src_h / max_zoom.
        check(near(r.crop.height, src_h / AutoFrameParams{}.max_zoom, 1e-6),
              "the crop never zooms past max_zoom");
        check(near(r.crop.width / r.crop.height, aspect, 1e-9),
              "aspect survives the zoom clamp");
    }

    // ── A big box asks to zoom OUT past the frame; clamped to the frame ──────
    {
        SubjectFrame huge = centred_subject();
        huge.box_h = 0.95f;
        const double src_w = 1920.0, src_h = 1080.0, aspect = 16.0 / 9.0;
        const AutoFrameResult r = solve_auto_frame(huge, src_w, src_h, aspect, p);
        check(near(r.crop.width, src_w, 1e-6) && near(r.crop.height, src_h, 1e-6),
              "the crop never grows past the source");
    }

    // ── Off-centre and off-edge subjects: translation clamps, aspect holds ───
    {
        const double src_w = 640.0, src_h = 360.0, aspect = 16.0 / 9.0;
        for (const float bx : {0.0f, 0.02f, 0.45f, 0.80f}) {
            SubjectFrame s = centred_subject();
            s.box_x = bx;
            s.eye_l_x = bx + 0.06f;
            s.eye_r_x = bx + 0.14f;
            const AutoFrameResult r = solve_auto_frame(s, src_w, src_h, aspect, p);
            check(r.valid, "an off-centre subject still frames");
            check(near(r.crop.width / r.crop.height, aspect, 1e-9),
                  "aspect holds for an off-centre subject");
            check(r.crop.x >= 0.0 && r.crop.x + r.crop.width <= src_w,
                  "the crop stays inside the source horizontally");
            check(r.crop.y >= 0.0 && r.crop.y + r.crop.height <= src_h,
                  "the crop stays inside the source vertically");
        }
        // Eyes near the top of the frame would put the crop above y=0.
        SubjectFrame high = centred_subject();
        high.eye_l_y = 0.02f; high.eye_r_y = 0.02f;
        const AutoFrameResult r = solve_auto_frame(high, src_w, src_h, aspect, p);
        check(r.crop.y >= 0.0, "a high eyeline cannot push the crop off the top");
        check(near(r.crop.width / r.crop.height, aspect, 1e-9),
              "aspect holds when the vertical clamp bites");
    }

    // ── Non-16:9 tiles: the aspect that must be honoured is the TILE's ───────
    {
        const double src_w = 640.0, src_h = 360.0;
        for (const double aspect : {4.0 / 3.0, 1.0, 9.0 / 16.0, 21.0 / 9.0}) {
            const AutoFrameResult r =
                solve_auto_frame(centred_subject(), src_w, src_h, aspect, p);
            check(r.valid, "a non-16:9 tile still frames");
            check(near(r.crop.width / r.crop.height, aspect, 1e-9),
                  "the crop matches the tile aspect exactly");
            check(r.crop.x >= 0.0 && r.crop.y >= 0.0 &&
                  r.crop.x + r.crop.width <= src_w &&
                  r.crop.y + r.crop.height <= src_h,
                  "the non-16:9 crop stays inside the source");
        }
    }

    // ── Degenerate inputs never produce a rect the draw path would use ───────
    {
        check(!solve_auto_frame(centred_subject(), 0.0, 360.0, 1.7778, p).valid,
              "a zero-width source yields no auto-frame");
        check(!solve_auto_frame(centred_subject(), 640.0, 0.0, 1.7778, p).valid,
              "a zero-height source yields no auto-frame");
        check(!solve_auto_frame(centred_subject(), 640.0, 360.0, 0.0, p).valid,
              "a zero aspect yields no auto-frame");
        SubjectFrame nan_box = centred_subject();
        nan_box.box_h = std::nanf("");
        check(!solve_auto_frame(nan_box, 640.0, 360.0, 1.7778, p).valid,
              "a NaN box yields no auto-frame");
    }

    // ── Resolution invariance ────────────────────────────────────────────────
    // Auto-framing NEVER asks for a resolution upgrade: it consumes whatever
    // the shared feed already is, which is 1080p for an ISO'd or program-output
    // participant and 360p for one only the tile wall is showing. So the SAME
    // subject must produce the SAME NORMALIZED crop at both, or the framing
    // decision would silently depend on who happens to be ISO'd — and the tile
    // would jump the moment somebody's ISO was switched on mid-show.
    {
        const double aspect = 16.0 / 9.0;
        const SubjectFrame s = centred_subject();
        const AutoFrameResult lo = solve_auto_frame(s, 640.0, 360.0, aspect, p);
        const AutoFrameResult hi = solve_auto_frame(s, 1920.0, 1080.0, aspect, p);
        check(lo.valid && hi.valid, "both resolutions frame");
        check(near(lo.crop.x / 640.0, hi.crop.x / 1920.0, 1e-9),
              "normalized crop x is identical at 360p and 1080p");
        check(near(lo.crop.y / 360.0, hi.crop.y / 1080.0, 1e-9),
              "normalized crop y is identical at 360p and 1080p");
        check(near(lo.crop.width / 640.0, hi.crop.width / 1920.0, 1e-9),
              "normalized crop width is identical at 360p and 1080p");
        check(near(lo.crop.height / 360.0, hi.crop.height / 1080.0, 1e-9),
              "normalized crop height is identical at 360p and 1080p");

        // And at an odd, non-16:9 source too, so nothing has quietly assumed a
        // 16:9 feed. 1280x720 and 960x540 are the same picture; 704x396 is not
        // a standard rung and must still agree with them normalized.
        const AutoFrameResult a = solve_auto_frame(s, 1280.0, 720.0, aspect, p);
        const AutoFrameResult b = solve_auto_frame(s, 704.0, 396.0, aspect, p);
        check(near(a.crop.width / 1280.0, b.crop.width / 704.0, 1e-9),
              "normalized crop is resolution-independent at odd sizes too");
    }

    // ── crop_uv registration, reproduced exactly as the draw path does it ────
    // This is the arithmetic at src/zoom-supersource.cpp:1957-2007, copied the
    // way tests/tile-shape-test.cpp:189-240 copies it. If the shader's
    // (uv - crop_uv.xy) / crop_uv.zw stops landing on 0..1, every border on
    // every auto-framed tile silently misregisters.
    {
        const double tex_w = 640.0, tex_h = 360.0;
        const double aspect = 4.0 / 3.0;
        const AutoFrameResult r =
            solve_auto_frame(centred_subject(), tex_w, tex_h, aspect, p);
        check(r.valid, "the registration case produced a rect");

        const uint32_t cx = static_cast<uint32_t>(r.crop.x);
        const uint32_t cy = static_cast<uint32_t>(r.crop.y);
        const uint32_t cw = static_cast<uint32_t>(r.crop.width);
        const uint32_t ch = static_cast<uint32_t>(r.crop.height);
        check(cw != 0 && ch != 0, "the auto-frame crop did not truncate to nothing");
        check(cx + cw <= static_cast<uint32_t>(tex_w) &&
              cy + ch <= static_cast<uint32_t>(tex_h),
              "the TRUNCATED integers are still inside the texture");

        const float crop_u  = static_cast<float>(cx) / static_cast<float>(tex_w);
        const float crop_v  = static_cast<float>(cy) / static_cast<float>(tex_h);
        const float crop_cu = static_cast<float>(cw) / static_cast<float>(tex_w);
        const float crop_cv = static_cast<float>(ch) / static_cast<float>(tex_h);

        const float u0 = static_cast<float>(cx) / static_cast<float>(tex_w);
        const float u1 = static_cast<float>(cx + cw) / static_cast<float>(tex_w);
        const float v0 = static_cast<float>(cy) / static_cast<float>(tex_h);
        const float v1 = static_cast<float>(cy + ch) / static_cast<float>(tex_h);

        const double tile_u0 = (u0 - crop_u) / crop_cu;
        const double tile_u1 = (u1 - crop_u) / crop_cu;
        const double tile_v0 = (v0 - crop_v) / crop_cv;
        const double tile_v1 = (v1 - crop_v) / crop_cv;
        check(near(tile_u0, 0.0, 1e-5) && near(tile_u1, 1.0, 1e-5) &&
              near(tile_v0, 0.0, 1e-5) && near(tile_v1, 1.0, 1e-5),
              "crop_uv from the truncated integers still lands on 0..1");
    }

    if (g_failures == 0) std::cout << "auto-frame: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register the target and run it to verify it fails**

In `CMakeLists.txt`, inside `if(BUILD_TESTING)`, after the `CoreVideoSubjectSourceTest` block:

```cmake
    # Auto-frame crop math. Exact tile aspect, in-bounds, crop_uv taken from
    # the truncated integers, and the same normalized answer at 360p and 1080p —
    # the four ways this silently breaks tiles.
    add_executable(CoreVideoAutoFrameTest
        tests/auto-frame-test.cpp
        src/zoom-tile-grid.cpp
    )
    target_include_directories(CoreVideoAutoFrameTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoAutoFrame
             COMMAND CoreVideoAutoFrameTest)
```

Run: `cmake --build build --config Release --parallel 8 --target CoreVideoAutoFrameTest`
Expected: FAIL — `Cannot open include file: 'zoom-auto-frame.h'`

- [ ] **Step 3: Write the header**

Create `src/zoom-auto-frame.h`:

```cpp
#pragma once

// Why this exists
// ---------------
// Tiles auto-framing decides ONE thing: the sub-rectangle of a participant's
// frame the wall should sample. That decision has to satisfy the same four
// constraints the manual slot crop does (see src/zoom-supersource.cpp:1957):
// exact tile aspect or the tile letterboxes; inside [0,tex_w]x[0,tex_h]; a
// crop_uv derived from the truncated integers or borders misregister; and a
// snapshot under ctx->mutex so a pass lands as a unit.
//
// Constraints 1 and 2 are structural and live HERE, in pure arithmetic that
// tests/auto-frame-test.cpp pins the way tests/tile-shape-test.cpp pins the
// shader's crop maths. There is no headless GPU harness in this repo and one
// has been ruled against, so a pinned pure header is the whole verification
// story.
//
// Two things this file deliberately does NOT do:
//   * Smoothing. The detector already applies deadband, rate limit and dropout
//     hold. Re-smoothing here would double the lag and make tiles feel dead.
//   * Anything resolution-dependent, and above all anything that asks for MORE
//     resolution. This consumes whatever the shared feed already is: the engine
//     holds one upgrade-only subscription per participant, so a participant on
//     a 1080p program output or an ISO is already high-resolution and the tile
//     reuses that for free, while a participant only the wall is showing stays
//     at 360p and simply looks softer. That softness is a diagnosis - nobody is
//     paying for their pixels - and the remedy is to ISO them, which the
//     operator can already do. All maths is in source pixels, read from
//     tex_w/tex_h, and the SAME subject yields the same NORMALIZED crop at
//     640x360 and at 1920x1080.
//
// The framing rule, in order:
//   1. Height: scale the crop so the subject's box height is
//      target_box_height of the crop height. That is a medium close-up.
//   2. Clamp that height between the largest crop that fits the source at the
//      tile aspect, and that same height divided by max_zoom. Width is then
//      height * aspect, which by construction also fits — so the aspect is
//      EXACT and no later clamp can break it.
//   3. Position: put the eyeline on the crop's upper-third line and the
//      subject's horizontal centre on the crop's centre, then translate the
//      whole rect back inside the source. Translation cannot change the
//      dimensions, so the aspect survives the clamp.

#include "zoom-subject-source.h"
#include "zoom-tile-grid.h"

#include <algorithm>
#include <cmath>

struct AutoFrameParams {
    // Largest magnification allowed, as a linear factor on the crop's
    // dimensions. 2.0 means "never sample less than half the frame's height".
    // Held low on purpose: a tile the wall alone is showing is carried at 360p,
    // so a 2x crop is already a 320x180 region drawn into a tile several
    // hundred pixels wide, and past that the softness reads as a fault rather
    // than a choice. A participant who is ISO'd or on a program output is
    // already carried higher and gets the same framing from more pixels, for
    // free - this number does not change, and nothing here ever asks for more.
    double max_zoom = 2.0;
    // Where the eyeline goes, as a fraction of the crop height from its top.
    // The broadcast upper-third line.
    double eyeline_fraction = 1.0 / 3.0;
    // The subject's box height as a fraction of the crop height. 0.45 is a
    // conventional medium close-up: head and shoulders, air above.
    double target_box_height = 0.45;
};

struct AutoFrameResult {
    bool     valid = false;  // false => the caller must use the manual crop
    CropRect crop;           // source pixels; aspect is exactly dst_aspect
};

// True for a finite, strictly positive double. Guards every input, because a
// detector bug that emits NaN must degrade to "no auto-frame", never to a
// crop rect the draw path will try to sample.
inline bool auto_frame_finite_positive(double v)
{
    return std::isfinite(v) && v > 0.0;
}

inline AutoFrameResult solve_auto_frame(const SubjectFrame &subject,
                                        double src_width, double src_height,
                                        double dst_aspect,
                                        const AutoFrameParams &params)
{
    AutoFrameResult out;
    if (!subject.found) return out;
    if (!auto_frame_finite_positive(src_width) ||
        !auto_frame_finite_positive(src_height) ||
        !auto_frame_finite_positive(dst_aspect))
        return out;
    if (!auto_frame_finite_positive(static_cast<double>(subject.box_h)) ||
        !auto_frame_finite_positive(static_cast<double>(subject.box_w)))
        return out;
    if (!std::isfinite(static_cast<double>(subject.box_x)) ||
        !std::isfinite(static_cast<double>(subject.eye_l_y)) ||
        !std::isfinite(static_cast<double>(subject.eye_r_y)) ||
        !std::isfinite(static_cast<double>(subject.eye_l_x)) ||
        !std::isfinite(static_cast<double>(subject.eye_r_x)))
        return out;

    const double max_zoom =
        auto_frame_finite_positive(params.max_zoom) && params.max_zoom >= 1.0
            ? params.max_zoom : 1.0;
    const double eyeline =
        std::min(std::max(params.eyeline_fraction, 0.05), 0.95);
    const double target_box =
        std::min(std::max(params.target_box_height, 0.05), 1.0);

    // 1. Desired crop height, in source pixels.
    const double box_h_px = static_cast<double>(subject.box_h) * src_height;
    const double desired_h = box_h_px / target_box;

    // 2. Clamp, aspect-first. h_max is the tallest crop of this aspect that
    //    fits the source, so h <= h_max guarantees width = h * aspect <=
    //    src_width AND height <= src_height. Nothing below changes the
    //    dimensions again, which is why the aspect comes out exact.
    const double h_max = std::min(src_height, src_width / dst_aspect);
    const double h_min = h_max / max_zoom;
    const double crop_h = std::min(std::max(desired_h, h_min), h_max);
    const double crop_w = crop_h * dst_aspect;

    // 3. Position. Eyeline onto the upper-third line, subject centred
    //    horizontally, then translated back inside the frame.
    const double eye_y_px =
        0.5 * (static_cast<double>(subject.eye_l_y) +
               static_cast<double>(subject.eye_r_y)) * src_height;
    const double centre_x_px =
        (static_cast<double>(subject.box_x) +
         0.5 * static_cast<double>(subject.box_w)) * src_width;

    double x = centre_x_px - 0.5 * crop_w;
    double y = eye_y_px - eyeline * crop_h;
    x = std::min(std::max(x, 0.0), src_width - crop_w);
    y = std::min(std::max(y, 0.0), src_height - crop_h);

    // A crop that truncates to zero on either axis would make the draw path
    // fall back to the neutral placeholder, which reads as a dead tile. Refuse
    // instead, so the manual crop keeps the tile alive.
    if (crop_w < 2.0 || crop_h < 2.0) return out;

    out.valid = true;
    out.crop.x = x;
    out.crop.y = y;
    out.crop.width = crop_w;
    out.crop.height = crop_h;
    return out;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```
cmake --build build --config Release --parallel 8 --target CoreVideoAutoFrameTest
ctest -C Release -R CoreVideoAutoFrame --output-on-failure
```
Expected: PASS, `auto-frame: all checks passed`

- [ ] **Step 5: Commit**

```bash
git add src/zoom-auto-frame.h tests/auto-frame-test.cpp CMakeLists.txt
git commit -m "feat: pure auto-frame crop math, aspect- and bounds-pinned"
```

---

## Task 7: Wire auto-framing into the Tiles source

**Files:**
- Modify: `src/zoom-supersource.cpp` — struct fields (~`:297`, `:343`), property names (~`:2259`), update (~`:2640`, `:2805`), defaults (~`:3115`), properties (~`:3395`), snapshot (`:1286`), crop insertion (`:1957`), registration (`:3505`)
- Modify: `data/locale/en-US.ini`

**Interfaces:**
- Consumes: `solve_auto_frame()` / `AutoFrameParams` (Task 6), `subject_for()` (Task 5), `solve_slot_crop()` (`src/zoom-tile-crop.h`).
- Produces: properties `auto_frame` (bool, default false) and `auto_frame_max_zoom` (int %, default 200); a per-slot `AutoFrameResult` snapshot the draw path consumes.

**Hard constraint on this task:** it must not call `ZoomEngineClient::subscribe()`, must not modify `tile_feed_subscribe` (`:475-480`), and must not name a `VideoResolution` anywhere. Auto-framing consumes whatever the shared feed already is. A tile the wall alone is showing stays at 360p and its auto-framed crop looks softer; that is the diagnosis (nobody is paying for that participant's pixels) and the remedy is for the operator to ISO them, which they can already do. Verify before committing with `git diff` — the diff for this task must contain no `subscribe`, no `VideoResolution`, and no `P360`/`P720`.

- [ ] **Step 1: Write the failing test — the insertion-point selection rule**

Append to `tests/auto-frame-test.cpp`, before its final `if (g_failures == 0)` block. This pins the exact decision the draw path makes at `:1957` — auto-frame replaces the manual crop only when it is both enabled and valid — without needing a GPU:

```cpp
    // ── The insertion-point rule, reproduced ─────────────────────────────────
    // src/zoom-supersource.cpp:1957 picks between the operator's slot crop and
    // the auto-frame. Both branches must satisfy the SAME four constraints, so
    // the selection itself is pinned here rather than trusted to a comment.
    {
        const double tex_w = 640.0, tex_h = 360.0, aspect = 16.0 / 9.0;
        const auto pick = [&](bool enabled, const SubjectFrame &s) {
            const AutoFrameResult af =
                enabled ? solve_auto_frame(s, tex_w, tex_h, aspect, p)
                        : AutoFrameResult{};
            return af.valid ? af.crop
                            : solve_slot_crop(tex_w, tex_h, aspect, 10.0, 0.0);
        };

        SubjectFrame gone{};
        const CropRect off = pick(false, centred_subject());
        const CropRect lost = pick(true, gone);
        const CropRect on = pick(true, centred_subject());

        const CropRect manual = solve_slot_crop(tex_w, tex_h, aspect, 10.0, 0.0);
        check(near(off.x, manual.x, 1e-9) && near(off.width, manual.width, 1e-9),
              "auto-frame off leaves the manual crop untouched");
        check(near(lost.x, manual.x, 1e-9) && near(lost.width, manual.width, 1e-9),
              "a lost subject falls back to the manual crop");
        check(!near(on.width, manual.width, 1e-9) || !near(on.y, manual.y, 1e-9),
              "auto-frame on with a subject actually changes the rect");

        for (const CropRect &c : {off, lost, on}) {
            check(near(c.width / c.height, aspect, 1e-9),
                  "every branch of the insertion point keeps the tile aspect");
            check(c.x >= 0.0 && c.y >= 0.0 && c.x + c.width <= tex_w &&
                  c.y + c.height <= tex_h,
                  "every branch of the insertion point stays in bounds");
            check(static_cast<uint32_t>(c.width) != 0 &&
                  static_cast<uint32_t>(c.height) != 0,
                  "no branch truncates to a zero-size sub-region");
        }
    }
```

Add `#include "zoom-tile-crop.h"` to the test's includes.

- [ ] **Step 2: Run it to verify it fails**

Run:
```
cmake --build build --config Release --parallel 8 --target CoreVideoAutoFrameTest
ctest -C Release -R CoreVideoAutoFrame --output-on-failure
```
Expected: FAIL — `Cannot open include file: 'zoom-tile-crop.h'` until the include is added; then it should pass, since the rule is expressed entirely in already-implemented functions. Confirm it is a real test by temporarily changing `af.valid ?` to `false ?` and seeing `auto-frame on with a subject actually changes the rect` fail; restore it.

- [ ] **Step 3: Add the property names, defaults, properties and atomics**

In `src/zoom-supersource.cpp`, add the includes with the other project includes at the top:

```cpp
#include "zoom-auto-frame.h"
#include "zoom-subject-source.h"
```

After the `PROP_CROP_GROUP` declaration (~`:2259`):

```cpp
// Auto-framing. OFF by default and a complete bypass when off: with it off the
// draw path takes exactly the solve_slot_crop() call it always took, so a scene
// saved before this existed renders byte-for-byte as it did.
//
// The zoom cap is a percentage because a slider of "200%" reads to an operator
// and a slider of "2.00" does not. Deliberately modest: a tile only the wall is
// showing is carried at 360p (see tile_feed_subscribe), and past ~2x a 360p
// source magnified into a tile looks like a fault rather than a framing choice.
// Auto-framing NEVER raises that subscription - it consumes whatever the shared
// feed already is, so an ISO'd or program-output participant is framed from
// their existing high-resolution feed for free.
static constexpr const char *PROP_AUTO_FRAME      = "auto_frame";
static constexpr const char *PROP_AUTO_FRAME_ZOOM = "auto_frame_max_zoom_pct";
static constexpr int64_t kMinAutoFrameZoomPct = 100;
static constexpr int64_t kMaxAutoFrameZoomPct = 300;
```

In `struct tiles_source`, beside `slot_crop` (~`:297`):

```cpp
    // Auto-framing. The settings pair is atomic like every other setting here;
    // the per-slot results are the framing STATE, and they live under
    // ctx->mutex beside slot_crop for the reason spelled out at the snapshot
    // in tiles_source_render(): a framing pass must land on the draw path as a
    // unit, not slot by slot.
    std::atomic<bool>     auto_frame{false};
    std::atomic<uint32_t> auto_frame_zoom_pct{200};
    std::array<AutoFrameResult, kMaxTileSlots> auto_frame_state{};
```

And beside `render_slot_crop` (~`:343`):

```cpp
    std::array<AutoFrameResult, kMaxTileSlots> render_auto_frame{};
```

In `tiles_source_get_defaults` (~`:3115`, next to the `PROP_ANIMATE` defaults):

```cpp
    // Off by default, and off is a complete bypass — see PROP_AUTO_FRAME.
    obs_data_set_default_bool(settings, PROP_AUTO_FRAME, false);
    obs_data_set_default_int(settings, PROP_AUTO_FRAME_ZOOM, 200);
```

In `tiles_source_get_properties` (~`:3395`, just before the crop group):

```cpp
    // Auto-framing sits beside the per-tile crop group because it is the same
    // decision made automatically: which part of the source this tile samples.
    obs_properties_add_bool(props, PROP_AUTO_FRAME,
                            obs_module_text("CoreVideoTiles.AutoFrame"));
    obs_properties_add_int_slider(props, PROP_AUTO_FRAME_ZOOM,
        obs_module_text("CoreVideoTiles.AutoFrameZoom"),
        static_cast<int>(kMinAutoFrameZoomPct),
        static_cast<int>(kMaxAutoFrameZoomPct), 10);
```

In `tiles_source_update`, beside the `PROP_ANIMATE` stores (~`:2736`):

```cpp
    ctx->auto_frame.store(obs_data_get_bool(settings, PROP_AUTO_FRAME),
                          std::memory_order_release);
    // Clamped on the same threat model as every other setting here: the slider
    // bounds it, obs_data_get_int returns an int64, and scene files are
    // hand-editable. solve_auto_frame() clamps again; this bounds the setting.
    ctx->auto_frame_zoom_pct.store(
        static_cast<uint32_t>(std::min<int64_t>(
            std::max<int64_t>(obs_data_get_int(settings, PROP_AUTO_FRAME_ZOOM),
                              kMinAutoFrameZoomPct),
            kMaxAutoFrameZoomPct)),
        std::memory_order_release);
```

- [ ] **Step 4: Add the tick that computes framing state, and register it**

Add this function immediately above `tiles_source_render`:

```cpp
// Recomputes each slot's auto-frame rect. Runs on the OBS graphics thread via
// info.video_tick, i.e. immediately before the render that consumes it, and
// does no I/O: subject_for() is a cached lookup of the detector's already
// smoothed result, and solve_auto_frame() is arithmetic.
//
// It runs here rather than in the roster callback because detection updates at
// 2-5 fps on its own schedule, which no roster event is correlated with.
//
// Writes under ctx->mutex, beside slot_crop, so the whole pass is visible to
// the draw path at once. Reading the feed list requires the same lock, so the
// participant ids are copied out first and the solve runs unlocked.
static void tiles_source_tick(void *data, float /*seconds*/)
{
    auto *ctx = static_cast<tiles_source *>(data);

    if (!ctx->auto_frame.load(std::memory_order_acquire)) {
        // Off is a complete bypass: clear the state so switching it back on
        // cannot resurrect a stale rect from minutes ago.
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->auto_frame_state = {};
        return;
    }

    AutoFrameParams params;
    params.max_zoom =
        static_cast<double>(ctx->auto_frame_zoom_pct.load(
            std::memory_order_acquire)) / 100.0;
    const double aspect = ctx->tile_aspect.load(std::memory_order_acquire);

    struct SlotSubject {
        uint32_t participant_id = 0;
        double   tex_w = 0.0;
        double   tex_h = 0.0;
    };
    std::array<SlotSubject, kMaxTileSlots> slots{};
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        for (std::size_t i = 0; i < ctx->feeds.size() && i < slots.size(); ++i) {
            const TileFeedPtr &feed = ctx->feeds[i];
            if (!feed) continue;
            slots[i].participant_id = feed->slot.participant_id();
            slots[i].tex_w = static_cast<double>(feed->tex_w);
            slots[i].tex_h = static_cast<double>(feed->tex_h);
        }
    }

    std::array<AutoFrameResult, kMaxTileSlots> next{};
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].participant_id == 0) continue;
        if (slots[i].tex_w < 2.0 || slots[i].tex_h < 2.0) continue;
        next[i] = solve_auto_frame(subject_for(slots[i].participant_id),
                                   slots[i].tex_w, slots[i].tex_h, aspect,
                                   params);
    }

    std::lock_guard<std::mutex> lock(ctx->mutex);
    ctx->auto_frame_state = next;
}
```

In `zoom_supersource_register()` (~`:3505`), beside the other `info.` assignments:

```cpp
    // Recomputes auto-framing just before each render. Registered even though
    // auto-framing is off by default: the tick's first act is to check the
    // atomic and return, which costs one relaxed load per frame.
    info.video_tick     = tiles_source_tick;
```

- [ ] **Step 5: Snapshot it with the crop, and consume it at the insertion point**

In `tiles_source_render`, extend the existing snapshot block at `:1286`:

```cpp
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->render_feeds = ctx->feeds;
        ctx->render_slot_crop = ctx->slot_crop;
        // Snapshotted with the crop, under the same lock, for the same reason:
        // a framing pass must land on the draw path as a unit. Taken here and
        // not re-read below, so a tick landing mid-frame cannot give slot 3 a
        // new rect while slot 4 still has the old one.
        ctx->render_auto_frame = ctx->auto_frame_state;
    }
```

Then at `:1957`, replace the single `solve_slot_crop` call with the selection. The variable stays named `crop` so everything below it — the truncation, `crop_uv`, the sprite sub-region — is untouched:

```cpp
        // Auto-framing REPLACES the operator's slot crop for this slot when it
        // is both enabled and valid; otherwise the manual crop stands. Both
        // branches produce a rect of exactly params.tile_aspect that lies
        // inside the source, which is what the four constraints on this site
        // require — pinned in tests/auto-frame-test.cpp, because there is no
        // headless GPU harness in this repo and one has been ruled against.
        //
        // Not blended with the slot crop: two mechanisms moving the same
        // rectangle at once is a shot nobody can predict, and the operator's
        // slider is the one that must win when they touch it. So it is a
        // clean either/or, and switching auto-framing off restores exactly
        // the rect they were looking at before.
        const AutoFrameResult auto_frame =
            i < ctx->render_auto_frame.size() ? ctx->render_auto_frame[i]
                                              : AutoFrameResult{};
        const CropRect crop =
            auto_frame.valid
                ? auto_frame.crop
                : solve_slot_crop(static_cast<double>(feed->tex_w),
                                  static_cast<double>(feed->tex_h),
                                  params.tile_aspect,
                                  slot_crop.first, slot_crop.second);
```

- [ ] **Step 6: Add the locale strings**

In `data/locale/en-US.ini`, beside the other `CoreVideoTiles.*` keys:

```ini
CoreVideoTiles.AutoFrame="Auto-frame tiles (experimental)"
CoreVideoTiles.AutoFrameZoom="Auto-frame maximum zoom (%)"
```

- [ ] **Step 7: Build and run the whole suite**

Run:
```
cmake --build build --config Release --parallel 8
ctest -C Release --output-on-failure
```
Expected: builds clean, N/N green.

- [ ] **Step 8: Commit**

```bash
git add src/zoom-supersource.cpp data/locale/en-US.ini tests/auto-frame-test.cpp
git commit -m "feat: opt-in Tiles auto-framing at the single crop insertion point"
```

---

## Task 8: Framing advice predicates (pure)

**Files:**
- Create: `src/zoom-framing-advice.h`
- Create: `tests/framing-advice-test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `SubjectFrame` (Task 5).
- Produces: `enum class FramingCondition`, `struct FramingThresholds`, `FramingCondition evaluate_framing(const SubjectFrame&, const FramingThresholds&, FramingCondition previous)`.

The seven spec conditions, each as a concrete geometric predicate on normalized coordinates:

| Spec condition | Predicate | Threshold |
|---|---|---|
| No face for longer than the hold period | `!found` (the detector's dropout hold already expired) | — |
| Box touches a frame edge | `box_x <= m` / `box_x+box_w >= 1-m` / `box_y <= m` / `box_y+box_h >= 1-m` | `m = edge_margin = 0.02` |
| Eyeline well above the upper-third line | `eye_y < 1/3 - t` | `t = eyeline_tolerance = 0.08` |
| Eyeline well below the upper-third line | `eye_y > 1/3 + t` | same |
| Box height below minimum fraction | `box_h < 0.18` | `min_box_height` |
| Box height above maximum fraction | `box_h > 0.45` | `max_box_height` |
| Horizontal centre off by more than tolerance | `abs(box_centre_x - 0.5) > 0.12` | `centre_tolerance` |

- [ ] **Step 1: Write the failing test**

Create `tests/framing-advice-test.cpp`:

```cpp
// Every framing condition, tested at its boundary. These predicates are what a
// panelist sees on a return feed, so an off-by-a-threshold here is somebody
// being told to move when they are fine — which is worse than saying nothing.
#include "zoom-framing-advice.h"

#include <iostream>
#include <string>

static int g_failures = 0;

static void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

// A subject that is framed correctly by every rule: centred, box 0.30 tall,
// eyeline exactly on the upper third, well clear of every edge.
static SubjectFrame good()
{
    SubjectFrame f{};
    f.found = true;
    f.box_w = 0.20f; f.box_h = 0.30f;
    f.box_x = 0.40f; f.box_y = 0.20f;
    f.eye_l_x = 0.46f; f.eye_r_x = 0.54f;
    f.eye_l_y = 1.0f / 3.0f; f.eye_r_y = 1.0f / 3.0f;
    f.confidence = 0.9f;
    f.detected_ns = 1;
    return f;
}

int main()
{
    const FramingThresholds t;
    const FramingCondition none = FramingCondition::Good;

    check(evaluate_framing(good(), t, none) == FramingCondition::Good,
          "a correctly framed subject is Good");

    // ── No face ──────────────────────────────────────────────────────────────
    {
        SubjectFrame f{};  // found == false: the detector's hold already expired
        check(evaluate_framing(f, t, none) == FramingCondition::NoSubject,
              "a lost subject reads as NoSubject");
    }

    // ── Cut off, one edge at a time, each side at its boundary ───────────────
    {
        SubjectFrame f = good();
        f.box_x = static_cast<float>(t.edge_margin) + 0.001f;
        check(evaluate_framing(f, t, none) != FramingCondition::CutOffLeft,
              "just inside the left margin is not cut off");
        f.box_x = static_cast<float>(t.edge_margin) - 0.001f;
        check(evaluate_framing(f, t, none) == FramingCondition::CutOffLeft,
              "at the left margin the subject is cut off");

        f = good();
        f.box_x = 1.0f - f.box_w - static_cast<float>(t.edge_margin) + 0.001f;
        check(evaluate_framing(f, t, none) == FramingCondition::CutOffRight,
              "at the right margin the subject is cut off");

        f = good();
        f.box_y = static_cast<float>(t.edge_margin) - 0.001f;
        check(evaluate_framing(f, t, none) == FramingCondition::CutOffTop,
              "at the top margin the subject is cut off");

        f = good();
        f.box_h = 0.30f;
        f.box_y = 1.0f - f.box_h - static_cast<float>(t.edge_margin) + 0.001f;
        check(evaluate_framing(f, t, none) == FramingCondition::CutOffBottom,
              "at the bottom margin the subject is cut off");
    }

    // ── Camera height, both directions, at the tolerance ─────────────────────
    {
        const float third = 1.0f / 3.0f;
        SubjectFrame f = good();
        f.eye_l_y = f.eye_r_y = third - static_cast<float>(t.eyeline_tolerance) + 0.001f;
        check(evaluate_framing(f, t, none) == FramingCondition::Good,
              "inside the eyeline tolerance is still Good");
        f.eye_l_y = f.eye_r_y = third - static_cast<float>(t.eyeline_tolerance) - 0.001f;
        check(evaluate_framing(f, t, none) == FramingCondition::CameraTooHigh,
              "an eyeline above the upper third means lower your camera");
        f.eye_l_y = f.eye_r_y = third + static_cast<float>(t.eyeline_tolerance) + 0.001f;
        check(evaluate_framing(f, t, none) == FramingCondition::CameraTooLow,
              "an eyeline below the upper third means raise your camera");
    }

    // ── Distance, both directions, at the fraction ───────────────────────────
    {
        SubjectFrame f = good();
        f.box_y = 0.30f;
        f.box_h = static_cast<float>(t.min_box_height) + 0.001f;
        check(evaluate_framing(f, t, none) == FramingCondition::Good,
              "just above the minimum box height is Good");
        f.box_h = static_cast<float>(t.min_box_height) - 0.001f;
        check(evaluate_framing(f, t, none) == FramingCondition::TooFar,
              "below the minimum box height means move closer");

        f = good();
        f.box_y = 0.10f;
        f.box_h = static_cast<float>(t.max_box_height) + 0.001f;
        check(evaluate_framing(f, t, none) == FramingCondition::TooClose,
              "above the maximum box height means move back");
    }

    // ── Horizontal centring, both directions, at the tolerance ───────────────
    {
        SubjectFrame f = good();
        const float off = static_cast<float>(t.centre_tolerance) + 0.001f;
        f.box_x = 0.5f - 0.5f * f.box_w + off;  // subject right of centre
        check(evaluate_framing(f, t, none) == FramingCondition::OffCentreRight,
              "a subject right of centre is told to shift left");
        f.box_x = 0.5f - 0.5f * f.box_w - off;
        check(evaluate_framing(f, t, none) == FramingCondition::OffCentreLeft,
              "a subject left of centre is told to shift right");
        f.box_x = 0.5f - 0.5f * f.box_w +
                  static_cast<float>(t.centre_tolerance) - 0.001f;
        check(evaluate_framing(f, t, none) == FramingCondition::Good,
              "inside the centring tolerance is Good");
    }

    // ── Priority: only one thing is ever said, and it is the worst one ───────
    {
        SubjectFrame f = good();
        f.box_x = 0.005f;              // cut off left
        f.box_h = 0.60f;               // and too close
        f.eye_l_y = f.eye_r_y = 0.05f; // and camera too high
        check(evaluate_framing(f, t, none) == FramingCondition::CutOffLeft,
              "being cut off outranks distance and camera height");

        SubjectFrame g2 = good();
        g2.box_y = 0.30f;
        g2.box_h = 0.10f;              // too far
        g2.eye_l_y = g2.eye_r_y = 0.05f;
        check(evaluate_framing(g2, t, none) == FramingCondition::TooFar,
              "distance outranks camera height");
    }

    // ── Hysteresis: a condition already showing needs a wider margin to clear,
    //    so a subject sitting exactly on a threshold does not strobe advice.
    {
        SubjectFrame f = good();
        f.box_y = 0.30f;
        f.box_h = static_cast<float>(t.min_box_height) + 0.005f;
        check(evaluate_framing(f, t, FramingCondition::Good) ==
                  FramingCondition::Good,
              "from Good, just inside the threshold stays Good");
        check(evaluate_framing(f, t, FramingCondition::TooFar) ==
                  FramingCondition::TooFar,
              "from TooFar, the same frame still reads TooFar");
        f.box_h = static_cast<float>(t.min_box_height) * 1.30f;
        check(evaluate_framing(f, t, FramingCondition::TooFar) ==
                  FramingCondition::Good,
              "clearing by the hysteresis margin returns to Good");
    }

    if (g_failures == 0) std::cout << "framing-advice: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register the target and run it to verify it fails**

In `CMakeLists.txt`, inside `if(BUILD_TESTING)`, after `CoreVideoAutoFrameTest`:

```cmake
    # Framing advice predicates, each pinned at its boundary. What a panelist
    # is told on the return feed.
    add_executable(CoreVideoFramingAdviceTest
        tests/framing-advice-test.cpp
    )
    target_include_directories(CoreVideoFramingAdviceTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoFramingAdvice
             COMMAND CoreVideoFramingAdviceTest)
```

Run: `cmake --build build --config Release --parallel 8 --target CoreVideoFramingAdviceTest`
Expected: FAIL — `Cannot open include file: 'zoom-framing-advice.h'`

- [ ] **Step 3: Write the header**

Create `src/zoom-framing-advice.h`:

```cpp
#pragma once

// Why this exists
// ---------------
// The return feed tells the panelist currently on air exactly one thing about
// their framing. This file decides which thing. Every rule is a geometric
// predicate on the detector's normalized box and eye landmarks, with an
// explicit threshold, because "looks badly framed" is not testable and
// "box_h < 0.18" is.
//
// Three decisions worth keeping:
//
//  * ONE condition at a time, chosen by a fixed priority. A panel guest given
//    three instructions does none of them. Being cut off outranks everything
//    because it is the only condition where part of them is missing; distance
//    outranks camera height because moving changes both.
//
//  * HYSTERESIS on top of the detector's smoothing. The detector stabilises the
//    BOX; it cannot know that a box hovering at box_h = 0.180 will strobe the
//    advice between "move closer" and nothing. A condition already on screen
//    must clear its threshold by kHysteresis before it goes away.
//
//  * Eyeline direction follows the spec table literally: an eyeline ABOVE the
//    upper-third line (a smaller y) means the camera is too high and wants
//    lowering. Getting this backwards is the single easiest mistake here and
//    the reason both directions are tested.

#include "zoom-subject-source.h"

#include <algorithm>
#include <cmath>

enum class FramingCondition {
    Good = 0,
    NoSubject,       // "Step into frame"
    CutOffLeft,      // "You're cut off - move right"
    CutOffRight,     // "You're cut off - move left"
    CutOffTop,       // "You're cut off - move down"
    CutOffBottom,    // "You're cut off - move up"
    TooClose,        // "Move back"
    TooFar,          // "Move closer"
    CameraTooHigh,   // "Lower your camera"
    CameraTooLow,    // "Raise your camera"
    OffCentreLeft,   // subject left of centre  -> "Shift right"
    OffCentreRight,  // subject right of centre -> "Shift left"
};

struct FramingThresholds {
    // How close to a frame edge counts as cut off. 2% of the frame: at 640x360
    // that is 13 px horizontally, which is inside the detector's own box
    // slop, so anything tighter would fire on a well-framed subject.
    double edge_margin = 0.02;
    // The broadcast upper-third line, and how far off it is acceptable. 8% of
    // frame height is about half a head at a normal medium shot - visibly
    // wrong, not pedantically wrong.
    double eyeline_target = 1.0 / 3.0;
    double eyeline_tolerance = 0.08;
    // Head height as a fraction of frame height. Below 0.18 the face is too
    // small to read at gallery size; above 0.45 the shot is a close-up that
    // crops the shoulders.
    double min_box_height = 0.18;
    double max_box_height = 0.45;
    // How far the subject's horizontal centre may sit from frame centre.
    double centre_tolerance = 0.12;
    // Multiplier applied to a threshold that is currently FIRING, so leaving a
    // condition takes more movement than entering it. 1.3 is roughly a third
    // of a tolerance band of dead zone.
    double hysteresis = 1.3;
};

inline bool framing_condition_is(FramingCondition c, FramingCondition want)
{
    return c == want;
}

// Returns the single condition to show. `previous` is what is on screen right
// now; pass FramingCondition::Good on the first evaluation.
inline FramingCondition evaluate_framing(const SubjectFrame &s,
                                         const FramingThresholds &t,
                                         FramingCondition previous)
{
    if (!s.found) return FramingCondition::NoSubject;

    const double bx = static_cast<double>(s.box_x);
    const double by = static_cast<double>(s.box_y);
    const double bw = static_cast<double>(s.box_w);
    const double bh = static_cast<double>(s.box_h);
    const double eye_y = 0.5 * (static_cast<double>(s.eye_l_y) +
                                static_cast<double>(s.eye_r_y));
    if (!std::isfinite(bx) || !std::isfinite(by) || !std::isfinite(bw) ||
        !std::isfinite(bh) || !std::isfinite(eye_y) || bw <= 0.0 || bh <= 0.0)
        return FramingCondition::NoSubject;

    const double hys = std::max(t.hysteresis, 1.0);
    // A threshold is widened only for the condition that is currently showing,
    // which is what makes leaving harder than entering.
    const auto band = [&](FramingCondition c, double tolerance) {
        return previous == c ? tolerance * hys : tolerance;
    };

    // 1. Cut off - part of them is missing, so nothing else matters.
    if (bx <= band(FramingCondition::CutOffLeft, t.edge_margin))
        return FramingCondition::CutOffLeft;
    if (bx + bw >= 1.0 - band(FramingCondition::CutOffRight, t.edge_margin))
        return FramingCondition::CutOffRight;
    if (by <= band(FramingCondition::CutOffTop, t.edge_margin))
        return FramingCondition::CutOffTop;
    if (by + bh >= 1.0 - band(FramingCondition::CutOffBottom, t.edge_margin))
        return FramingCondition::CutOffBottom;

    // 2. Distance - moving fixes framing and eyeline together, so it comes
    //    before camera height.
    if (bh > t.max_box_height *
                 (previous == FramingCondition::TooClose ? 1.0 / hys : 1.0))
        return FramingCondition::TooClose;
    if (bh < t.min_box_height *
                 (previous == FramingCondition::TooFar ? hys : 1.0))
        return FramingCondition::TooFar;

    // 3. Camera height. Above the line (smaller y) => the camera is too high.
    const double eye_offset = eye_y - t.eyeline_target;
    if (eye_offset < -band(FramingCondition::CameraTooHigh, t.eyeline_tolerance))
        return FramingCondition::CameraTooHigh;
    if (eye_offset > band(FramingCondition::CameraTooLow, t.eyeline_tolerance))
        return FramingCondition::CameraTooLow;

    // 4. Horizontal placement, last because it is the least damaging.
    const double centre_offset = (bx + 0.5 * bw) - 0.5;
    if (centre_offset > band(FramingCondition::OffCentreRight, t.centre_tolerance))
        return FramingCondition::OffCentreRight;
    if (centre_offset < -band(FramingCondition::OffCentreLeft, t.centre_tolerance))
        return FramingCondition::OffCentreLeft;

    return FramingCondition::Good;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```
cmake --build build --config Release --parallel 8 --target CoreVideoFramingAdviceTest
ctest -C Release -R CoreVideoFramingAdvice --output-on-failure
```
Expected: PASS, `framing-advice: all checks passed`

- [ ] **Step 5: Commit**

```bash
git add src/zoom-framing-advice.h tests/framing-advice-test.cpp CMakeLists.txt
git commit -m "feat: framing advice predicates with explicit thresholds"
```

---

## Task 9: Overlay layout, legible at 640×360 (pure)

**The constraint:** a gallery tile occupies roughly **640×360 of physical screen space** on a panelist's display. Spotlight is explicitly not required and must not be assumed, so 640×360 is the design size, not 1080p.

**The consequence, decided here:** the overlay draws **no text at all**. Words would need either a font atlas or a nested OBS text source, and at 640×360 a legible word is a very small number of very large glyphs — a worse signal than an arrow. So each condition maps to a geometric symbol built from solid quads: direction arrows (which way to move), inward/outward brackets (distance), a camera-tilt chevron pair (height), and a full-frame pulsing border (no subject). Every quad has a short side of at least 24 px at 640×360, which is ~6.7% of frame height — no hairlines, nothing that survives a Zoom re-encode as mush.

**Files:**
- Create: `src/zoom-framing-overlay-layout.h`
- Create: `tests/framing-overlay-layout-test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `FramingCondition` (Task 8).
- Produces: `struct OverlayQuad { double x, y, w, h; uint32_t argb; }`, `std::vector<OverlayQuad> solve_overlay_quads(FramingCondition, double canvas_w, double canvas_h, uint32_t phase)`, and `kOverlayMinShortSidePx`.

- [ ] **Step 1: Write the failing test**

Create `tests/framing-overlay-layout-test.cpp`:

```cpp
// The overlay is consumed at GALLERY-TILE size - roughly 640x360 of physical
// screen space on a panelist's display, whatever the encode. Spotlight is not
// required and is not assumed. So this file's job is to prove, at 640x360,
// that every element is chunky, on-canvas, and high contrast, for every
// condition. There is no headless GPU harness in this repo and one has been
// ruled against, so this is the verification.
#include "zoom-framing-overlay-layout.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

static int g_failures = 0;

static void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

static const FramingCondition kAll[] = {
    FramingCondition::Good,          FramingCondition::NoSubject,
    FramingCondition::CutOffLeft,    FramingCondition::CutOffRight,
    FramingCondition::CutOffTop,     FramingCondition::CutOffBottom,
    FramingCondition::TooClose,      FramingCondition::TooFar,
    FramingCondition::CameraTooHigh, FramingCondition::CameraTooLow,
    FramingCondition::OffCentreLeft, FramingCondition::OffCentreRight,
};

// Relative luminance, sRGB, for the contrast ratio below.
static double luminance(uint32_t argb)
{
    const auto channel = [](uint32_t v) {
        const double c = static_cast<double>(v) / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    const double r = channel((argb >> 16) & 0xFF);
    const double g = channel((argb >> 8) & 0xFF);
    const double b = channel(argb & 0xFF);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

int main()
{
    // The design size. Not 1080p - see the header comment.
    const double w = 640.0, h = 360.0;

    for (const FramingCondition c : kAll) {
        const std::vector<OverlayQuad> quads = solve_overlay_quads(c, w, h, 0);

        if (c == FramingCondition::Good) {
            check(quads.empty(),
                  "a correctly framed panelist is shown nothing at all");
            continue;
        }
        check(!quads.empty(), "every advice condition draws something");

        double covered = 0.0;
        for (const OverlayQuad &q : quads) {
            check(std::min(q.w, q.h) >= kOverlayMinShortSidePx,
                  "no element is thinner than the minimum at 640x360");
            check(q.x >= 0.0 && q.y >= 0.0 && q.x + q.w <= w && q.y + q.h <= h,
                  "every element is inside the canvas");
            check(((q.argb >> 24) & 0xFF) >= 0xC0,
                  "every element is near-opaque - no washed-out overlays");

            // Hard contrast against BOTH a black and a white background, since
            // we are drawn over arbitrary video.
            const double lum = luminance(q.argb);
            const double vs_black = (lum + 0.05) / 0.05;
            const double vs_white = 1.05 / (lum + 0.05);
            check(std::max(vs_black, vs_white) >= 4.5,
                  "every element clears 4.5:1 against black or white");

            covered += q.w * q.h;
        }
        // Big enough to read across a room, on a tile the size of a postcard.
        check(covered >= 0.04 * w * h,
              "the symbol occupies a readable share of the tile");
    }

    // NoSubject pulses: the phase changes the drawing, so a panelist who has
    // walked away sees motion rather than a static frame they mistake for a
    // frozen picture.
    {
        const std::vector<OverlayQuad> a =
            solve_overlay_quads(FramingCondition::NoSubject, w, h, 0);
        const std::vector<OverlayQuad> b =
            solve_overlay_quads(FramingCondition::NoSubject, w, h, 30);
        check(a.size() == b.size(), "the pulse does not change the element count");
        bool differs = false;
        for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
            if (a[i].argb != b[i].argb) differs = true;
        check(differs, "the no-subject symbol actually pulses");
    }

    // Arrows point the way the panelist must MOVE. Cut off on the left means
    // move right, so the arrow's mass sits right of centre.
    {
        const auto mass_centre_x = [&](FramingCondition c) {
            const std::vector<OverlayQuad> q = solve_overlay_quads(c, w, h, 0);
            double sum = 0.0, area = 0.0;
            for (const OverlayQuad &e : q) {
                sum += (e.x + 0.5 * e.w) * e.w * e.h;
                area += e.w * e.h;
            }
            return area > 0.0 ? sum / area : 0.5 * w;
        };
        check(mass_centre_x(FramingCondition::CutOffLeft) > 0.5 * w,
              "cut off on the left points right");
        check(mass_centre_x(FramingCondition::CutOffRight) < 0.5 * w,
              "cut off on the right points left");
        check(mass_centre_x(FramingCondition::OffCentreRight) < 0.5 * w,
              "a subject right of centre is pointed left");
        check(mass_centre_x(FramingCondition::OffCentreLeft) > 0.5 * w,
              "a subject left of centre is pointed right");
    }

    // And it must still be sane at 1080p, where an operator previews it.
    for (const FramingCondition c : kAll) {
        const std::vector<OverlayQuad> quads =
            solve_overlay_quads(c, 1920.0, 1080.0, 0);
        for (const OverlayQuad &q : quads)
            check(q.x >= 0.0 && q.y >= 0.0 && q.x + q.w <= 1920.0 &&
                  q.y + q.h <= 1080.0,
                  "every element is inside a 1080p canvas too");
    }

    // A canvas too small to draw a legible symbol draws NOTHING rather than
    // hairlines. Illegible advice is worse than none.
    {
        const std::vector<OverlayQuad> tiny =
            solve_overlay_quads(FramingCondition::TooFar, 64.0, 36.0, 0);
        check(tiny.empty(), "a tiny canvas draws nothing rather than hairlines");
    }

    if (g_failures == 0) std::cout << "framing-overlay-layout: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register the target and run it to verify it fails**

In `CMakeLists.txt`, inside `if(BUILD_TESTING)`, after `CoreVideoFramingAdviceTest`:

```cmake
    # Overlay legibility at 640x360 - the physical size of a gallery tile on a
    # panelist's screen. Chunky, on-canvas, high-contrast, no hairlines.
    add_executable(CoreVideoFramingOverlayLayoutTest
        tests/framing-overlay-layout-test.cpp
    )
    target_include_directories(CoreVideoFramingOverlayLayoutTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoFramingOverlayLayout
             COMMAND CoreVideoFramingOverlayLayoutTest)
```

Run: `cmake --build build --config Release --parallel 8 --target CoreVideoFramingOverlayLayoutTest`
Expected: FAIL — `Cannot open include file: 'zoom-framing-overlay-layout.h'`

- [ ] **Step 3: Write the header**

Create `src/zoom-framing-overlay-layout.h`:

```cpp
#pragma once

// Why this exists
// ---------------
// The framing overlay is consumed by a panelist looking at a GALLERY TILE:
// roughly 640x360 of physical screen space on their display, however clean the
// Zoom encode is. Spotlighting the return feed would not change that and is
// explicitly NOT required, so 640x360 is the design size and 1080p is only
// where the operator previews it.
//
// At that size the overlay draws NO TEXT. Words need a font atlas or a nested
// text source, and a word legible at 640x360 is a handful of enormous glyphs -
// a worse signal than an arrow that means the same thing. So every condition
// is a symbol built from solid quads:
//
//   cut off / off centre  -> a thick arrow pointing the way to MOVE
//   too close / too far   -> brackets closing in / opening out
//   camera too high / low -> a chevron pair pointing the way to TILT
//   no subject            -> a pulsing full-frame border
//   good                  -> nothing at all
//
// Rules the test enforces, so they cannot quietly rot:
//   * every quad's short side >= kOverlayMinShortSidePx at 640x360,
//   * every quad inside the canvas,
//   * alpha >= 0xC0 and >= 4.5:1 contrast against black or white,
//   * the symbol covers >= 4% of the frame,
//   * a canvas too small for a legible symbol draws nothing.
//
// Everything is expressed as a FRACTION of the canvas and scaled at the end, so
// 640x360 and 1920x1080 are the same picture.

#include "zoom-framing-advice.h"

#include <cstdint>
#include <vector>

struct OverlayQuad {
    double   x = 0.0, y = 0.0, w = 0.0, h = 0.0;  // canvas pixels
    uint32_t argb = 0xFFFFFFFF;
};

// The thinnest anything may be at the 640x360 design size. 24 px is ~6.7% of
// frame height: it survives a Zoom re-encode as a bar, not as mush.
constexpr double kOverlayMinShortSidePx = 24.0;

// The design canvas everything is authored against.
constexpr double kOverlayDesignWidth  = 640.0;
constexpr double kOverlayDesignHeight = 360.0;

// Amber for "fix something", red for "we cannot see you". Both clear 4.5:1
// against white and against black, which matters because we are drawn over
// arbitrary video.
constexpr uint32_t kOverlayAdviceColor = 0xFFE8A317;  // amber
constexpr uint32_t kOverlayAlertColor  = 0xFFE01B24;  // red

namespace overlay_layout_detail {

// A quad in 0..1 canvas fractions, resolved at the end.
struct FracQuad {
    double x, y, w, h;
    uint32_t argb;
};

// A blocky arrow: one shaft plus a staircase head, so it is all axis-aligned
// quads and needs no triangle geometry. dx/dy is the unit direction.
inline void push_arrow(std::vector<FracQuad> &out, double cx, double cy,
                       double length, double thickness, int dx, int dy,
                       uint32_t argb)
{
    if (dx != 0) {
        out.push_back({cx - 0.5 * length, cy - 0.5 * thickness, length,
                       thickness, argb});
        // Three steps of decreasing height at the tip.
        for (int step = 1; step <= 3; ++step) {
            const double step_h = thickness * (1.0 + 2.0 * (3 - step) * 0.6);
            const double step_w = thickness * 0.75;
            const double tip_x = dx > 0
                ? cx + 0.5 * length - step_w * static_cast<double>(step)
                : cx - 0.5 * length + step_w * static_cast<double>(step - 1);
            out.push_back({tip_x, cy - 0.5 * step_h, step_w, step_h, argb});
        }
        return;
    }
    out.push_back({cx - 0.5 * thickness, cy - 0.5 * length, thickness, length,
                   argb});
    for (int step = 1; step <= 3; ++step) {
        const double step_w = thickness * (1.0 + 2.0 * (3 - step) * 0.6);
        const double step_h = thickness * 0.75;
        const double tip_y = dy > 0
            ? cy + 0.5 * length - step_h * static_cast<double>(step)
            : cy - 0.5 * length + step_h * static_cast<double>(step - 1);
        out.push_back({cx - 0.5 * step_w, tip_y, step_w, step_h, argb});
    }
}

// Two vertical bars, either closing in (move back) or opening out (move
// closer), with short returns top and bottom so they read as brackets.
inline void push_brackets(std::vector<FracQuad> &out, bool inward,
                          uint32_t argb)
{
    const double t = 0.05;                  // bar thickness, fraction of width
    const double inset = inward ? 0.30 : 0.12;
    const double top = 0.22, height = 0.56;
    const double arm = 0.10;
    // Left bracket.
    out.push_back({inset, top, t, height, argb});
    out.push_back({inset, top, arm, t * (kOverlayDesignWidth /
                                         kOverlayDesignHeight), argb});
    out.push_back({inset, top + height - t * (kOverlayDesignWidth /
                                              kOverlayDesignHeight),
                   arm, t * (kOverlayDesignWidth / kOverlayDesignHeight), argb});
    // Right bracket, mirrored.
    out.push_back({1.0 - inset - t, top, t, height, argb});
    out.push_back({1.0 - inset - arm, top,
                   arm, t * (kOverlayDesignWidth / kOverlayDesignHeight), argb});
    out.push_back({1.0 - inset - arm,
                   top + height - t * (kOverlayDesignWidth /
                                       kOverlayDesignHeight),
                   arm, t * (kOverlayDesignWidth / kOverlayDesignHeight), argb});
}

// A full-frame border, four bars, alpha pulsing with the phase.
inline void push_border(std::vector<FracQuad> &out, uint32_t phase,
                        uint32_t rgb)
{
    // 0..59 -> a triangle wave between 0xC0 and 0xFF, so it is always at least
    // near-opaque. Integer maths so the test can predict it.
    const uint32_t p = phase % 60u;
    const uint32_t up = p < 30u ? p : 59u - p;   // 0..29
    const uint32_t alpha = 0xC0u + up * 2u;      // 0xC0..0xFA
    const uint32_t argb = (alpha << 24) | (rgb & 0x00FFFFFFu);
    const double tx = 0.06, ty = 0.06 * (kOverlayDesignWidth /
                                         kOverlayDesignHeight);
    out.push_back({0.0, 0.0, 1.0, ty, argb});
    out.push_back({0.0, 1.0 - ty, 1.0, ty, argb});
    out.push_back({0.0, ty, tx, 1.0 - 2.0 * ty, argb});
    out.push_back({1.0 - tx, ty, tx, 1.0 - 2.0 * ty, argb});
}

}  // namespace overlay_layout_detail

// The quads to draw for a condition, in canvas pixels. `phase` is a monotonic
// frame counter used only by the pulsing no-subject border.
inline std::vector<OverlayQuad> solve_overlay_quads(FramingCondition condition,
                                                    double canvas_w,
                                                    double canvas_h,
                                                    uint32_t phase)
{
    using overlay_layout_detail::FracQuad;
    std::vector<OverlayQuad> out;
    if (condition == FramingCondition::Good) return out;
    if (!(canvas_w > 0.0) || !(canvas_h > 0.0)) return out;
    // Below the design size a legible symbol cannot be drawn, and hairlines are
    // worse than nothing. Refuse.
    if (canvas_w < kOverlayDesignWidth || canvas_h < kOverlayDesignHeight)
        return out;

    // Arrow geometry, in fractions. The shaft is 8% of frame height thick,
    // which is 28.8 px at 640x360 - comfortably over the 24 px floor - and the
    // head steps are 75% of that.
    const double thick_y = 0.09;
    const double thick_x = thick_y * (canvas_h / canvas_w);
    const double len_x = 0.52;
    const double len_y = 0.52;

    std::vector<FracQuad> frac;
    switch (condition) {
    case FramingCondition::NoSubject:
        overlay_layout_detail::push_border(frac, phase, kOverlayAlertColor);
        break;
    // Cut off on a side means MOVE AWAY from that side, so the arrow points the
    // other way and its mass sits on the far side of centre. The offset is what
    // the mass-centre assertions in the test key on.
    case FramingCondition::CutOffLeft:
        overlay_layout_detail::push_arrow(frac, 0.62, 0.5, len_x, thick_x, +1, 0,
                                          kOverlayAlertColor);
        break;
    case FramingCondition::CutOffRight:
        overlay_layout_detail::push_arrow(frac, 0.38, 0.5, len_x, thick_x, -1, 0,
                                          kOverlayAlertColor);
        break;
    case FramingCondition::CutOffTop:
        overlay_layout_detail::push_arrow(frac, 0.5, 0.62, len_y, thick_y, 0, +1,
                                          kOverlayAlertColor);
        break;
    case FramingCondition::CutOffBottom:
        overlay_layout_detail::push_arrow(frac, 0.5, 0.38, len_y, thick_y, 0, -1,
                                          kOverlayAlertColor);
        break;
    case FramingCondition::TooClose:
        overlay_layout_detail::push_brackets(frac, /*inward=*/false,
                                             kOverlayAdviceColor);
        break;
    case FramingCondition::TooFar:
        overlay_layout_detail::push_brackets(frac, /*inward=*/true,
                                             kOverlayAdviceColor);
        break;
    // Camera too high => tilt it DOWN => the arrow points down.
    case FramingCondition::CameraTooHigh:
        overlay_layout_detail::push_arrow(frac, 0.5, 0.5, len_y, thick_y, 0, +1,
                                          kOverlayAdviceColor);
        break;
    case FramingCondition::CameraTooLow:
        overlay_layout_detail::push_arrow(frac, 0.5, 0.5, len_y, thick_y, 0, -1,
                                          kOverlayAdviceColor);
        break;
    case FramingCondition::OffCentreRight:
        overlay_layout_detail::push_arrow(frac, 0.38, 0.5, len_x, thick_x, -1, 0,
                                          kOverlayAdviceColor);
        break;
    case FramingCondition::OffCentreLeft:
        overlay_layout_detail::push_arrow(frac, 0.62, 0.5, len_x, thick_x, +1, 0,
                                          kOverlayAdviceColor);
        break;
    case FramingCondition::Good:
    default:
        return out;
    }

    out.reserve(frac.size());
    for (const FracQuad &f : frac) {
        OverlayQuad q;
        q.x = f.x * canvas_w;
        q.y = f.y * canvas_h;
        q.w = f.w * canvas_w;
        q.h = f.h * canvas_h;
        q.argb = f.argb;
        // Clamp into the canvas. Every fraction above is already inside 0..1,
        // so this only absorbs floating-point edge cases - it must never be
        // load-bearing, which is why the test checks bounds independently.
        if (q.x < 0.0) { q.w += q.x; q.x = 0.0; }
        if (q.y < 0.0) { q.h += q.y; q.y = 0.0; }
        if (q.x + q.w > canvas_w) q.w = canvas_w - q.x;
        if (q.y + q.h > canvas_h) q.h = canvas_h - q.y;
        if (q.w <= 0.0 || q.h <= 0.0) continue;
        out.push_back(q);
    }
    return out;
}
```

- [ ] **Step 4: Run the test, and tune the geometry until it passes**

Run:
```
cmake --build build --config Release --parallel 8 --target CoreVideoFramingOverlayLayoutTest
ctest -C Release -R CoreVideoFramingOverlayLayout --output-on-failure
```
Expected: PASS. If the minimum-short-side or coverage assertions fail, raise `thick_y` / `len_x` / `len_y` and the bracket `t` — do **not** relax the assertions, they are the feature's acceptance criteria.

- [ ] **Step 5: Commit**

```bash
git add src/zoom-framing-overlay-layout.h tests/framing-overlay-layout-test.cpp CMakeLists.txt
git commit -m "feat: framing overlay layout, pinned legible at 640x360"
```

---

## Task 10: The `corevideo_framing_overlay` OBS source

**Design note:** the existing `corevideo_active_speaker_source` (`src/zoom-source.cpp:2875`) is an **async** source (`OBS_SOURCE_ASYNC_VIDEO`) — it pushes finished frames and has no render hook to draw into. So the overlay is a **separate custom-draw source** the operator stacks above the active speaker in the return scene. That keeps the async video path completely untouched, and keeps the overlay's decisions on the CPU where they are testable. It polls `SpeakerDirector` — which is **poll-only, there is no observer list** — for who is on air.

**Files:**
- Create: `src/zoom-framing-overlay.h`, `src/zoom-framing-overlay.cpp`
- Modify: `src/zoom-plugin.cpp`, `CMakeLists.txt`, `data/locale/en-US.ini`

**Interfaces:**
- Consumes: `solve_overlay_quads()` (Task 9), `evaluate_framing()` (Task 8), `subject_for()` (Task 5), `ReturnIdentityRegistry` (Task 1), `SpeakerDirector::instance().snapshot()`.
- Produces: `void zoom_framing_overlay_register();`

- [ ] **Step 1: Write the header**

Create `src/zoom-framing-overlay.h`:

```cpp
#pragma once

// The framing-advice overlay source.
//
// A transparent, custom-draw OBS source that shows the panelist currently on
// air one geometric instruction about their framing. Stacked ABOVE
// corevideo_active_speaker_source in the return scene, which goes out over the
// OBS Virtual Camera and back into the meeting on a separate Zoom seat.
//
// Separate from the active-speaker source because that source is ASYNC
// (OBS_SOURCE_ASYNC_VIDEO): it pushes finished frames and has no render hook to
// draw into. Layering keeps the async video path untouched.
void zoom_framing_overlay_register();
```

- [ ] **Step 2: Write the source**

Create `src/zoom-framing-overlay.cpp`:

```cpp
#include "zoom-framing-overlay.h"

#include "speaker-director.h"
#include "zoom-engine-client.h"
#include "zoom-framing-advice.h"
#include "zoom-framing-overlay-layout.h"
#include "zoom-self-identity.h"
#include "zoom-subject-source.h"

#include <obs-module.h>
#include <util/platform.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

static const char *const kFramingOverlaySourceId = "corevideo_framing_overlay";

// Per-source settings, obs_data + atomics — the PROP_ANIMATE pattern in
// zoom-supersource.cpp, NOT the global-ini ZoomPluginSettings mechanism.
static constexpr const char *PROP_CANVAS_W    = "canvas_width";
static constexpr const char *PROP_CANVAS_H    = "canvas_height";
static constexpr const char *PROP_RETURN_SEAT = "return_seat_name";

// Set COREVIDEO_FRAMING_LAYOUT_TEST to cycle every condition in the REAL
// source, in real OBS, one per second. The same in-product visual check
// COREVIDEO_TALKBACK_LAYOUT_TEST provides for the intercom dock — there is no
// headless GPU harness in this repo and one has been ruled against, so this is
// how a human eyeballs the symbols at tile size.
static const char *const kFramingLayoutTestEnv = "COREVIDEO_FRAMING_LAYOUT_TEST";

struct framing_overlay_source {
    obs_source_t *source = nullptr;

    std::atomic<uint32_t> canvas_width{1920};
    std::atomic<uint32_t> canvas_height{1080};

    // Everything the render reads, written by the tick, snapshotted as a unit.
    std::mutex mutex;
    std::vector<OverlayQuad> quads;

    // Tick-thread state; never touched by render.
    FramingCondition previous = FramingCondition::Good;
    uint32_t phase = 0;
    bool layout_test = false;
};

static const char *framing_overlay_get_name(void *)
{
    return obs_module_text("CoreVideoFramingOverlay.Name");
}

static uint32_t framing_overlay_get_width(void *data)
{
    return static_cast<framing_overlay_source *>(data)->canvas_width.load(
        std::memory_order_acquire);
}

static uint32_t framing_overlay_get_height(void *data)
{
    return static_cast<framing_overlay_source *>(data)->canvas_height.load(
        std::memory_order_acquire);
}

static void framing_overlay_update(void *data, obs_data_t *settings)
{
    auto *ctx = static_cast<framing_overlay_source *>(data);

    // Clamped on the same threat model as every other setting in this plugin:
    // scene files are hand-editable and obs_data_get_int returns an int64.
    const auto clamp_dim = [](int64_t raw, int64_t fallback) {
        if (raw < 160 || raw > 7680) return fallback;
        return raw;
    };
    ctx->canvas_width.store(
        static_cast<uint32_t>(
            clamp_dim(obs_data_get_int(settings, PROP_CANVAS_W), 1920)),
        std::memory_order_release);
    ctx->canvas_height.store(
        static_cast<uint32_t>(
            clamp_dim(obs_data_get_int(settings, PROP_CANVAS_H), 1080)),
        std::memory_order_release);

    // The return seat's display name. This is what makes the exclusion durable:
    // that seat is a separate Zoom account whose user_id changes on every
    // rejoin, so it is matched by name and re-derived on every roster message.
    // See zoom-self-identity.h.
    const char *seat = obs_data_get_string(settings, PROP_RETURN_SEAT);
    ReturnIdentityRegistry::instance().set_names(
        {seat ? std::string(seat) : std::string()});
}

static void framing_overlay_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, PROP_CANVAS_W, 1920);
    obs_data_set_default_int(settings, PROP_CANVAS_H, 1080);
    // Ships EMPTY. Naming the seat is both the destination and the on-switch,
    // exactly as the Tiles audio group is: an empty name must not match every
    // unnamed participant in somebody's meeting on upgrade.
    obs_data_set_default_string(settings, PROP_RETURN_SEAT, "");
}

static obs_properties_t *framing_overlay_get_properties(void *)
{
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_text(props, PROP_RETURN_SEAT,
                            obs_module_text("CoreVideoFramingOverlay.ReturnSeat"),
                            OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, PROP_CANVAS_W,
                           obs_module_text("CoreVideoFramingOverlay.CanvasWidth"),
                           160, 7680, 2);
    obs_properties_add_int(props, PROP_CANVAS_H,
                           obs_module_text("CoreVideoFramingOverlay.CanvasHeight"),
                           160, 4320, 2);
    return props;
}

static void *framing_overlay_create(obs_data_t *settings, obs_source_t *source)
{
    auto *ctx = new framing_overlay_source();
    ctx->source = source;
    ctx->layout_test = os_getenv(kFramingLayoutTestEnv) != nullptr;
    if (ctx->layout_test) {
        blog(LOG_INFO,
             "[obs-zoom-plugin] Framing overlay: %s set — cycling every "
             "condition, one per second, with no detector",
             kFramingLayoutTestEnv);
    }
    framing_overlay_update(ctx, settings);
    return ctx;
}

static void framing_overlay_destroy(void *data)
{
    delete static_cast<framing_overlay_source *>(data);
}

// Who is on air. SpeakerDirector is POLL-ONLY — there is no observer list — so
// this asks it once per tick and does not wait for an event that never comes.
static uint32_t framing_overlay_on_air()
{
    const uint64_t now_ms = os_gettime_ns() / 1000000ULL;
    const SpeakerDirectorSnapshot snap =
        SpeakerDirector::instance().snapshot(now_ms);
    return snap.directed_speaker_id;
}

static void framing_overlay_tick(void *data, float /*seconds*/)
{
    auto *ctx = static_cast<framing_overlay_source *>(data);
    ++ctx->phase;

    FramingCondition condition = FramingCondition::Good;
    if (ctx->layout_test) {
        // One condition per second at 60 fps, in enum order, skipping Good so
        // every symbol is actually seen.
        static const FramingCondition kCycle[] = {
            FramingCondition::NoSubject,     FramingCondition::CutOffLeft,
            FramingCondition::CutOffRight,   FramingCondition::CutOffTop,
            FramingCondition::CutOffBottom,  FramingCondition::TooClose,
            FramingCondition::TooFar,        FramingCondition::CameraTooHigh,
            FramingCondition::CameraTooLow,  FramingCondition::OffCentreLeft,
            FramingCondition::OffCentreRight,
        };
        constexpr uint32_t kCount =
            static_cast<uint32_t>(sizeof(kCycle) / sizeof(kCycle[0]));
        condition = kCycle[(ctx->phase / 60u) % kCount];
    } else {
        const uint32_t on_air = framing_overlay_on_air();
        if (on_air == 0) {
            // Nobody directed: say nothing rather than advising an empty chair.
            condition = FramingCondition::Good;
        } else {
            condition = evaluate_framing(subject_for(on_air), FramingThresholds{},
                                         ctx->previous);
        }
    }
    ctx->previous = condition;

    std::vector<OverlayQuad> quads = solve_overlay_quads(
        condition,
        static_cast<double>(ctx->canvas_width.load(std::memory_order_acquire)),
        static_cast<double>(ctx->canvas_height.load(std::memory_order_acquire)),
        ctx->phase);

    std::lock_guard<std::mutex> lock(ctx->mutex);
    ctx->quads = std::move(quads);
}

static void framing_overlay_render(void *data, gs_effect_t *)
{
    auto *ctx = static_cast<framing_overlay_source *>(data);

    std::vector<OverlayQuad> quads;
    {
        // Snapshotted as a unit, for the same reason the Tiles wall snapshots
        // its crop under ctx->mutex: half of one symbol and half of the next is
        // not a picture anybody can interpret.
        std::lock_guard<std::mutex> lock(ctx->mutex);
        quads = ctx->quads;
    }
    if (quads.empty()) return;  // Good draws nothing at all

    gs_effect_t *const solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    if (!solid) return;
    gs_eparam_t *const color = gs_effect_get_param_by_name(solid, "color");
    if (!color) return;

    gs_blend_state_push();
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

    for (const OverlayQuad &q : quads) {
        const uint32_t w = static_cast<uint32_t>(q.w);
        const uint32_t h = static_cast<uint32_t>(q.h);
        if (w == 0 || h == 0) continue;

        // vec4_from_rgba takes ABGR-ordered bytes; our quads are ARGB, so the
        // red and blue bytes swap here. Getting this wrong is a silent colour
        // bug, not a crash — amber renders as blue and still "works".
        struct vec4 fill;
        const uint32_t a = (q.argb >> 24) & 0xFF;
        const uint32_t r = (q.argb >> 16) & 0xFF;
        const uint32_t g = (q.argb >> 8) & 0xFF;
        const uint32_t b = q.argb & 0xFF;
        vec4_from_rgba(&fill, (a << 24) | (b << 16) | (g << 8) | r);
        gs_effect_set_vec4(color, &fill);

        gs_technique_t *const tech = gs_effect_get_technique(solid, "Solid");
        gs_technique_begin(tech);
        if (gs_technique_begin_pass(tech, 0)) {
            gs_matrix_push();
            gs_matrix_translate3f(static_cast<float>(q.x),
                                  static_cast<float>(q.y), 0.0f);
            gs_draw_sprite(nullptr, 0, w, h);
            gs_matrix_pop();
            gs_technique_end_pass(tech);
        }
        gs_technique_end(tech);
    }

    gs_blend_state_pop();
}

void zoom_framing_overlay_register()
{
    obs_source_info info = {};
    info.id = kFramingOverlaySourceId;
    info.type = OBS_SOURCE_TYPE_INPUT;
    // CUSTOM_DRAW because it binds its own effect; DO_NOT_DUPLICATE because two
    // copies would both poll the director and neither would be wrong, but the
    // second one buys nothing.
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
                        OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name = framing_overlay_get_name;
    info.create = framing_overlay_create;
    info.destroy = framing_overlay_destroy;
    info.update = framing_overlay_update;
    info.video_tick = framing_overlay_tick;
    info.video_render = framing_overlay_render;
    info.get_width = framing_overlay_get_width;
    info.get_height = framing_overlay_get_height;
    info.get_properties = framing_overlay_get_properties;
    info.get_defaults = framing_overlay_get_defaults;
    obs_register_source(&info);
}
```

- [ ] **Step 3: Register it and add the strings**

In `src/zoom-plugin.cpp`, beside the other `*_register()` calls in `obs_module_load`:

```cpp
    zoom_framing_overlay_register();
```

and the include with the others:

```cpp
#include "zoom-framing-overlay.h"
```

In `CMakeLists.txt`, add `src/zoom-framing-overlay.cpp` to the plugin target's source list, beside `src/zoom-supersource.cpp`.

In `data/locale/en-US.ini`:

```ini
CoreVideoFramingOverlay.Name="CoreVideo Framing Advice"
CoreVideoFramingOverlay.ReturnSeat="Return-feed Zoom display name (excluded from tiles and speaker)"
CoreVideoFramingOverlay.CanvasWidth="Canvas width"
CoreVideoFramingOverlay.CanvasHeight="Canvas height"
```

- [ ] **Step 4: Build and run the suite**

Run:
```
cmake --build build --config Release --parallel 8
ctest -C Release --output-on-failure
```
Expected: builds clean, N/N green.

- [ ] **Step 5: In-product visual check at tile size**

Run OBS with the env var set, add a **CoreVideo Framing Advice** source to a scene, and set the preview to roughly 640×360 on screen:

```
cmd /c "set COREVIDEO_FRAMING_LAYOUT_TEST=1 && Launch-OBS-CoreVideo.cmd"
```

Confirm by eye, at that size: every symbol is unmistakable, the arrows point where you would move, nothing is thin, and the no-subject border pulses. This is the sanctioned substitute for a GPU harness — do not add one.

- [ ] **Step 6: Commit**

```bash
git add src/zoom-framing-overlay.h src/zoom-framing-overlay.cpp src/zoom-plugin.cpp CMakeLists.txt data/locale/en-US.ini
git commit -m "feat: corevideo_framing_overlay source for the vcam return feed"
```

---

## Task 11: Documentation and the return-scene runbook

**Files:**
- Modify: `CLAUDE.md`
- Modify: `CHANGELOG.md`
- Create: `docs/framing-return-feed.md`

- [ ] **Step 1: Write the operator runbook**

Create `docs/framing-return-feed.md`:

```markdown
# Framing advice return feed (preshow)

This is a **preshow** tool: green room, mic check, rehearsal. The return feed
is visible to everyone in the meeting, which is the intended delivery, not a
leak. Nothing here touches a live program shot.

## Setting it up

1. In OBS, make a scene containing:
   - **CoreVideo Active Speaker** (the existing `corevideo_active_speaker_source`), and
   - **CoreVideo Framing Advice** on top of it.
2. Start the **OBS Virtual Camera**.
3. On a **second machine or a second Zoom account**, join the meeting and
   select **OBS Virtual Camera** as the webcam. It must be a separate account:
   same-account joins collide (see the ZComms talkback findings).
4. In the Framing Advice source's properties, type that seat's **Zoom display
   name** into *Return-feed Zoom display name*.

Step 4 is not cosmetic. It is what stops the return feed being shown on the
Tiles wall or directed to as the active speaker — which would close a video
feedback loop. The match is by name and is re-derived on every roster message,
so it survives that seat rejoining, which a user-id-based exclusion does not.

The bot's own SDK identity is excluded automatically (the engine stamps it from
`GetMySelfUser()`), including while talkback has the bot unmuted.

## Reading it

| Symbol | Meaning |
|---|---|
| Pulsing red border | We cannot see you — step into frame |
| Red arrow | You are cut off; move the way it points |
| Amber up/down arrow | Tilt your camera the way it points |
| Amber brackets closing in | Move closer |
| Amber brackets opening out | Move back |
| Amber left/right arrow | Shift the way it points |
| Nothing | You are framed correctly |

There is deliberately no text: the overlay is read on a gallery tile roughly
640x360 in physical screen size, and at that size a symbol beats a word.
Spotlighting the return is not required.

## Tiles auto-framing

Off by default, per-wall, in the Tiles source properties. It replaces the
per-tile crop sliders for any tile with a detected subject and falls straight
back to them when the subject is lost.

It never raises the Zoom subscription for anybody. It frames from whatever the
shared feed already is: a participant who is ISO'd or on a program output is
already carried at high resolution and is framed from those pixels for free,
while a participant only the tile wall is showing is carried at 360p and their
auto-framed tile will look softer.

**A soft auto-framed tile is a diagnosis, not a defect.** It means nobody is
paying for that participant's pixels. The remedy is to ISO them, which you can
already do, and Tiles picks the better feed up automatically.
```

- [ ] **Step 2: Update CLAUDE.md**

Add to `CLAUDE.md`, in the section listing sources and pure headers:

```markdown
- **Framing consumers** (`src/zoom-auto-frame.h`, `src/zoom-framing-advice.h`,
  `src/zoom-framing-overlay-layout.h`, `src/zoom-framing-overlay.cpp`): Tiles
  auto-framing and the Virtual-Camera return overlay. All decisions are pure
  headers with pinned tests (`CoreVideoAutoFrame`, `CoreVideoFramingAdvice`,
  `CoreVideoFramingOverlayLayout`); there is no GPU harness and one has been
  ruled against. The overlay is designed and reviewed at **640x360**, the
  physical size of a gallery tile. `COREVIDEO_FRAMING_LAYOUT_TEST` cycles every
  condition in the real source.
- **Identity** (`src/zoom-self-identity.h`): `ParticipantInfo::is_self` is
  stamped by the engine from `GetMySelfUser()` on every roster rebuild;
  `is_return_identity` matches the vcam seat by display name. Both are
  re-derived per roster message, so they survive a rejoin, and both are refused
  by tile assignment AND by `SpeakerDirector`. Do not add another
  user-id-keyed exclude combo box — that is the mechanism this replaced.
- **Auto-framing never requests a resolution upgrade.** It consumes whatever
  the shared feed already is — the engine's subscription policy is upgrade-only,
  so an ISO'd or program-output participant is already high-resolution and the
  tile reuses that, while a wall-only participant stays at 360p and looks
  softer. That is the diagnosis, not the defect: ISO them. Adding a second
  resolution lever here would fight the ISO/program controls over the same
  envelope, and a 720p wall throttled a live meeting to 0.3-0.45x real time on
  2026-08-17. `zoom-auto-frame.h`'s maths reads tex_w/tex_h and is pinned
  resolution-invariant.
```

- [ ] **Step 3: Update CHANGELOG.md**

Under the unreleased heading:

```markdown
- Tiles: opt-in per-tile auto-framing (off by default). It frames from whatever
  the shared feed already is and never raises a Zoom subscription, so it adds no
  bandwidth of its own; a wall-only participant's tile will look softer than an
  ISO'd one's, and ISO'ing them is the fix.
- Framing advice return feed: a new **CoreVideo Framing Advice** source that
  tells the panelist on air how to fix their framing, for return over the OBS
  Virtual Camera. Designed to read at gallery-tile size; no spotlight required.
- Durable self/return-identity exclusion: the bot and the return seat can no
  longer appear on the Tiles wall or be directed to as the active speaker, and
  the exclusion survives a rejoin — including while talkback has the bot
  unmuted.
```

- [ ] **Step 4: Final full verification**

Run:
```
cmake --build build --config Release --parallel 8
ctest -C Release --output-on-failure
```
Expected: builds clean, N/N green. Record the actual N/N line in the commit body — evidence before assertions.

- [ ] **Step 5: Commit**

```bash
git add CLAUDE.md CHANGELOG.md docs/framing-return-feed.md
git commit -m "docs: framing return feed runbook and identity notes"
```

---

## Self-review

**Spec coverage (Subsystem 3 only, as scoped):**

| Spec item | Task |
|---|---|
| 3a. Tiles auto-frame, opt-in, per-tile crop rect, eyeline on upper third, centred, clamped to bounds and max zoom, off by default, no resolution upgrade | 6, 7 |
| 3b. Return overlay on the active speaker, over the OBS Virtual Camera | 8, 9, 10 |
| The seven overlay conditions with concrete predicates and boundary tests | 8 |
| Overlay legible at 640×360, chunky, hard contrast, no hairlines, no fine text, spotlight not assumed | 9, 10 |
| 3c. Durable self/return-identity exclusion applied to tiles AND speaker direction, with tests | 1, 2, 3, 4 |
| The four crop-insertion-point constraints, each tested | 6 (aspect, bounds, crop_uv from truncated ints), 7 (snapshot under `ctx->mutex`, and the insertion-point selection rule) |
| Resolution rule: consume the shared feed, never upgrade; crop math resolution-invariant | Resolution-rule block, Task 6 (the 360p-vs-1080p invariance test), Task 7 (no `subscribe()` call) |
| Per-source `obs_data` + atomics, never the global ini | 7, 10 |
| `SpeakerDirector` polled, no observer list | 10 |
| Plain `int main()` + local `check()`, `CoreVideo<Thing>Test`, hand-registered in root CMakeLists | 1, 5, 6, 8, 9 |
| No GPU harness; env-var visual check | 9 (pure), 10 step 5 (`COREVIDEO_FRAMING_LAYOUT_TEST`) |

**Out of scope by instruction, and deliberately absent:** the loudness engine and meter source (Subsystem 1), and the detector engine itself (Subsystem 2) — including the library vendoring, the worker thread, the round-robin schedule, and all smoothing. This plan consumes the detector solely through `subject_for()` (Task 5) and never re-smooths.

**Type consistency:** `SubjectFrame` field names are used exactly as given in the contract everywhere. `AutoFrameResult{bool valid; CropRect crop;}` is produced in Task 6 and consumed under those names in Task 7. `FramingCondition` enumerators are produced in Task 8 and consumed by the same names in Tasks 9 and 10. `OverlayQuad{x,y,w,h,argb}` is produced in Task 9 and consumed in Task 10. `identity_excluded()` is defined in Task 1 and called in Tasks 3 and 4. No task defines or calls any resolution-selection function: nothing in this plan calls `subscribe()`, changes `tile_feed_subscribe`, or names a `VideoResolution`.
