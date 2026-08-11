# Per-Participant Tiles Audio Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Tiles wall auto-creates and maintains one Zoom participant audio source per assigned tile, in an operator-nominated group, so a nine-person wall does not have to be built twice.

**Architecture:** All decision logic is a pure header (`zoom-tiles-audio-plan.h`) that takes the wall's assignment list plus a snapshot of what already exists and returns a list of actions. A thin libobs layer (`zoom-tiles-audio.cpp`) takes the snapshot, applies the actions, and owns every unsafe operation. The Tiles source calls the pair from its existing `apply_assignments` path.

**Tech Stack:** C++17, libobs (OBS 30+), CMake, hand-rolled `int main()` unit tests registered with `add_test`.

## Global Constraints

- Source kind created: **`zoom_participant_audio_source`**, keyed by its existing `participant_id` integer setting.
- Ownership marker settings key: **`cv_tiles_audio_owner`**, holding the creating Tiles source's `obs_source_get_uuid()`.
- The feature is **opt-in and off by default.** Nothing is created until the operator names a group; the group name is the on-switch.
- The Tiles source itself stays **silent**. This plan adds no audio output to the wall.
- The plugin touches **only** sources carrying its marker. Never rename, never remove, never re-point an operator's source.
- Participants leaving the wall are **muted, not deleted**.
- Track 1 is the program mix and **every** created source joins it — that is what gives the operator a live fader per person. Tracks 2–6 carry ISO stems, one each, so **five** participants get stems; the sixth onward is program-only.
- Two Tiles sources showing the same participant must yield **one** audio source. Doubling is the failure this design exists to prevent.
- Tests are plain `int main()`: print a message to `std::cerr` and `return 1` on failure; `std::cout << "<name>: all tests passed\n"; return 0;` at the end.

---

### Task 1: Pure reconciliation and track allocation

**Files:**
- Create: `src/zoom-tiles-audio-plan.h`
- Create: `tests/tiles-audio-plan-test.cpp`
- Modify: `CMakeLists.txt` (add a test target beside the existing `CoreVideoTileGlow` block)

**Interfaces:**
- Consumes: `ParticipantInfo` from `src/zoom-types.h` (fields `user_id`, `display_name`).
- Produces: `TilesAudioSourceState`, `TilesAudioActionKind`, `TilesAudioAction`, `TilesAudioPlanParams`, `TilesAudioPlan`, `kTilesAudioMaxTracks`, `tiles_audio_mixers_for_slot(std::size_t) -> uint32_t`, `plan_tiles_audio(const std::vector<uint32_t>&, const std::vector<TilesAudioSourceState>&, const std::vector<ParticipantInfo>&, const TilesAudioPlanParams&) -> TilesAudioPlan`.

- [ ] **Step 1: Write the failing test**

Create `tests/tiles-audio-plan-test.cpp`:

