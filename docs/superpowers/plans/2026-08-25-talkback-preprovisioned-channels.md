# Zoom Talkback — Pre-Provisioned Channels (Milestone 6) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make keying instant — the director presses talk and is heard from the first syllable, instead of losing the first words while a channel is created.

**Architecture:** Channels stop being created on the key press. Instead the operator **nominates** talent by name; the engine provisions a standing channel per nominee (and `ceil(n/10)` channels for the all-talent target) and invites them once. Keying then only **selects** an already-live channel. A pure planner owns the 16-channel / 10-user arithmetic and decides who gets covered when the budget runs out.

**Tech Stack:** C++17, Zoom Windows Meeting SDK 7.1.5.43953, CMake + CTest, existing named-pipe line-JSON IPC.

**Spec:** `docs/superpowers/specs/2026-08-24-zoom-talkback-design.md` — §"Channels, identity and the caps" is the authority for this plan.

**Prior plans:** `2026-08-24-zoom-talkback-milestone-1.md` (probe; gate PASSED live), `2026-08-25-talkback-audio-path.md` (audio path), `2026-08-25-talkback-wiring.md` (wiring; **this plan reworks its session path**).

## Why this exists, in one measurement

Live on 2026-08-25 the log showed `no_channel_drops: 1` on every key press: buffers discarded because the channel did not exist yet. The channel is created **on** the press, so the create round-trip plus the invite round-trip — plausibly 0.5–1.5 s — is dead air. The spec anticipated this exactly: *"the director presses talk to Sarah and the first words are clipped waiting for `onChannelUserJoinResponse`."*

## Global Constraints

- **This plan REWORKS Milestone 5's session path.** `session_start`/`session_stop` currently create and destroy a channel. They become select/deselect over channels that already exist. Do not leave both mechanisms live.
- **Caps are hard, from the SDK headers:** max **16 channels**, max **10 users per channel**. Exceeding either is not degradable — it must be planned around and reported.
- **Identity is by NAME, always.** Zoom user IDs are meeting-scoped: a nomination holding a raw ID points at nobody after a rejoin and at the wrong person once IDs are recycled. Nominations store names; membership is re-resolved on roster change.
- **Gates are surfaced, never swallowed.** `IUserInfo::IsSupportTalkback()` is per participant — an unreachable nominee must be reported as unreachable, not silently skipped. A director who briefs someone who never heard a word is worse than a feature that refuses to arm.
- **Budget exhaustion reports who is uncovered.** Never silently drop a nominee.
- Protected invariants, unchanged: `m_phase` atomic and probe-only; `m_chan_mtx` guards channel-id state and **the SDK is never called while holding it**; `tick()` remains the sole caller of the batch-destroy API for the probe's stray queue; the seqlock copy-inside-window; the six forbidden routing APIs in `talkback-tap.cpp`; `evaluate()` releases `m_mtx` before `key_off()`; probe/session mutual exclusion.
- **`CreateChannel` is called only from the engine's command-loop thread.** Provisioning must obey this — it is where the arbiter's single-outstanding-create rule lives.
- Tests are plain executables, no framework, `check()`-style, box-drawing section headers, registered with `add_executable` + `target_include_directories(... PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")` + `add_test`.
- Build: `cmake --build build_x64 --config Release --parallel 8`; test: `cd build_x64 && ctest -C Release --output-on-failure`. **`build_x64` is already configured — never delete or reconfigure it.**
- **Baseline is 62 tests.** Each task states the expected count.
- **OBS may be running.** Never kill it, install binaries, or touch Program Files.
- Comments state the constraint the code cannot show.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/talkback-plan.h` (new) | Pure: nominations + caps → a channel plan, and who is uncovered. No SDK. |
| `engine/src/engine-talkback.{h,cpp}` (modify) | Provisioning, the standing channel table, select/deselect, roster re-resolution. |
| `engine/src/main.cpp` (modify) | Routes `talkback_nominate`; re-resolves on roster change. |
| `src/talkback-controller.{h,cpp}` (modify) | `key_on` selects a target instead of naming a participant to create for. |
| `src/zoom-control-server.cpp` (modify) | `talkback_nominate`; `talkback_key` takes a target. |
| `src/zoom-engine-client.{h,cpp}` (modify) | `talkback_nominate` sender; nomination status. |

---

### Task 1: The planner

The whole caps decision as one pure function, pinned before any SDK code depends on it. This is where a mistake is cheapest to catch and most expensive to discover live — an over-allocation means `CreateChannel` fails mid-show, and a silent drop means someone never hears their cue.

**Files:**
- Create: `src/talkback-plan.h`
- Create: `tests/talkback-plan-test.cpp`
- Modify: `CMakeLists.txt` (register after the `CoreVideoTalkbackCueIsolation` block)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `constexpr uint32_t kTalkbackMaxChannels = 16;`
  - `constexpr uint32_t kTalkbackMaxUsersPerChannel = 10;`
  - `struct TalkbackPlannedChannel { std::vector<std::string> members; bool is_all_talent; };`
  - `struct TalkbackPlan { std::vector<TalkbackPlannedChannel> channels; std::vector<std::string> uncovered_private; bool all_talent_complete; };`
  - `TalkbackPlan talkback_plan(const std::vector<std::string> &nominees);`

- [ ] **Step 1: Write the failing test**

Create `tests/talkback-plan-test.cpp`:

```cpp
// tests/talkback-plan-test.cpp
// How nominated talent maps onto Zoom's 16-channel / 10-user budget.
//
// Two SDK limits drive everything here and neither degrades gracefully:
// CreateChannel refuses past 16 channels, and a channel refuses past 10
// members. So the arithmetic has to be decided up front, not discovered when
// an invite fails in front of an audience.
//
// The rule this pins hardest: when the budget runs out, the people who did
// not get a private channel are NAMED. Silently dropping one means a director
// keys "talk to Sarah", hears their own cue, speaks -- and Sarah never had a
// channel. That failure is invisible from the control room, which is exactly
// why it is a test and not a comment.
#include "talkback-plan.h"

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