```cpp
// tests/tiles-audio-plan-test.cpp
// The decision logic behind auto-created participant audio (src/zoom-tiles-audio-plan.h).
//
// Two assertions in here are load-bearing, and both are about damage rather
// than about features:
//
//   1. A participant already owned by a DIFFERENT Tiles source yields no
//      Create. Two walls showing the same person must not carry that person's
//      voice twice — doubling the mix is the exact artefact the whole
//      group-based audio topology exists to prevent.
//   2. A participant who leaves the wall yields Mute, never a removal. A
//      source that vanishes mid-show takes its fader, its filters and any
//      operator tuning with it, and in Auto mode the wall reflows constantly.

#include "zoom-tiles-audio-plan.h"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

static const std::string kSelf  = "uuid-self";
static const std::string kOther = "uuid-other-tiles-source";

static std::vector<ParticipantInfo> roster_of(
    const std::vector<std::pair<uint32_t, std::string>> &people)
{
    std::vector<ParticipantInfo> out;
    for (const auto &p : people) {
        ParticipantInfo info;
        info.user_id      = p.first;
        info.display_name = p.second;
        out.push_back(info);
    }
    return out;
}

static TilesAudioPlanParams params_on()
{
    TilesAudioPlanParams p;
    p.self_uuid = kSelf;
    p.enabled   = true;
    return p;
}

static const TilesAudioAction *find_action(const TilesAudioPlan &plan,
                                           TilesAudioActionKind kind,
                                           uint32_t id)
{
    for (const auto &a : plan.actions)
        if (a.kind == kind && a.participant_id == id) return &a;
    return nullptr;
}

static bool has_any_for(const TilesAudioPlan &plan, uint32_t id)
{
    for (const auto &a : plan.actions)
        if (a.participant_id == id) return true;
    return false;
}

int main()
{
    const auto roster = roster_of({{11, "Ada"}, {22, "Grace"}, {33, "Katherine"}});

    // ── Off by default ───────────────────────────────────────────────────────
    // The switch is checked before anything else. A disabled feature that still
    // computed actions would be one refactor away from applying them.
    {
        TilesAudioPlanParams p = params_on();
        p.enabled = false;
        const TilesAudioPlan plan = plan_tiles_audio({11, 22}, {}, roster, p);
        if (!plan.actions.empty()) {
            std::cerr << "disabled planner emitted " << plan.actions.size()
                      << " action(s); want 0\n";
            return 1;
        }
    }

    // ── Create for a participant with nothing existing ───────────────────────
    {
        const TilesAudioPlan plan = plan_tiles_audio({11}, {}, roster, params_on());
        const TilesAudioAction *a =
            find_action(plan, TilesAudioActionKind::Create, 11);
        if (!a) {
            std::cerr << "no Create emitted for a participant with no source\n";
            return 1;
        }
        // Track 1 (program, bit 0) plus the first stem track 2 (bit 1).
        if (a->mixers != 0x3u) {
            std::cerr << "first slot mixers " << a->mixers << "; want 3 "
                      << "(track 1 program + track 2 stem)\n";
            return 1;
        }
        if (a->name.find("Ada") == std::string::npos) {
            std::cerr << "created name '" << a->name
                      << "' does not carry the roster display name\n";
            return 1;
        }
    }

    // ── Idempotence: a correct existing source produces no actions ───────────
    // Reconciliation runs on every roster change. If a settled wall emitted
    // actions, it would rewrite sources continuously during a show.
    {
        TilesAudioSourceState st;
        st.participant_id = 11;
        st.owner_uuid     = kSelf;
        st.name           = "Ada (CoreVideo)";
        st.muted          = false;
        st.mixers         = 0x3u;
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        if (!plan.actions.empty()) {
            std::cerr << "settled wall emitted " << plan.actions.size()
                      << " action(s); want 0\n";
            return 1;
        }
    }

    // ── A returning participant is unmuted, not recreated ────────────────────
    {
        TilesAudioSourceState st;
        st.participant_id = 11;
        st.owner_uuid     = kSelf;
        st.name           = "Ada (CoreVideo)";
        st.muted          = true;
        st.mixers         = 0x3u;
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        if (!find_action(plan, TilesAudioActionKind::Unmute, 11)) {
            std::cerr << "a muted participant back on the wall was not unmuted\n";
            return 1;
        }
        if (find_action(plan, TilesAudioActionKind::Create, 11)) {
            std::cerr << "a muted existing source was recreated instead of unmuted\n";
            return 1;
        }
    }

    // ── Leaving the wall mutes, never removes ────────────────────────────────
    {
        TilesAudioSourceState st;
        st.participant_id = 22;
        st.owner_uuid     = kSelf;
        st.name           = "Grace (CoreVideo)";
        st.muted          = false;
        st.mixers         = 0x3u;
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        if (!find_action(plan, TilesAudioActionKind::Mute, 22)) {
            std::cerr << "a participant who left the wall was not muted\n";
            return 1;
        }
    }

    // ── An already-muted absentee is left alone ──────────────────────────────
    {
        TilesAudioSourceState st;
        st.participant_id = 22;
        st.owner_uuid     = kSelf;
        st.name           = "Grace (CoreVideo)";
        st.muted          = true;
        st.mixers         = 0x3u;
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        if (has_any_for(plan, 22)) {
            std::cerr << "an already-muted absentee was acted on again\n";
            return 1;
        }
    }

    // ── LOAD-BEARING: another Tiles source owns this participant ─────────────
    // No Create. Two sources for one voice is doubling.
    {
        TilesAudioSourceState st;
        st.participant_id = 11;
        st.owner_uuid     = kOther;
        st.name           = "Ada (CoreVideo)";
        st.muted          = false;
        st.mixers         = 0x3u;
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        if (find_action(plan, TilesAudioActionKind::Create, 11)) {
            std::cerr << "created a duplicate audio source for a participant "
                         "already owned by another Tiles source — this doubles "
                         "their audio\n";
            return 1;
        }
        if (has_any_for(plan, 11)) {
            std::cerr << "acted on a source owned by another Tiles source\n";
            return 1;
        }
    }

    // ── Another source's absentee is not muted by us ─────────────────────────
    // Muting someone else's source would silence the other wall's audio.
    {
        TilesAudioSourceState st;
        st.participant_id = 33;
        st.owner_uuid     = kOther;
        st.name           = "Katherine (CoreVideo)";
        st.muted          = false;
        st.mixers         = 0x3u;
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        if (has_any_for(plan, 33)) {
            std::cerr << "muted or altered a source owned by another Tiles "
                         "source\n";
            return 1;
        }
    }

    // ── An orphan is adopted, not duplicated ─────────────────────────────────
    // The Tiles source that made it was deleted; its sources outlive it.
    {
        TilesAudioSourceState st;
        st.participant_id = 11;
        st.owner_uuid     = "";  // orphaned
        st.name           = "Ada (CoreVideo)";
        st.muted          = true;
        st.mixers         = 0u;
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        if (!find_action(plan, TilesAudioActionKind::Adopt, 11)) {
            std::cerr << "an orphaned source was not adopted\n";
            return 1;
        }
        if (find_action(plan, TilesAudioActionKind::Create, 11)) {
            std::cerr << "an orphaned source was duplicated instead of adopted\n";
            return 1;
        }
        // Adoption must also restore it to service.
        if (!find_action(plan, TilesAudioActionKind::Unmute, 11)) {
            std::cerr << "an adopted source was left muted\n";
            return 1;
        }
        if (!find_action(plan, TilesAudioActionKind::SetMixers, 11)) {
            std::cerr << "an adopted source kept its stale track assignment\n";
            return 1;
        }
    }

    // ── Drifted track assignment is corrected ────────────────────────────────
    {
        TilesAudioSourceState st;
        st.participant_id = 11;
        st.owner_uuid     = kSelf;
        st.name           = "Ada (CoreVideo)";
        st.muted          = false;
        st.mixers         = 0x1u;  // program only; slot 0 should also hold track 2
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        const TilesAudioAction *a =
            find_action(plan, TilesAudioActionKind::SetMixers, 11);
        if (!a) {
            std::cerr << "a drifted track assignment was not corrected\n";
            return 1;
        }
        if (a->mixers != 0x3u) {
            std::cerr << "corrected mixers " << a->mixers << "; want 3\n";
            return 1;
        }
    }

    // ── The six-track ceiling, degrading honestly ────────────────────────────
    // Track 1 is the program mix and everyone joins it, so tracks 2..6 leave
    // exactly five stems. The sixth participant onward is program-only.
    {
        if (tiles_audio_mixers_for_slot(0) != 0x3u) {
            std::cerr << "slot 0 mixers " << tiles_audio_mixers_for_slot(0)
                      << "; want 3\n";
            return 1;
        }
        if (tiles_audio_mixers_for_slot(4) != 0x21u) {
            std::cerr << "slot 4 mixers " << tiles_audio_mixers_for_slot(4)
                      << "; want 33 (program + track 6)\n";
            return 1;
        }
        if (tiles_audio_mixers_for_slot(5) != 0x1u) {
            std::cerr << "slot 5 mixers " << tiles_audio_mixers_for_slot(5)
                      << "; want 1 (program only — stems exhausted)\n";
            return 1;
        }
        // Every slot must always carry the program bit, or that person goes
        // missing from the live mix entirely.
        for (std::size_t slot = 0; slot < 32; ++slot) {
            if ((tiles_audio_mixers_for_slot(slot) & 0x1u) == 0u) {
                std::cerr << "slot " << slot
                          << " is not on the program track — that person would "
                             "be inaudible live\n";
                return 1;
            }
        }

        const auto big_roster = roster_of({{1, "A"}, {2, "B"}, {3, "C"},
                                           {4, "D"}, {5, "E"}, {6, "F"},
                                           {7, "G"}});
        const TilesAudioPlan plan = plan_tiles_audio(
            {1, 2, 3, 4, 5, 6, 7}, {}, big_roster, params_on());
        if (plan.overflow != 2) {
            std::cerr << "overflow " << plan.overflow
                      << " for a 7-person wall; want 2\n";
            return 1;
        }
        const TilesAudioAction *sixth =
            find_action(plan, TilesAudioActionKind::Create, 6);
        if (!sixth || sixth->mixers != 0x1u) {
            std::cerr << "the sixth participant did not degrade to program-only\n";
            return 1;
        }
    }

    // ── A participant absent from the roster still gets a usable name ────────
    {
        const TilesAudioPlan plan =
            plan_tiles_audio({99}, {}, roster, params_on());
        const TilesAudioAction *a =
            find_action(plan, TilesAudioActionKind::Create, 99);
        if (!a || a->name.empty()) {
            std::cerr << "a participant missing from the roster got no name\n";
            return 1;
        }
        if (a->name.find("99") == std::string::npos) {
            std::cerr << "the fallback name '" << a->name
                      << "' does not identify the participant\n";
            return 1;
        }
    }

    std::cout << "tiles-audio-plan: all tests passed\n";
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build build-tests --target CoreVideoTilesAudioPlanTest
```

Expected: FAIL — the target does not exist yet (Step 3 adds it) and `zoom-tiles-audio-plan.h` is missing.

- [ ] **Step 3: Write the implementation**

Create `src/zoom-tiles-audio-plan.h`:

```cpp
// src/zoom-tiles-audio-plan.h
// Decides what to do about per-participant audio for a Tiles wall: what to
// create, what to adopt, what to mute, what to leave strictly alone.
//
// Pure by design, and the purity is not stylistic. Every action in the output
// mutates the operator's scene collection, so the rules that decide them are
// the part that has to be provable without a rig — a bug here either doubles
// somebody's audio on air or damages a scene the operator built by hand.

#pragma once

#include "zoom-types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// OBS provides six audio tracks. Track 1 is the program mix.
inline constexpr std::size_t kTilesAudioMaxTracks = 6;

// One audio source the plugin owns or could adopt, as read off the scene
// collection before planning. Only sources carrying the ownership marker are
// ever represented here; an operator's own sources are invisible to the
// planner and therefore cannot be chosen as targets.
struct TilesAudioSourceState {
    uint32_t    participant_id = 0;
    std::string name;
    std::string owner_uuid;  // empty => orphaned: marked as ours, owner gone
    bool        muted        = false;
    uint32_t    mixers       = 0;  // bitmask; bit 0 = track 1
};

enum class TilesAudioActionKind {
    Create,     // nothing exists for this participant
    Adopt,      // an orphan exists; take ownership of it
    Unmute,     // back on the wall
    Mute,       // left the wall — never deleted
    SetMixers,  // track assignment drifted, or was just adopted
};

struct TilesAudioAction {
    TilesAudioActionKind kind           = TilesAudioActionKind::Create;
    uint32_t             participant_id = 0;
    std::string          name;        // Create only
    uint32_t             mixers       = 0;  // Create and SetMixers
};

struct TilesAudioPlanParams {
    std::string self_uuid;        // this Tiles source's obs_source_get_uuid()
    bool        enabled = false;  // false => no actions at all
};

struct TilesAudioPlan {
    std::vector<TilesAudioAction> actions;
    std::size_t                   overflow = 0;  // participants past the stems
};

// Track 1 (bit 0) is the program mix and every source joins it, which is what
// gives the operator a live fader for everyone. Tracks 2..6 carry one ISO stem
// each — five of them — so the sixth participant onward is program-only. That
// is a real ceiling in OBS, not a limit worth pretending around: the plan
// reports the overflow so it can be logged rather than silently swallowed.
inline uint32_t tiles_audio_mixers_for_slot(std::size_t slot)
{
    constexpr uint32_t kProgram = 1u;
    if (slot + 1 >= kTilesAudioMaxTracks) return kProgram;
    return kProgram | (1u << static_cast<uint32_t>(slot + 1));
}

inline TilesAudioPlan plan_tiles_audio(
    const std::vector<uint32_t>              &assignments,
    const std::vector<TilesAudioSourceState> &existing,
    const std::vector<ParticipantInfo>       &roster,
    const TilesAudioPlanParams               &params)
{
    TilesAudioPlan plan;
    if (!params.enabled) return plan;

    const auto find_existing =
        [&existing](uint32_t id) -> const TilesAudioSourceState * {
        for (const auto &s : existing)
            if (s.participant_id == id) return &s;
        return nullptr;
    };

    const auto display_name = [&roster](uint32_t id) -> std::string {
        for (const auto &p : roster)
            if (p.user_id == id && !p.display_name.empty())
                return p.display_name + " (CoreVideo)";
        // A participant can be assigned but momentarily absent from the roster
        // (a manual tile held for someone who has not rejoined). The id keeps
        // the source identifiable rather than anonymous.
        return "Participant " + std::to_string(id) + " (CoreVideo)";
    };

    std::vector<uint32_t> handled;
    handled.reserve(assignments.size());

    std::size_t slot = 0;
    for (const uint32_t id : assignments) {
        if (id == 0) continue;
        // resolve_tile_assignments already de-duplicates, but a duplicate here
        // would mean two slots claiming one voice, so it is dropped explicitly.
        if (std::find(handled.begin(), handled.end(), id) != handled.end())
            continue;
        handled.push_back(id);

        const TilesAudioSourceState *cur = find_existing(id);

        // Owned by a different Tiles source: not ours to create, mute, or
        // retrack. Creating a second source here would carry this participant's
        // voice twice and double them in the mix.
        if (cur && !cur->owner_uuid.empty() && cur->owner_uuid != params.self_uuid) {
            ++slot;
            continue;
        }

        const uint32_t want = tiles_audio_mixers_for_slot(slot);
        if (slot + 1 >= kTilesAudioMaxTracks) ++plan.overflow;

        if (!cur) {
            TilesAudioAction a;
            a.kind           = TilesAudioActionKind::Create;
            a.participant_id = id;
            a.name           = display_name(id);
            a.mixers         = want;
            plan.actions.push_back(std::move(a));
            ++slot;
            continue;
        }

        // An orphan: marked as ours, but its creating Tiles source is gone.
        // Adopting beats creating — the operator's fader and any filters they
        // added to it survive.
        if (cur->owner_uuid.empty()) {
            TilesAudioAction a;
            a.kind           = TilesAudioActionKind::Adopt;
            a.participant_id = id;
            plan.actions.push_back(a);
        }

        if (cur->muted) {
            TilesAudioAction a;
            a.kind           = TilesAudioActionKind::Unmute;
            a.participant_id = id;
            plan.actions.push_back(a);
        }
        if (cur->mixers != want) {
            TilesAudioAction a;
            a.kind           = TilesAudioActionKind::SetMixers;
            a.participant_id = id;
            a.mixers         = want;
            plan.actions.push_back(a);
        }
        ++slot;
    }

    // Anyone we own who is no longer on the wall is muted and kept. Deleting
    // would take their fader, their filters and any operator tuning with them,
    // and in Auto mode the wall reflows constantly.
    for (const auto &s : existing) {
        if (s.owner_uuid != params.self_uuid) continue;
        if (std::find(handled.begin(), handled.end(), s.participant_id) !=
            handled.end())
            continue;
        if (s.muted) continue;
        TilesAudioAction a;
        a.kind           = TilesAudioActionKind::Mute;
        a.participant_id = s.participant_id;
        plan.actions.push_back(a);
    }

    return plan;
}
```