static std::vector<std::string> names(int n)
{
    std::vector<std::string> v;
    for (int i = 0; i < n; ++i) v.push_back("Talent " + std::to_string(i + 1));
    return v;
}

static uint32_t private_count(const TalkbackPlan &p)
{
    uint32_t n = 0;
    for (const auto &c : p.channels) if (!c.is_all_talent) ++n;
    return n;
}

static uint32_t all_talent_count(const TalkbackPlan &p)
{
    uint32_t n = 0;
    for (const auto &c : p.channels) if (c.is_all_talent) ++n;
    return n;
}

int main()
{
    // ── Nobody nominated: nothing planned, nothing uncovered ───────────────
    {
        const TalkbackPlan p = talkback_plan({});
        check(p.channels.empty(), "an empty nomination produced channels");
        check(p.uncovered_private.empty(), "an empty nomination reported uncovered people");
        check(p.all_talent_complete, "an empty nomination was not considered complete");
    }

    // ── One nominee: one all-talent channel + one private ──────────────────
    {
        const TalkbackPlan p = talkback_plan({"Sarah Muller"});
        check(all_talent_count(p) == 1, "one nominee did not yield exactly one all-talent channel");
        check(private_count(p) == 1, "one nominee did not yield exactly one private channel");
        check(p.uncovered_private.empty(), "one nominee was reported uncovered");
    }

    // ── The 10-user cap: all-talent fans out at 11 ─────────────────────────
    {
        const TalkbackPlan ten = talkback_plan(names(10));
        check(all_talent_count(ten) == 1, "10 nominees needed more than one all-talent channel");
        const TalkbackPlan eleven = talkback_plan(names(11));
        check(all_talent_count(eleven) == 2,
              "11 nominees did not fan all-talent out to 2 channels -- the 10-user cap is hard");
        // No all-talent channel may exceed the cap.
        for (const auto &c : eleven.channels)
            if (c.is_all_talent)
                check(c.members.size() <= kTalkbackMaxUsersPerChannel,
                      "an all-talent channel exceeded the 10-user cap");
    }

    // ── Every nominee appears in exactly one all-talent channel ────────────
    {
        const TalkbackPlan p = talkback_plan(names(24));
        std::vector<std::string> seen;
        for (const auto &c : p.channels)
            if (c.is_all_talent)
                for (const auto &m : c.members) seen.push_back(m);
        check(seen.size() == 24,
              "the all-talent fan-out did not cover every nominee exactly once");
    }

    // ── A private channel holds exactly one person ─────────────────────────
    {
        const TalkbackPlan p = talkback_plan(names(5));
        for (const auto &c : p.channels)
            if (!c.is_all_talent)
                check(c.members.size() == 1,
                      "a private channel held more than one member -- private means one");
    }

    // ── The 16-channel budget is never exceeded ────────────────────────────
    {
        const TalkbackPlan p = talkback_plan(names(40));
        check(p.channels.size() <= kTalkbackMaxChannels,
              "the plan exceeded the 16-channel cap -- CreateChannel would fail live");
    }

    // ── Budget exhaustion NAMES the uncovered, never drops them silently ───
    // 24 nominees: 3 all-talent (ceil(24/10)) + 13 private = 16. So 11 people
    // get no private channel, and all 11 must be named.
    {
        const TalkbackPlan p = talkback_plan(names(24));
        check(all_talent_count(p) == 3, "24 nominees did not yield 3 all-talent channels");
        check(private_count(p) == 13, "24 nominees did not yield 13 private channels");
        check(p.channels.size() == 16, "the plan did not use the full 16-channel budget");
        check(p.uncovered_private.size() == 11,
              "the 11 nominees with no private channel were not all reported");
        // and the named ones must be real nominees, not invented
        for (const auto &n : p.uncovered_private) {
            bool known = false;
            for (const auto &c : names(24)) if (c == n) known = true;
            check(known, "an uncovered name was not one of the nominees");
        }
    }

    // ── All-talent is never sacrificed to fit privates ─────────────────────
    // Talking to everyone must survive the budget; private coverage is what
    // degrades. A director who cannot reach the whole panel has lost more than
    // one who cannot take somebody aside.
    {
        const TalkbackPlan p = talkback_plan(names(160));
        check(all_talent_count(p) == 16,
              "all-talent was truncated to make room for private channels");
        check(private_count(p) == 0, "privates were allocated with no budget left");
        check(p.uncovered_private.size() == 160,
              "every nominee should be reported uncovered when no private budget remains");
        check(p.all_talent_complete, "all-talent fit the budget but was reported incomplete");
    }

    // ── Beyond even all-talent's reach, say so ─────────────────────────────
    {
        const TalkbackPlan p = talkback_plan(names(200));   // needs 20 > 16
        check(!p.all_talent_complete,
              "a panel too large for even the all-talent fan-out was reported complete");
        check(p.channels.size() <= kTalkbackMaxChannels, "the cap was exceeded anyway");
    }

    // ── Duplicate names collapse, they do not consume two channels ─────────
    {
        const TalkbackPlan p = talkback_plan({"Sarah", "Sarah", "Luis"});
        check(private_count(p) == 2, "a duplicate nominee consumed a second private channel");
    }

    if (failures == 0)
        std::cout << "talkback-plan: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register it and confirm it fails**

In `CMakeLists.txt`, after the `add_test(NAME CoreVideoTalkbackCueIsolation ...)` block:

```cmake
    # How nominated talent maps onto Zoom's 16-channel / 10-user budget. Neither
    # cap degrades gracefully, and a silently-dropped nominee is invisible from
    # the control room. See src/talkback-plan.h.
    add_executable(CoreVideoTalkbackPlanTest
        tests/talkback-plan-test.cpp
    )
    target_include_directories(CoreVideoTalkbackPlanTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTalkbackPlan
             COMMAND CoreVideoTalkbackPlanTest)
```

Run: `cmake --build build_x64 --config Release --target CoreVideoTalkbackPlanTest --parallel 8`
Expected: FAIL — `Cannot open include file: 'talkback-plan.h'`.

- [ ] **Step 3: Implement**

Create `src/talkback-plan.h`:

```cpp
#pragma once
//
// talkback-plan.h — how nominated talent maps onto Zoom's channel budget.
//
// Two SDK limits drive this, and neither degrades gracefully: CreateChannel
// refuses past 16 channels, and a channel refuses past 10 members. So the
// arithmetic is decided up front rather than discovered when an invite fails
// in front of an audience.
//
// WHY PRE-PROVISION AT ALL. Creating a channel on the key press costs a
// create round-trip plus an invite round-trip before any audio can flow --
// measured live on 2026-08-25 as buffers discarded on every press
// (no_channel_drops). The director speaks and the first words are gone. So
// channels are created at NOMINATION time and keying only selects one.
//
// WHAT DEGRADES WHEN THE BUDGET RUNS OUT. All-talent is allocated first and
// never sacrificed: a director who cannot reach the whole panel has lost more
// than one who cannot take somebody aside. Private channels take what is
// left, and everyone who does not get one is NAMED in uncovered_private --
// never silently dropped, because that failure is invisible from the control
// room.
//
// Free of Qt / OBS / Zoom SDK dependencies so the whole decision can be
// pinned by a test with no engine and no meeting.
//
#include <cstdint>
#include <string>
#include <vector>

// From the SDK headers: IMeetingTalkbackController::CreateChannel documents
// "Supports a maximum of 16 channels", and AddUserToInvite "A channel can
// have at most 10 users."
constexpr uint32_t kTalkbackMaxChannels        = 16;
constexpr uint32_t kTalkbackMaxUsersPerChannel = 10;

struct TalkbackPlannedChannel {
    // Nominee display names, resolved to meeting-scoped ids only at invite
    // time -- see the spec on why ids are never stored.
    std::vector<std::string> members;
    // True for a slice of the all-talent target, false for a one-person
    // private channel.
    bool is_all_talent = false;
};

struct TalkbackPlan {
    std::vector<TalkbackPlannedChannel> channels;
    // Nominees with no private channel of their own. They are still reachable
    // via all-talent; they just cannot be taken aside.
    std::vector<std::string> uncovered_private;
    // False when the panel is so large that even the all-talent fan-out does
    // not fit in 16 channels -- at which point some people cannot be reached
    // at all, and the operator has to know.
    bool all_talent_complete = true;
};

inline TalkbackPlan talkback_plan(const std::vector<std::string> &nominees)
{
    TalkbackPlan plan;

    // Collapse duplicates, preserving nomination order. A name repeated in the
    // list is one person; letting it consume two private channels would spend
    // the budget on nobody.
    std::vector<std::string> unique;
    for (const auto &n : nominees) {
        bool seen = false;
        for (const auto &u : unique) if (u == n) { seen = true; break; }
        if (!seen) unique.push_back(n);
    }
    if (unique.empty()) return plan;

    // ── All-talent first, and never truncated to make room for privates ───
    const std::size_t need =
        (unique.size() + kTalkbackMaxUsersPerChannel - 1) / kTalkbackMaxUsersPerChannel;
    const std::size_t all_talent_channels =
        need > kTalkbackMaxChannels ? kTalkbackMaxChannels : need;
    plan.all_talent_complete = (all_talent_channels == need);

    for (std::size_t i = 0; i < all_talent_channels; ++i) {
        TalkbackPlannedChannel c;
        c.is_all_talent = true;
        const std::size_t first = i * kTalkbackMaxUsersPerChannel;
        const std::size_t last  =
            std::min(first + kTalkbackMaxUsersPerChannel, unique.size());
        for (std::size_t m = first; m < last; ++m) c.members.push_back(unique[m]);
        plan.channels.push_back(std::move(c));
    }

    // ── Private channels take whatever is left ────────────────────────────
    std::size_t remaining = kTalkbackMaxChannels - plan.channels.size();
    for (std::size_t i = 0; i < unique.size(); ++i) {
        if (remaining == 0) {
            // Named, not dropped. Someone who cannot be taken aside must be
            // visible to the operator BEFORE they try.
            plan.uncovered_private.push_back(unique[i]);
            continue;
        }
        TalkbackPlannedChannel c;
        c.is_all_talent = false;
        c.members.push_back(unique[i]);
        plan.channels.push_back(std::move(c));
        --remaining;
    }

    return plan;
}
```

Add `#include <algorithm>` for `std::min`.

- [ ] **Step 4: Confirm it passes**

```sh
cmake --build build_x64 --config Release --parallel 8
cd build_x64 && ctest -C Release --output-on-failure
```
Expected: **63/63** green.

- [ ] **Step 5: Commit**

```sh
git add src/talkback-plan.h tests/talkback-plan-test.cpp CMakeLists.txt
git commit -m "feat(talkback): the channel-budget planner"
```

---

### Task 2: Provision channels at nomination

**Files:**
- Modify: `engine/src/engine-talkback.{h,cpp}`, `src/engine-ipc.h`, `src/engine-command.h`, `tests/engine-command-test.cpp`, `engine/src/main.cpp`

**Interfaces:**
- Consumes: `talkback_plan()` (Task 1), the existing arbiter.
- Produces: `bool EngineTalkback::nominate(ZOOMSDK::IMeetingService*, const std::vector<std::string>&)`, `IPC_CMD_TALKBACK_NOMINATE` / `IpcCommand::TalkbackNominate`.

- [ ] **Step 1: Route the command, TDD**

Add routing cases to `tests/engine-command-test.cpp` mirroring the existing `talkback_*` cases (exact match; a longer command starting with `talkback_nominate` must be `Unknown`). Add `IPC_CMD_TALKBACK_NOMINATE "talkback_nominate"`, the enum value, and the `ipc_command_of` line. Run, fail, implement, pass.

- [ ] **Step 2: Provisioning state**

Add to `EngineTalkback`: a table of provisioned channels, each holding its SDK channel id, its planned members, and whether it is the all-talent target. Guard it with `m_chan_mtx` like every other channel-id state.

Provisioning is **sequential**: the arbiter allows exactly one outstanding `CreateChannel`. So `nominate()` computes the plan, stores it as a queue of channels still to create, and issues the first `CreateChannel`. Each `onCreateChannelResponse` for the nomination owner records the id, invites that channel's members, and issues the next create. Report progress per channel.

Add a `TalkbackChannelOwner::Nomination` value to `src/talkback-channel-owner.h` so the arbiter can route these responses — and update that header's writer/threading inventory, which it maintains deliberately.

- [ ] **Step 3: Invite by name, and surface the per-user gate**

For each planned member, resolve the name to a live user id at invite time and read `IUserInfo::IsSupportTalkback()`. Report an unreachable nominee explicitly. A nominee not currently in the meeting is reported and skipped — not an error; roster re-resolution (Task 4) picks them up when they arrive.

- [ ] **Step 4: Build and test**

Expected: **63/63** green (routing cases go in an existing test file).

- [ ] **Step 5: Commit**

```sh
git commit -m "feat(talkback): provision channels at nomination time"
```

---

### Task 3: Keying selects, never creates

This is the change that removes the clipping, and the one that reworks Milestone 5.

**Files:**
- Modify: `engine/src/engine-talkback.{h,cpp}`, `engine/src/main.cpp`

- [ ] **Step 1: Replace create-on-key with select-on-key**

`session_start(target)` looks the target up in the provisioned table and, if it is live, points `drain_audio` at its channel id(s) — no `CreateChannel`, no invite, no round-trip. `session_stop()` stops sending; **the channel stays alive** for the next press. Destruction happens on `talkback_denominate`/`Leave`/`quit`, not on key release.

An unprovisioned target must be refused with a specific reason, not silently create one — creating on demand is the behaviour this plan exists to remove.

- [ ] **Step 2: Fan out to every channel of a target**

`drain_audio` currently sends to one channel. An all-talent target with more than 10 people owns several, and the same PCM goes to all of them in one drain pass. Copy the id list out under `m_chan_mtx`, release, then send — the SDK is never called while holding that lock.

- [ ] **Step 3: Build and test**

Expected: **63/63** green.

- [ ] **Step 4: Commit**

```sh
git commit -m "feat(talkback): keying selects a provisioned channel instead of creating one"
```

---

### Task 4: Roster re-resolution

**Files:**
- Modify: `engine/src/engine-talkback.{h,cpp}`, `engine/src/main.cpp`

- [ ] **Step 1: Re-resolve on roster change**

On the engine's existing roster-change path, re-resolve every nominated name. Invite anyone newly present into the channels their name is planned for; report anyone who left. A rejoin therefore rebuilds membership automatically, because nominations hold names and never ids.

Guard against re-inviting somebody already in a channel — `TALKBACK_ERROR_ALREADY_EXIST` exists for this and must be treated as success, not failure.

- [ ] **Step 2: Build and test**

Expected: **63/63** green.

- [ ] **Step 3: Commit**

```sh
git commit -m "feat(talkback): re-resolve nominations when the roster changes"
```

---

### Task 5: The operator surface

**Files:**
- Modify: `src/zoom-engine-client.{h,cpp}`, `src/talkback-controller.{h,cpp}`, `src/zoom-control-server.cpp`, `src/zoom-control-parse.h`

- [ ] **Step 1: `talkback_nominate`**

Control API takes a list of names, forwards to the engine, and reports back the plan: how many channels, who has a private channel, who is uncovered, and who is unreachable. That report is the point — the operator must learn the budget outcome at nomination time, not when a key press fails mid-show.

- [ ] **Step 2: `talkback_key` takes a target**

Either `"all"` or a nominee's name. Refuse an unprovisioned target with a specific message. Keep `state:"off"` short-circuiting before any validation — closing a key must never be refused on a technicality.

- [ ] **Step 3: Update the known-commands list and its size guard**

It is currently `std::array<const char *, 24>` in `src/zoom-control-parse.h`, asserted in `tests/zoom-control-parse-test.cpp`. Adding one command means 25 in both.

- [ ] **Step 4: Build and test**

Expected: **63/63** green including the size guard.

- [ ] **Step 5: Commit**

```sh
git commit -m "feat(talkback): nominate talent and key by target"
```

---

### Task 6: Live verification — THE GATE

- [ ] **Step 1: Install the matched pair** — both binaries, SHA256-verified, OBS closed. Requires the operator's UAC.

- [ ] **Step 2: Meeting setup — read this, it is what blocked 2026-08-25**

The SDK client and the operator's Zoom **must be different Zoom accounts**. On 2026-08-25 they shared one account and displaced each other into separate PMI sessions; every invite then targeted a user id that existed only in the engine's own dead session, returning `SDKERR_WRONG_USAGE`. A control run of the Milestone 1 probe failed identically, which is what proved the wiring was not at fault.

Required: the engine signed in as the entitled account and **host or co-host**; a second device joined **as a guest** (not signed into that account) as the invited talent; and a third guest as the **uninvited control**.

- [ ] **Step 3: Nominate, then key**

Nominate the guests. Confirm the reported plan matches expectation. Then key and speak a count-in.

- [ ] **Step 4: Confirm — the measurements this milestone exists for**

- **Is the first syllable there?** Compare against the 2026-08-25 baseline, where the start of every press was lost. This is the whole point of the milestone.
- The invited guest heard it; the **uninvited control heard nothing** — still the untested promise.
- Program output and any ISO recording contain no talkback audio.
- Key repeatedly: the channel must persist and stay instant, not be recreated.

- [ ] **Step 5: Record and decide** — write the log, both human confirmations, and the clipping comparison into `docs/superpowers/notes/`.

---

## Self-Review

**Spec coverage.** §"A target maps to one or more channels" → Task 1's fan-out and Task 3's multi-channel send. §"Private channels are pre-provisioned" → Tasks 2 and 3. §"Identity is by name" → Tasks 2 and 4. §"Gates are surfaced, never swallowed" → Task 2 Step 3 (`IsSupportTalkback`) and Task 5 Step 1 (the plan report). The budget-exhaustion rule is pinned by Task 1's tests.

**Deliberately out of scope:** Companion, OBS hotkey, and the dock — including the dock's program-track warning UI (the `LOG_WARNING` already ships). Deadline-anchored tone pacing. The parked `leave`-mid-probe wedge.

**Placeholder scan:** none. Tasks 2–5 describe behaviour and constraints rather than transcribing full implementations, because they modify substantial existing files whose current shape the implementer must read — Task 1, the new pure file, carries complete code.

**Type consistency:** `TalkbackPlan`/`TalkbackPlannedChannel`/`talkback_plan()` are defined in Task 1 and consumed in Task 2. `kTalkbackMaxChannels`/`kTalkbackMaxUsersPerChannel` are defined once in Task 1. `TalkbackChannelOwner::Nomination` is added in Task 2 and used by the arbiter there.

**Known risk, stated rather than hidden:** Task 3 reworks a path that took three fix rounds to get right in Milestone 5. The arbiter, the mutual exclusion, and the `m_chan_mtx` discipline all have to survive it. A reviewer should check those specifically rather than only the new behaviour.