- [ ] **Step 4: Register the test target**

In `CMakeLists.txt`, immediately after the `add_test(NAME CoreVideoTileGlow ...)` block, add:

```cmake
    # Per-participant audio reconciliation. Pure — no libobs — because every
    # action it emits mutates the operator's scene collection, and the two
    # rules that matter (never duplicate a participant another Tiles source
    # owns, never delete anyone) have to be provable off-rig.
    add_executable(CoreVideoTilesAudioPlanTest
        tests/tiles-audio-plan-test.cpp
    )
    target_include_directories(CoreVideoTilesAudioPlanTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTilesAudioPlan
             COMMAND CoreVideoTilesAudioPlanTest)
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build build-tests --target CoreVideoTilesAudioPlanTest
ctest --test-dir build-tests -R CoreVideoTilesAudioPlan --output-on-failure
```

Expected: PASS, printing `tiles-audio-plan: all tests passed`.

- [ ] **Step 6: Run the whole suite to confirm nothing regressed**

```bash
ctest --test-dir build-tests --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/zoom-tiles-audio-plan.h tests/tiles-audio-plan-test.cpp CMakeLists.txt
git commit -m "feat: pure reconciliation for per-participant Tiles audio

Decides create/adopt/unmute/mute/retrack from the wall's assignments and
a snapshot of what exists. Two rules carry the risk and both are tested:
a participant owned by another Tiles source is never duplicated (that
would double their audio), and leaving the wall mutes rather than
deletes."
```

---

### Task 2: The libobs executor

**Files:**
- Create: `src/zoom-tiles-audio.h`
- Create: `src/zoom-tiles-audio.cpp`
- Modify: `CMakeLists.txt` (add `src/zoom-tiles-audio.cpp` to the `obs-zoom-plugin` source list, near `src/zoom-tiles-background.cpp`)

**Interfaces:**
- Consumes: `TilesAudioPlan`, `TilesAudioAction`, `TilesAudioActionKind`, `TilesAudioSourceState` from Task 1's `src/zoom-tiles-audio-plan.h`.
- Produces: `CV_TILES_AUDIO_OWNER_KEY`, `tiles_audio_scan() -> std::vector<TilesAudioSourceState>`, `tiles_audio_apply(const TilesAudioPlan &, const std::string &group_name, const std::string &self_uuid) -> void`.

This task changes no behaviour on its own — nothing calls these functions until Task 3. That is deliberate: it keeps the unsafe libobs work reviewable in isolation, and leaves the tree in a safe state if the plan is interrupted here.

- [ ] **Step 1: Write the header**

Create `src/zoom-tiles-audio.h`:

```cpp
// src/zoom-tiles-audio.h
// The libobs half of per-participant Tiles audio: reads what exists, applies
// what the planner decided. Every scene-collection mutation in this feature
// happens here and nowhere else.

#pragma once

#include "zoom-tiles-audio-plan.h"

#include <string>
#include <vector>

// Written into every source this feature creates, holding the creating Tiles
// source's obs_source_get_uuid(). Ownership is decided by this marker and
// never by name: the operator can rename anything at any time, and a name
// match would eventually let the plugin adopt — and mute — a source it did
// not create.
#define CV_TILES_AUDIO_OWNER_KEY "cv_tiles_audio_owner"

// Snapshots every marked source in the scene collection. Takes no arguments on
// purpose: ownership is read off each source's own marker, never inferred from
// who is asking. A marker naming a uuid with no live source is reported with an
// empty owner_uuid, which the planner reads as an adoptable orphan.
std::vector<TilesAudioSourceState> tiles_audio_scan();

// Applies the plan. Creates into group_name, which must already exist —
// creating the group is the operator's act, and is also how they opt in.
void tiles_audio_apply(const TilesAudioPlan &plan, const std::string &group_name,
                       const std::string &self_uuid);
```

- [ ] **Step 2: Write the implementation**

Create `src/zoom-tiles-audio.cpp`:

```cpp
// src/zoom-tiles-audio.cpp
#include "zoom-tiles-audio.h"

#include <obs.h>
#include <util/base.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char *kParticipantAudioId = "zoom_participant_audio_source";
constexpr const char *kParticipantIdKey   = "participant_id";

struct ScanCtx {
    std::vector<TilesAudioSourceState> *out;
};

// True if a source with this uuid is still in the scene collection. A marker
// naming a deleted Tiles source is what makes its audio sources orphans.
bool owner_is_live(const char *uuid)
{
    if (!uuid || !*uuid) return false;
    obs_source_t *owner = obs_get_source_by_uuid(uuid);
    if (!owner) return false;
    obs_source_release(owner);
    return true;
}

// Finds a marked source by the participant it carries. Returns a new reference
// the caller must release, or nullptr.
obs_source_t *find_marked_source(uint32_t participant_id)
{
    struct FindCtx {
        uint32_t      want;
        obs_source_t *found;
    } ctx{participant_id, nullptr};

    obs_enum_sources(
        [](void *param, obs_source_t *src) -> bool {
            auto *c = static_cast<FindCtx *>(param);
            const char *id = obs_source_get_id(src);
            if (!id || std::strcmp(id, kParticipantAudioId) != 0) return true;

            obs_data_t *settings = obs_source_get_settings(src);
            if (!settings) return true;
            const char *owner =
                obs_data_get_string(settings, CV_TILES_AUDIO_OWNER_KEY);
            const bool marked = owner && *owner;
            const auto pid = static_cast<uint32_t>(
                obs_data_get_int(settings, kParticipantIdKey));
            obs_data_release(settings);

            if (!marked || pid != c->want) return true;
            c->found = obs_source_get_ref(src);
            return false;  // stop enumerating
        },
        &ctx);

    return ctx.found;
}

void set_owner(obs_source_t *src, const std::string &self_uuid)
{
    obs_data_t *patch = obs_data_create();
    obs_data_set_string(patch, CV_TILES_AUDIO_OWNER_KEY, self_uuid.c_str());
    obs_source_update(src, patch);
    obs_data_release(patch);
}

}  // namespace

std::vector<TilesAudioSourceState> tiles_audio_scan()
{
    std::vector<TilesAudioSourceState> out;
    ScanCtx ctx{&out};

    obs_enum_sources(
        [](void *param, obs_source_t *src) -> bool {
            auto *c = static_cast<ScanCtx *>(param);

            const char *id = obs_source_get_id(src);
            if (!id || std::strcmp(id, kParticipantAudioId) != 0) return true;

            obs_data_t *settings = obs_source_get_settings(src);
            if (!settings) return true;

            const char *owner =
                obs_data_get_string(settings, CV_TILES_AUDIO_OWNER_KEY);
            // No marker means the operator made this by hand. It is not ours,
            // it never becomes ours, and it is not reported to the planner.
            if (owner && *owner) {
                TilesAudioSourceState st;
                st.participant_id = static_cast<uint32_t>(
                    obs_data_get_int(settings, kParticipantIdKey));
                const char *name = obs_source_get_name(src);
                st.name   = name ? name : "";
                st.muted  = obs_source_muted(src);
                st.mixers = obs_source_get_audio_mixers(src);
                // An empty owner_uuid is the planner's signal for "adoptable".
                st.owner_uuid = owner_is_live(owner) ? owner : "";
                c->out->push_back(std::move(st));
            }

            obs_data_release(settings);
            return true;
        },
        &ctx);

    return out;
}

void tiles_audio_apply(const TilesAudioPlan &plan, const std::string &group_name,
                       const std::string &self_uuid)
{
    if (plan.actions.empty()) return;
    if (group_name.empty() || self_uuid.empty()) return;

    obs_source_t *group_src = obs_get_source_by_name(group_name.c_str());
    if (!group_src) {
        blog(LOG_INFO,
             "[corevideo] tiles audio: group '%s' not found; nothing created",
             group_name.c_str());
        return;
    }
    obs_scene_t *group = obs_group_from_source(group_src);
    if (!group) {
        blog(LOG_WARNING,
             "[corevideo] tiles audio: '%s' is not a group; nothing created",
             group_name.c_str());
        obs_source_release(group_src);
        return;
    }

    for (const TilesAudioAction &action : plan.actions) {
        if (action.kind == TilesAudioActionKind::Create) {
            // A name already in use by something the plugin does not own is
            // deferred, never overwritten and never renamed around. The
            // operator's source keeps its name.
            obs_source_t *clash = obs_get_source_by_name(action.name.c_str());
            if (clash) {
                obs_source_release(clash);
                blog(LOG_WARNING,
                     "[corevideo] tiles audio: name '%s' is already taken; "
                     "skipping this participant rather than overwriting",
                     action.name.c_str());
                continue;
            }

            obs_data_t *settings = obs_data_create();
            obs_data_set_int(settings, kParticipantIdKey,
                             static_cast<long long>(action.participant_id));
            obs_data_set_string(settings, CV_TILES_AUDIO_OWNER_KEY,
                                self_uuid.c_str());
            obs_source_t *created = obs_source_create(
                kParticipantAudioId, action.name.c_str(), settings, nullptr);
            obs_data_release(settings);

            if (!created) {
                blog(LOG_WARNING,
                     "[corevideo] tiles audio: could not create a source for "
                     "participant %u",
                     action.participant_id);
                continue;
            }
            obs_source_set_audio_mixers(created, action.mixers);
            obs_source_set_muted(created, false);
            obs_sceneitem_t *item = obs_scene_add(group, created);
            if (!item)
                blog(LOG_WARNING,
                     "[corevideo] tiles audio: created '%s' but could not add "
                     "it to group '%s'",
                     action.name.c_str(), group_name.c_str());
            obs_source_release(created);
            continue;
        }

        obs_source_t *target = find_marked_source(action.participant_id);
        if (!target) continue;

        switch (action.kind) {
        case TilesAudioActionKind::Adopt:
            set_owner(target, self_uuid);
            break;
        case TilesAudioActionKind::Unmute:
            obs_source_set_muted(target, false);
            break;
        case TilesAudioActionKind::Mute:
            obs_source_set_muted(target, true);
            break;
        case TilesAudioActionKind::SetMixers:
            obs_source_set_audio_mixers(target, action.mixers);
            break;
        case TilesAudioActionKind::Create:
            break;  // handled above
        }
        obs_source_release(target);
    }

    if (plan.overflow > 0)
        blog(LOG_INFO,
             "[corevideo] tiles audio: %zu participant(s) past the five ISO "
             "stem tracks; they are on the program track only",
             plan.overflow);

    obs_source_release(group_src);
}
```

- [ ] **Step 3: Add the file to the plugin target**

In `CMakeLists.txt`, in the `obs-zoom-plugin` source list, add `src/zoom-tiles-audio.cpp` on the line after `src/zoom-tiles-background.cpp`.

- [ ] **Step 4: Build the plugin to verify it compiles**

```bash
cmake --build build-rel --config RelWithDebInfo --target obs-zoom-plugin
```

Expected: compiles clean, no new warnings.

- [ ] **Step 5: Confirm the unit suite still passes**

```bash
ctest --test-dir build-tests --output-on-failure
```

Expected: all tests pass, including `CoreVideoTilesAudioPlan`.

- [ ] **Step 6: Commit**

```bash
git add src/zoom-tiles-audio.h src/zoom-tiles-audio.cpp CMakeLists.txt
git commit -m "feat: libobs executor for per-participant Tiles audio

Scans marked sources, applies planned actions, and creates into the
operator's nominated group. Ownership is a settings marker rather than a
name match, so renaming never lets the plugin adopt an operator's own
source; a name clash defers instead of overwriting. Nothing calls this
yet."
```

---

### Task 3: Wire it into the Tiles source

**Files:**
- Modify: `src/zoom-supersource.cpp` (properties, settings, the `apply_assignments` path near line 1418, `tiles_source_update` near line 1439)
- Modify: `data/locale/en-US.ini`
- Modify: `CLAUDE.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: `tiles_audio_scan(const std::string &) -> std::vector<TilesAudioSourceState>` and `tiles_audio_apply(const TilesAudioPlan &, const std::string &, const std::string &)` from Task 2's `src/zoom-tiles-audio.h`; `plan_tiles_audio(...)` and `TilesAudioPlanParams` from Task 1's `src/zoom-tiles-audio-plan.h`.
- Produces: the setting key `audio_group` on `corevideo_tiles_source`, defaulting to `""` (feature off).

- [ ] **Step 1: Add the include and the property key**

In `src/zoom-supersource.cpp`, add to the include block:

```cpp
#include "zoom-tiles-audio.h"
```

and beside the other `PROP_` defines:

```cpp
// Naming a group is both the destination and the on-switch: empty means the
// feature does nothing at all. It ships empty, because this feature writes to
// the operator's scene collection and must not start doing that on upgrade for
// someone who never asked for it.
#define PROP_AUDIO_GROUP "audio_group"
```

- [ ] **Step 2: Default the setting to off**

In the Tiles source's `get_defaults` callback, beside the other `obs_data_set_default_*` calls:

```cpp
    obs_data_set_default_string(settings, PROP_AUDIO_GROUP, "");
```

- [ ] **Step 3: Add the property control**

In the Tiles source's `get_properties` callback, after the background controls, add a dropdown listing the scene collection's groups:

```cpp
    // Editable so a scene file naming a group that does not exist yet round-
    // trips instead of silently resetting to "off" on load.
    obs_property_t *audio_group = obs_properties_add_list(
        props, PROP_AUDIO_GROUP, obs_module_text("Tiles.AudioGroup"),
        OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(audio_group,
                                 obs_module_text("Tiles.AudioGroup.Off"), "");
    obs_enum_sources(
        [](void *param, obs_source_t *src) -> bool {
            if (!obs_group_from_source(src)) return true;
            const char *name = obs_source_get_name(src);
            if (name && *name)
                obs_property_list_add_string(
                    static_cast<obs_property_t *>(param), name, name);
            return true;
        },
        audio_group);
    obs_property_set_long_description(
        audio_group, obs_module_text("Tiles.AudioGroup.Desc"));
```

- [ ] **Step 4: Read the setting in `tiles_source_update`**

Add the field to the `tiles_source` struct (`src/zoom-supersource.cpp:191`), beside the other `ctx->mutex`-guarded settings:

```cpp
    std::string audio_group;  // empty => per-participant audio is off
```

In `tiles_source_update`, read it with the other settings, before the block that takes `ctx->mutex`:

```cpp
    const char *audio_group_raw = obs_data_get_string(settings, PROP_AUDIO_GROUP);
    const std::string audio_group = audio_group_raw ? audio_group_raw : "";
```

Then, inside the existing `std::lock_guard<std::mutex> lock(ctx->mutex);` block in `tiles_source_update` where the fill and crop params are stored, add:

```cpp
        ctx->audio_group = audio_group;
```

so one settings pass lands as a unit, matching how the crop params are already handled.

- [ ] **Step 5: Reconcile audio when the wall changes**

Replace the body of `apply_assignments` (currently at `src/zoom-supersource.cpp:1418`) with:

```cpp
static void apply_assignments(tiles_source *ctx)
{
    std::lock_guard<std::mutex> engine_lock(ctx->engine_mutex);

    // Fetched before ctx->mutex: roster() takes the engine client's lock, and
    // taking them in the other order anywhere would invite a deadlock.
    const std::vector<ParticipantInfo> roster =
        ZoomEngineClient::instance().roster();

    FeedPlan    plan;
    std::string audio_group;
    std::vector<uint32_t> assignments;
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        std::vector<uint32_t> next =
            resolve_tile_assignments(ctx->participants, roster, ctx->fill_params);
        if (ctx->participants == next) return;
        ctx->participants.swap(next);
        plan        = plan_feeds_locked(ctx);
        audio_group = ctx->audio_group;
        assignments = ctx->participants;
    }
    execute_feed_plan(plan);

    // Deliberately outside both locks. Creating sources and adding them to a
    // group makes libobs emit signals that run arbitrary handlers, and this
    // runs on the roster callback thread — holding ctx->mutex across that is
    // how a lock-order inversion gets built by accident.
    if (audio_group.empty()) return;
    const char *uuid = obs_source_get_uuid(ctx->source);
    if (!uuid || !*uuid) return;

    TilesAudioPlanParams params;
    params.self_uuid = uuid;
    params.enabled   = true;
    const TilesAudioPlan audio_plan =
        plan_tiles_audio(assignments, tiles_audio_scan(), roster, params);
    tiles_audio_apply(audio_plan, audio_group, params.self_uuid);
}
```

- [ ] **Step 6: Add the locale strings**

In `data/locale/en-US.ini`, beside the other `Tiles.*` entries:

```ini
Tiles.AudioGroup="Participant audio group"
Tiles.AudioGroup.Off="Off — no audio sources created"
Tiles.AudioGroup.Desc="Creates one Zoom participant audio source per tile inside this group, so each person gets their own fader and ISO track. Put the group in every scene: audio then stays put while you cut between scenes. Leave this off and nothing is created. The wall itself never carries audio."
```

- [ ] **Step 7: Build and run the full suite**

```bash
cmake --build build-rel --config RelWithDebInfo --target obs-zoom-plugin
ctest --test-dir build-tests --output-on-failure
```

Expected: compiles clean; all tests pass.

- [ ] **Step 8: Update the docs in the same change**

In `CHANGELOG.md`, under the unreleased heading, add:

```markdown
- Tiles can now create and maintain one Zoom participant audio source per tile,
  inside a group you nominate, so each person gets a live fader and an ISO
  track without building the wall twice. Off until you name a group. The wall
  itself stays silent, so cutting between scenes never swaps audio. Tracks 2-6
  carry five ISO stems; past that, participants are on the program track only.
```

In `CLAUDE.md`, in the section describing the Tiles source, add:

```markdown
Per-participant audio (`audio_group`, empty by default) auto-creates one
`zoom_participant_audio_source` per assigned tile into the nominated group.
Ownership is tracked by the `cv_tiles_audio_owner` settings marker holding the
creating Tiles source's uuid — never by name, since the operator can rename
anything. The plugin only ever touches marked sources; a name clash defers
rather than overwriting. Participants who leave the wall are muted, not
deleted. Two Tiles sources showing the same person yield one audio source, so
their voice is never doubled. Decision logic is pure in
`src/zoom-tiles-audio-plan.h` and tested in `tests/tiles-audio-plan-test.cpp`;
every scene-collection mutation lives in `src/zoom-tiles-audio.cpp`.
```

- [ ] **Step 9: Commit**

```bash
git add src/zoom-supersource.cpp data/locale/en-US.ini CLAUDE.md CHANGELOG.md
git commit -m "feat: wire per-participant audio into the Tiles source

Naming a group turns the feature on and is where sources are created;
empty by default, so upgrading changes nothing for anyone who has not
asked. Reconciliation runs outside both locks on the roster path,
because creating sources and adding them to a group emits libobs
signals that run arbitrary handlers."
```

- [ ] **Step 10: Verify on the rig**

This part cannot be unit-tested — it mutates a live scene collection — so it is checked by hand against a running meeting:

1. Launch OBS with a Tiles source on a wall of at least three participants.
2. Confirm **nothing is created** while the group setting is empty. This is the upgrade-safety property.
3. Create a group named `Meeting Audio`, add it to the current scene, and select it in the Tiles source's `Participant audio group`.
4. Confirm one audio source appears per tile inside the group, each named for the participant, each with a fader in the Audio Mixer.
5. Open Advanced Audio Properties. Confirm every created source is on track 1, and that the first five also hold tracks 2, 3, 4, 5, 6 respectively.
6. Let the wall reflow (or exclude a participant). Confirm the departing person's source is **muted, still present**, with its fader intact — not removed.
7. Rename one created source by hand, then force a reflow. Confirm it is still recognised as owned (ownership is the marker, not the name) and is not duplicated.
8. Add a second Tiles source showing an overlapping participant. Confirm **no second audio source** is created for the shared participant.
9. Delete the first Tiles source. Confirm its audio sources remain, then force a reflow on the second Tiles source and confirm it **adopts** them rather than creating duplicates.
10. Save the scene collection, restart OBS, and confirm the group setting and every created source round-trip.

Record the result of each numbered check in the task report.

---

## Notes for the implementer

**Do not add audio output to the Tiles source.** The wall staying silent is what makes cutting between scenes safe; if the wall also carried the mix, any scene holding both it and the per-participant sources would play every voice twice, and during a transition both scenes are briefly active, so every cut would swell.

**The marker, not the name, is ownership.** Every place tempted to match on name is a place where the plugin eventually mutes or adopts a source the operator built. `find_marked_source` checks the marker before the participant id for exactly this reason.

**The overflow is logged, not hidden.** A wall bigger than five people cannot give everyone a stem. Saying so in the log is the difference between a known limit and a silent data loss in post.
