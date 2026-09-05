# Production Control Surface: Push-Control of the Meeting

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the production side push control INTO the meeting — mute a participant, ask them to unmute, rename them, mute everyone, lower all hands — through one new `meeting_control` engine command exposed on the control API, the dock's participant list, and Companion actions.

**Architecture:** One new fire-and-acknowledge IPC command (`meeting_control`) carries an action plus a participant *display name*; the engine resolves the name to a meeting-scoped id at execution time, gates the action through a pure decision header (`src/meeting-control-plan.h`, mirroring `src/talkback-plan.h`), calls the SDK controller, and reports the `SDKError` verdict asynchronously as a `"cmd":"meeting_control"` E2P line. The plugin logs that line verbatim and stashes it for polling; the control API, dock context menu, and Companion actions are thin senders over the same path.

**Tech Stack:** C++17, Zoom Windows Meeting SDK 7.1.5.43953, CMake + CTest, named-pipe line-JSON IPC, Qt6, TypeScript Companion module.

**Spec:** This document doubles as the spec.

Requirements:

1. Actions: `mute` (one participant), `ask_unmute` (one participant), `mute_all`, `rename` (one participant, new name), `lower_all_hands`. All addressed by display name, resolved to a user id only at the moment the SDK call is made.
2. Every action's outcome is reported asynchronously — the control API/dock/Companion ack only confirms the trigger was accepted (the repo's `talkback_probe` shape). The engine's result line carries the raw `SDKError` code and a named reason; a refusal is never silent.
3. A request the engine can already prove will fail (not host/co-host, name not in roster, two participants sharing the name) is refused with a named reason *before* any SDK call, by pure logic pinned by a host test.
4. **Spotlight and pin are OUT OF SCOPE — the vendored SDK headers cannot express them.** `third_party/zoom-sdk/h/meeting_video_interface.h` is a slim vendored stub: `IMeetingVideoController` declares ONLY `SetEvent(IMeetingVideoCtrlEvent*)` (line 40). There is no `SpotlightVideo`, `UnSpotlightVideo`, `UnSpotlightAllVideos`, `PinVideo`, or any `CanSpotlight`/`CanPin` check anywhere under `third_party/zoom-sdk/h` (verified by grep; the only spotlight symbols are the *receive-side* callback `onSpotlightedUserListChangeNotification` and the self-facing settings toggles `EnableSpotlightSelf`/`IsSpotlightSelfEnabled` in `setting_service_interface.h:1197-1203`). The honest alternative shipped by this plan: nothing pretends to spotlight. The existing receive-side spotlight-slot outputs (`subscribe` with `"mode":"spotlight"`) remain the production lever for following what the *host inside Zoom* spotlights. Pushing spotlight/pin becomes possible only when a fuller `meeting_video_interface.h` is vendored — at which point it slots into this same `meeting_control` action namespace.
5. **`ask_unmute` is a request, not a force.** The vendored `meeting_audio_interface.h:184-189` documents `UnMuteAudio(unsigned int userid)` only as "Unmutes the assigned user" — the doc comment does not state consent semantics. On the live SDK, unmuting *another* user does not force their mic hot: the target receives the host's start-audio request (`IMeetingAudioCtrlEvent::onHostRequestStartAudio`, same header lines 125-128, with the `IRequestStartAudioHandler` Accept/Ignore/Cancel flow at lines 50-75) and must consent. So the action is *named* `ask_unmute`, its success means "request delivered", and the actual unmute is observed the way everything else is: the roster's `is_muted` flag flips when `onUserAudioStatusChange` fires. `IMeetingParticipantsController::AskAllToUnmute()` (`meeting_participants_ctrl_interface.h:674`) exists for the all-hands variant but is not wired in this plan; `ask_unmute` per person covers the production need.

SDK ground truth (exact signatures from `third_party/zoom-sdk/h`):

- `meeting_audio_interface.h:181` — `virtual SDKError MuteAudio(unsigned int userid, bool allowUnmuteBySelf = true) = 0;` — doc: "ZERO(0) indicates to mute all the participants." Mute-all is `MuteAudio(0, allowUnmuteBySelf)`; there is no separate mute-all call.
- `meeting_audio_interface.h:189` — `virtual SDKError UnMuteAudio(unsigned int userid) = 0;`
- `meeting_audio_interface.h:218` — `virtual bool IsMuteOnEntryEnabled() = 0;` (not wired here; noted as the adjacent knob).
- `meeting_participants_ctrl_interface.h:580` — `virtual SDKError ChangeUserName(const unsigned int userid, const zchar_t* userName, bool bSaveUserName) = 0;` — doc: "Only the host or co-host can change the others' name."
- `meeting_participants_ctrl_interface.h:570` — `virtual SDKError LowerAllHands(bool forWebinarAttendees) = 0;`
- `meeting_participants_ctrl_interface.h:95,164` — `IUserInfo::IsHost()`, `IUserInfo::GetUserRole()` returning `UserRole` (`USERROLE_HOST`, `USERROLE_COHOST`, ... lines 18-32); `GetMySelfUser()` at line 526. `MakeHost(unsigned int)` (603) and `AssignCoHost(unsigned int)` (619) exist but are deliberately not exposed — handing off host from a control surface is a show-ending misclick.

## Global Constraints

- Build: `cmake --build build --config Release --parallel 8`. Test: `cd build && ctest -C Release --output-on-failure` — must be N/N green (the suite gains one executable, `CoreVideoMeetingControlPlanTest`).
- Tests are plain executables, no framework, `check()`-style, one file per invariant cluster, registered in `CMakeLists.txt` (mirror the `CoreVideoTalkbackPlan` block at `CMakeLists.txt:1104-1111`).
- Commands are routed by EXACT match on the declared `cmd` field (`src/engine-command.h`), never substring, and every new command is pinned in `tests/engine-command-test.cpp` with its verbatim wire line (that file doubles as the wire-format record).
- Participants are addressed by display name everywhere; names resolve to meeting-scoped ids only at use time, engine-side. Ids are never stored on a button, in a config, or across a rejoin.
- Companion: `companion/manifest.json` `runtime.apiVersion` must stay schema-valid, the build must emit ESM, and the version must be bumped on every rebuild you intend to install (Companion refuses to overwrite a version already on disk). Dropdown choices are baked at `buildActions()` time and are already rebuilt on roster change (`index.ts:203`).
- Comments state the constraint the code cannot show — when a decision is motivated by a doc-comment or a live behavior, cite it.
- Never run a second OBS instance while testing (pipe/SDK singleton collision, crash loop). Send `{"cmd":"leave"}` before closing OBS.

---

### Task 1: Route the `meeting_control` command

The engine identifies commands by exact match on the declared `cmd` field — a substring dispatch once routed every `unsubscribe` into the `subscribe` branch. A new command must be added to the token list, the enum, and the routing function together, and pinned by the existing routing test (this mirrors Task 1 of `docs/superpowers/plans/2026-08-24-zoom-talkback-milestone-1.md` exactly).

**Files:**
- Modify: `src/engine-ipc.h:26` (add token after `IPC_CMD_TALKBACK_NOMINATE`)
- Modify: `src/engine-command.h:32-50` (enum), `src/engine-command.h:86-106` (routing)
- Test: `tests/engine-command-test.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `IPC_CMD_MEETING_CONTROL` (string literal `"meeting_control"`), `IpcCommand::MeetingControl`. Task 3 branches on the enum; Task 4 emits the literal.

- [ ] **Step 1: Write the failing test.** Append inside `main()` in `tests/engine-command-test.cpp`, before the final `if (g_failures > 0)` block:

```cpp
    // ── Meeting control (production push-control) routes exactly ───────────
    routes(R"({"cmd":"meeting_control","action":"mute","participant":"Sarah Muller"})",
           IpcCommand::MeetingControl,
           "meeting_control did not route to IpcCommand::MeetingControl");
    // A payload containing the token must not route (the substring disease).
    routes(R"({"cmd":"join","display_name":"meeting_control"})",
           IpcCommand::Join,
           "a payload containing 'meeting_control' hijacked the join branch");
    routes(R"({"cmd":"meeting_control_extra"})", IpcCommand::Unknown,
           "a longer command starting with meeting_control matched it");
```

- [ ] **Step 2: Run to verify it fails.**

```sh
cmake --build build --config Release --target CoreVideoEngineCommandTest --parallel 8
```

Expected: compile FAILURE, `'MeetingControl' is not a member of 'IpcCommand'`.

- [ ] **Step 3: Minimal implementation.** In `src/engine-ipc.h` after `#define IPC_CMD_TALKBACK_NOMINATE "talkback_nominate"`:

```c
#define IPC_CMD_MEETING_CONTROL "meeting_control"
```

In `src/engine-command.h`, add to the enum after `TalkbackNominate,`:

```cpp
    MeetingControl,
```

and in `ipc_command_of()`, before `return IpcCommand::Unknown;`:

```cpp
    if (cmd == IPC_CMD_MEETING_CONTROL) return IpcCommand::MeetingControl;
```

- [ ] **Step 4: Run to verify it passes.**

```sh
cmake --build build --config Release --target CoreVideoEngineCommandTest --parallel 8
cd build && ctest -C Release -R CoreVideoEngineCommand --output-on-failure
```

Expected: PASS, `engine-command: all tests passed`.

- [ ] **Step 5: Commit.**

```sh
git add src/engine-ipc.h src/engine-command.h tests/engine-command-test.cpp
git commit -m "feat(prod-control): route the meeting_control command"
```

---

### Task 2: The decision header — `src/meeting-control-plan.h`

Every Major this repo's talkback feature shipped lived in wiring no test could reach; the standing rule is that pure decision logic goes in a Qt/OBS/SDK-free header pinned by a host test (`src/talkback-plan.h` is the model). The decisions here: which string names which action, whether a requested action + our role + the resolved target is allowed or refused-with-a-named-reason, how a display name resolves against a roster that can contain duplicates, and the exact shape of the engine's result report. A non-privileged refusal must be a NAMED reason (`not_host_or_cohost`), never silent and never a bare SDK integer.

**Files:**
- Create: `src/meeting-control-plan.h`
- Create: `tests/meeting-control-plan-test.cpp`
- Modify: `CMakeLists.txt` (new test block after the `CoreVideoTalkbackPlan` block at `CMakeLists.txt:1104-1111`)

**Interfaces:**
- Consumes: nothing (header is dependency-free by design).
- Produces (all `inline`, all consumed by Tasks 3, 5, 6):

```cpp
enum class MeetingControlAction { Unknown, Mute, AskUnmute, MuteAll, Rename, LowerAllHands };
MeetingControlAction meeting_control_action_of(const std::string &s);
struct MeetingControlTarget { uint32_t user_id; bool found; bool ambiguous; bool is_self; };
MeetingControlTarget meeting_control_resolve(
    const std::vector<std::pair<std::string, uint32_t>> &roster,
    const std::string &name, uint32_t self_id);
struct MeetingControlVerdict { bool allowed; std::string reason; };
MeetingControlVerdict meeting_control_decide(MeetingControlAction action,
    bool self_is_host_or_cohost, const MeetingControlTarget &target);
std::string meeting_control_validate(MeetingControlAction action,
    const std::string &participant, const std::string &new_name);
std::string meeting_control_report(const std::string &action, bool ok,
    int sdk_code, const std::string &reason);
```

- [ ] **Step 1: Write the failing test.** Create `tests/meeting-control-plan-test.cpp`:

```cpp
// Pins the production-control decision logic: action naming, name→id
// resolution against a duplicate-bearing roster, the privilege gate, and the
// report line shape. Free of Qt/OBS/SDK so it runs with no engine and no
// meeting — the layer this repo's tests can never reach is the one these
// decisions must therefore not live in.
#include "meeting-control-plan.h"
#include <iostream>

static int g_failures = 0;
static void check(bool ok, const char *what)
{
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}

int main()
{
    // ── Action naming fails closed ──────────────────────────────────────────
    check(meeting_control_action_of("mute") == MeetingControlAction::Mute, "mute names Mute");
    check(meeting_control_action_of("ask_unmute") == MeetingControlAction::AskUnmute, "ask_unmute names AskUnmute");
    check(meeting_control_action_of("mute_all") == MeetingControlAction::MuteAll, "mute_all names MuteAll");
    check(meeting_control_action_of("rename") == MeetingControlAction::Rename, "rename names Rename");
    check(meeting_control_action_of("lower_all_hands") == MeetingControlAction::LowerAllHands, "lower_all_hands names LowerAllHands");
    check(meeting_control_action_of("unmute") == MeetingControlAction::Unknown,
          "'unmute' must NOT map to anything — the action is a request and is named ask_unmute");
    check(meeting_control_action_of("") == MeetingControlAction::Unknown, "empty action is Unknown");

    // ── Resolution: by exact name, duplicates are AMBIGUOUS not first-wins ──
    const std::vector<std::pair<std::string, uint32_t>> roster = {
        {"Sarah Muller", 101}, {"Luis Ortiz", 102}, {"Guest", 103}, {"Guest", 104}, {"OBS", 105},
    };
    auto t = meeting_control_resolve(roster, "Sarah Muller", 105);
    check(t.found && !t.ambiguous && t.user_id == 101 && !t.is_self, "unique name resolves");
    t = meeting_control_resolve(roster, "Guest", 105);
    check(t.found && t.ambiguous, "duplicate display name is ambiguous, never first-match");
    t = meeting_control_resolve(roster, "Nobody Here", 105);
    check(!t.found, "absent name is not found");
    t = meeting_control_resolve(roster, "sarah muller", 105);
    check(!t.found, "names match EXACTLY — case variants are different people (talkback-plan.h rule)");
    t = meeting_control_resolve(roster, "OBS", 105);
    check(t.found && t.is_self, "self is flagged so the privilege gate can wave self-actions through");

    // ── The privilege gate: refusals are NAMED ─────────────────────────────
    MeetingControlTarget hit{101, true, false, false};
    check(meeting_control_decide(MeetingControlAction::Mute, true, hit).allowed, "host mutes");
    auto v = meeting_control_decide(MeetingControlAction::Mute, false, hit);
    check(!v.allowed && v.reason == "not_host_or_cohost", "non-privileged mute is refused BY NAME");
    v = meeting_control_decide(MeetingControlAction::MuteAll, false, MeetingControlTarget{});
    check(!v.allowed && v.reason == "not_host_or_cohost", "mute_all needs host/co-host");
    v = meeting_control_decide(MeetingControlAction::LowerAllHands, false, MeetingControlTarget{});
    check(!v.allowed && v.reason == "not_host_or_cohost", "lower_all_hands needs host/co-host");
    v = meeting_control_decide(MeetingControlAction::Rename, false, hit);
    check(!v.allowed && v.reason == "not_host_or_cohost",
          "rename of another needs host/co-host (ChangeUserName doc comment)");
    MeetingControlTarget self_hit{105, true, false, true};
    check(meeting_control_decide(MeetingControlAction::Rename, false, self_hit).allowed,
          "renaming SELF needs no privilege — the doc restricts only others' names");
    check(meeting_control_decide(MeetingControlAction::Mute, false, self_hit).allowed,
          "muting self needs no privilege");
    v = meeting_control_decide(MeetingControlAction::Mute, true, MeetingControlTarget{0, false, false, false});
    check(!v.allowed && v.reason == "participant_not_found", "absent target refused by name");
    v = meeting_control_decide(MeetingControlAction::Mute, true, MeetingControlTarget{0, true, true, false});
    check(!v.allowed && v.reason == "ambiguous_name",
          "two people sharing the name: refuse — muting the wrong face is worse than refusing");
    v = meeting_control_decide(MeetingControlAction::Unknown, true, hit);
    check(!v.allowed && v.reason == "unknown_action", "unknown action fails closed even for the host");

    // ── Control-API request validation (Task 5 consumes this) ──────────────
    check(meeting_control_validate(MeetingControlAction::Mute, "", "") == "participant_required",
          "mute without a participant is invalid");
    check(meeting_control_validate(MeetingControlAction::Rename, "Sarah Muller", "") == "new_name_required",
          "rename without a new name is invalid");
    check(meeting_control_validate(MeetingControlAction::MuteAll, "", "").empty(),
          "mute_all needs no participant");
    check(meeting_control_validate(MeetingControlAction::Unknown, "x", "y") == "invalid_action",
          "unknown action is invalid at the API edge too");
    check(meeting_control_validate(MeetingControlAction::AskUnmute, "Sarah Muller", "").empty(),
          "well-formed ask_unmute validates clean");

    // ── The report line the engine emits (Task 3 consumes this) ────────────
    check(meeting_control_report("mute", true, 0, "") ==
              R"({"cmd":"meeting_control","action":"mute","ok":true,"code":0})",
          "success report carries the SDK code and no reason field");
    check(meeting_control_report("rename", false, 12, "sdk_error") ==
              R"({"cmd":"meeting_control","action":"rename","ok":false,"code":12,"reason":"sdk_error"})",
          "failure report names the reason and keeps the raw SDKError code");

    if (g_failures > 0) {
        std::cerr << "meeting-control-plan: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "meeting-control-plan: all tests passed\n";
    return 0;
}
```

Register it in `CMakeLists.txt` directly after the `CoreVideoTalkbackPlan` block (line 1111):

```cmake
    # Production push-control decisions: who may do what to whom, and how the
    # verdict is reported. Pure logic — see src/meeting-control-plan.h.
    add_executable(CoreVideoMeetingControlPlanTest
        tests/meeting-control-plan-test.cpp
    )
    target_include_directories(CoreVideoMeetingControlPlanTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoMeetingControlPlan
             COMMAND CoreVideoMeetingControlPlanTest)
```

- [ ] **Step 2: Run to verify it fails.**

```sh
cmake --build build --config Release --target CoreVideoMeetingControlPlanTest --parallel 8
```

Expected: compile FAILURE, `Cannot open include file: 'meeting-control-plan.h'`.

- [ ] **Step 3: Minimal implementation.** Create `src/meeting-control-plan.h`:

```cpp
#pragma once
//
// meeting-control-plan.h — the production-control decisions: which string
// names which push action, how a display name resolves against a roster that
// can contain duplicates, whether our role permits the action, and the exact
// result line the engine reports.
//
// Free of Qt / OBS / Zoom SDK dependencies so every decision can be pinned by
// a test with no engine and no meeting (src/talkback-plan.h's reason for
// existing; both of that feature's shipped Majors lived in wiring no test
// could reach).
//
// Names are compared EXACTLY, and a duplicate is refused as ambiguous rather
// than first-match resolved: muting the wrong person on air is strictly worse
// than refusing and making the operator disambiguate in Zoom itself.
//
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

enum class MeetingControlAction { Unknown, Mute, AskUnmute, MuteAll, Rename, LowerAllHands };

// Fails closed: anything unrecognised — including "unmute", which does not
// exist because UnMuteAudio on another user is a consent REQUEST, not a
// force (see the plan's SDK-ground-truth notes) — is Unknown.
inline MeetingControlAction meeting_control_action_of(const std::string &s)
{
    if (s == "mute")            return MeetingControlAction::Mute;
    if (s == "ask_unmute")      return MeetingControlAction::AskUnmute;
    if (s == "mute_all")        return MeetingControlAction::MuteAll;
    if (s == "rename")          return MeetingControlAction::Rename;
    if (s == "lower_all_hands") return MeetingControlAction::LowerAllHands;
    return MeetingControlAction::Unknown;
}

struct MeetingControlTarget {
    uint32_t user_id  = 0;
    bool found        = false;
    bool ambiguous    = false; // two or more roster entries share the name
    bool is_self      = false;
};

// Resolve at USE time only — ids are meeting-scoped and recycled, so nothing
// upstream of this call ever stores one. Exact comparison, deliberately: two
// people can differ only in case and both be real (talkback-plan.h's rule).
inline MeetingControlTarget meeting_control_resolve(
    const std::vector<std::pair<std::string, uint32_t>> &roster,
    const std::string &name, uint32_t self_id)
{
    MeetingControlTarget t;
    for (const auto &entry : roster) {
        if (entry.first != name) continue;
        if (t.found) { t.ambiguous = true; return t; }
        t.found = true;
        t.user_id = entry.second;
    }
    if (t.found) t.is_self = (t.user_id == self_id);
    return t;
}

struct MeetingControlVerdict {
    bool allowed = false;
    std::string reason; // empty iff allowed
};

// The pre-SDK gate. This can only ever prove "known bad" — the SDK's own
// verdict is still final and still reported (Task 3) — but a refusal here is
// NAMED, where the SDK would return a bare integer mid-show. Self-targeted
// mute/ask_unmute/rename skip the privilege check: ChangeUserName's doc
// restricts only "the others' name", and muting/unmuting yourself is any
// participant's right.
inline MeetingControlVerdict meeting_control_decide(MeetingControlAction action,
    bool self_is_host_or_cohost, const MeetingControlTarget &target)
{
    if (action == MeetingControlAction::Unknown)
        return {false, "unknown_action"};
    const bool needs_target = action == MeetingControlAction::Mute ||
                              action == MeetingControlAction::AskUnmute ||
                              action == MeetingControlAction::Rename;
    if (needs_target) {
        if (target.ambiguous) return {false, "ambiguous_name"};
        if (!target.found)    return {false, "participant_not_found"};
        if (target.is_self)   return {true, {}};
    }
    if (!self_is_host_or_cohost) return {false, "not_host_or_cohost"};
    return {true, {}};
}

// Control-API edge validation (Task 5): shape errors are caught before the
// command crosses the pipe, so the socket ack can refuse them synchronously
// with a machine-readable token instead of acking a command the engine will
// drop on the floor.
inline std::string meeting_control_validate(MeetingControlAction action,
    const std::string &participant, const std::string &new_name)
{
    if (action == MeetingControlAction::Unknown) return "invalid_action";
    const bool needs_target = action == MeetingControlAction::Mute ||
                              action == MeetingControlAction::AskUnmute ||
                              action == MeetingControlAction::Rename;
    if (needs_target && participant.empty()) return "participant_required";
    if (action == MeetingControlAction::Rename && new_name.empty())
        return "new_name_required";
    return {};
}

// The one report shape (Task 3 emits it, the plugin's handle_event logs and
// stashes it verbatim). `code` is the raw SDKError integer (-1 when no SDK
// call was made); `reason` is present only on failure so a success line stays
// grep-clean. `action` is caller-supplied and never participant-controlled,
// so no escaping is needed — keep it that way.
inline std::string meeting_control_report(const std::string &action, bool ok,
    int sdk_code, const std::string &reason)
{
    std::string line = R"({"cmd":"meeting_control","action":")" + action +
        R"(","ok":)" + (ok ? "true" : "false") +
        R"(,"code":)" + std::to_string(sdk_code);
    if (!ok) line += R"(,"reason":")" + reason + "\"";
    line += "}";
    return line;
}
```

- [ ] **Step 4: Run to verify it passes.**

```sh
cmake --build build --config Release --target CoreVideoMeetingControlPlanTest --parallel 8
cd build && ctest -C Release -R CoreVideoMeetingControlPlan --output-on-failure
```

Expected: PASS, `meeting-control-plan: all tests passed`.

- [ ] **Step 5: Commit.**

```sh
git add src/meeting-control-plan.h tests/meeting-control-plan-test.cpp CMakeLists.txt
git commit -m "feat(prod-control): decision header — action naming, name resolution, privilege gate, report shape"
```

---

### Task 3: The engine branch — resolve, gate, call the SDK, report

The engine's command loop (`engine/src/main.cpp`) gains a `MeetingControl` branch after the `TalkbackNominate` branch (`engine/src/main.cpp:1664`). It gathers context from the SDK (roster pairs and self-role, via the same `GetMeetingParticipantsController()` the `INMEETING` handler uses at `main.cpp:1011`), runs `meeting_control_decide()`, makes exactly one SDK call, and reports through `meeting_control_report()` via `EngineIpc::write()` (`engine/src/engine-writer.h:48` — serialised, safe from this thread). The branch is thin wiring by design: every decision it makes is already pinned by Task 2's test, which is the only way this layer's logic gets tested at all (the SDK controllers cannot be faked cheaply — `IMeetingParticipantsController` has ~60 pure virtuals).

**Files:**
- Modify: `engine/src/main.cpp` (include near the other `src/` includes at the top; branch after line 1664's `TalkbackNominate` block closes)
- Test: covered by `tests/meeting-control-plan-test.cpp` (Task 2) for every decision, and by `tests/engine-command-test.cpp` (Task 1) for routing; this task's verification is the full suite staying green plus a clean engine build.

**Interfaces:**
- Consumes: `IpcCommand::MeetingControl` (Task 1); `meeting_control_action_of` / `meeting_control_resolve` / `meeting_control_decide` / `meeting_control_report` (Task 2); `json_str` (`engine/src/engine-json.h`), `to_zstr` and `zchar_to_utf8` (already used by the Join branch and `user_to_info` at `main.cpp:587`).
- Produces: the `{"cmd":"meeting_control",...}` E2P report line (Task 4 consumes it).

- [ ] **Step 1: Write the failing check.** Add to `tests/engine-command-test.cpp` the verbatim line the engine branch will parse — with `rename`'s extra fields — pinning the wire format before the branch exists:

```cpp
    routes(R"({"cmd":"meeting_control","action":"rename","participant":"Sarah Muller","new_name":"Sarah M (Panel)"})",
           IpcCommand::MeetingControl, "meeting_control rename routes with its payload fields");
    // allow_unmute rides mute/mute_all; absent means true (see the engine
    // branch: absence-of-":false" parsing, same style as video_only).
    routes(R"({"cmd":"meeting_control","action":"mute_all","allow_unmute":false})",
           IpcCommand::MeetingControl, "meeting_control mute_all routes with allow_unmute");
```

- [ ] **Step 2: Run to verify state.**

```sh
cmake --build build --config Release --target CoreVideoEngineCommandTest --parallel 8
cd build && ctest -C Release -R CoreVideoEngineCommand --output-on-failure
```

Expected: PASS (routing already exists from Task 1 — these lines extend the wire-format record; the failing half of this task is the engine build below, which does not compile until the branch's include is added correctly).

- [ ] **Step 3: Implement the branch.** In `engine/src/main.cpp`, add `#include "../../src/meeting-control-plan.h"` beside the existing `#include "../../src/engine-command.h"`, then after the `TalkbackNominate` branch:

```cpp
        } else if (command == IpcCommand::MeetingControl) {
            const std::string action_str = json_str(line, "action");
            const MeetingControlAction action = meeting_control_action_of(action_str);
            // One reporter for every exit: a refusal must be a named reason on
            // the pipe, never a silently skipped branch (the repo's standing
            // rule — fire-and-acknowledge means the ack is worthless without
            // the async verdict actually arriving).
            auto report = [&](bool ok, int code, const std::string &reason) {
                EngineIpc::write(meeting_control_report(action_str, ok, code, reason));
            };
            if (!meeting_svc) { report(false, -1, "not_in_meeting"); continue; }
            auto *part_ctrl  = meeting_svc->GetMeetingParticipantsController();
            auto *audio_ctrl = meeting_svc->GetMeetingAudioController();
            if (!part_ctrl || !audio_ctrl) { report(false, -1, "no_controller"); continue; }

            // Context is gathered fresh per command — ids are meeting-scoped,
            // so resolution happens HERE, never earlier (global constraint).
            uint32_t self_id = 0;
            bool privileged = false;
            if (auto *self = part_ctrl->GetMySelfUser()) {
                self_id = self->GetUserID();
                const ZOOMSDK::UserRole role = self->GetUserRole();
                privileged = self->IsHost() ||
                             role == ZOOMSDK::USERROLE_HOST ||
                             role == ZOOMSDK::USERROLE_COHOST;
            }
            std::vector<std::pair<std::string, uint32_t>> roster_pairs;
            if (auto *list = part_ctrl->GetParticipantsList()) {
                for (int i = 0; i < list->GetCount(); ++i) {
                    if (auto *u = part_ctrl->GetUserByUserID(list->GetItem(i)))
                        roster_pairs.emplace_back(zchar_to_utf8(u->GetUserName()),
                                                  u->GetUserID());
                }
            }
            const std::string who = json_str(line, "participant");
            const MeetingControlTarget target =
                meeting_control_resolve(roster_pairs, who, self_id);
            const MeetingControlVerdict verdict =
                meeting_control_decide(action, privileged, target);
            if (!verdict.allowed) { report(false, -1, verdict.reason); continue; }

            // Absent field defaults to allowing self-unmute — the humane
            // default; parsed by absence-of-":false" like video_only
            // (src/engine-command.h) because the naive decoder has no bools.
            const bool allow_unmute =
                line.find("\"allow_unmute\":false") == std::string::npos;
            ZOOMSDK::SDKError err = ZOOMSDK::SDKERR_UNKNOWN;
            switch (action) {
            case MeetingControlAction::Mute:
                err = audio_ctrl->MuteAudio(target.user_id, allow_unmute);
                break;
            case MeetingControlAction::AskUnmute:
                // A consent REQUEST on modern SDKs — the target sees the
                // host's unmute prompt; success means delivered, and the real
                // unmute shows up as the roster's is_muted flipping.
                err = audio_ctrl->UnMuteAudio(target.user_id);
                break;
            case MeetingControlAction::MuteAll:
                // MuteAudio's own doc: "ZERO(0) indicates to mute all the
                // participants." There is no dedicated mute-all call.
                err = audio_ctrl->MuteAudio(0, allow_unmute);
                break;
            case MeetingControlAction::Rename: {
                // Persistent storage of to_zstr's result is NOT needed here:
                // unlike Join(), ChangeUserName is synchronous — the pointer
                // only has to outlive the call.
                const auto wide_name = to_zstr(json_str(line, "new_name"));
                err = part_ctrl->ChangeUserName(target.user_id,
                                                wide_name.c_str(),
                                                /*bSaveUserName=*/false);
                break;
            }
            case MeetingControlAction::LowerAllHands:
                // false = everyone who is not a webinar attendee — the whole
                // room in a regular meeting (the signature's own doc).
                err = part_ctrl->LowerAllHands(false);
                break;
            case MeetingControlAction::Unknown:
                break; // unreachable: decide() refused it above
            }
            report(err == ZOOMSDK::SDKERR_SUCCESS, static_cast<int>(err),
                   err == ZOOMSDK::SDKERR_SUCCESS ? "" : "sdk_error");
```

- [ ] **Step 4: Build everything and run the whole suite.**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: full build (plugin AND engine — both binaries ship as a pair), all tests green.

- [ ] **Step 5: Commit.**

```sh
git add engine/src/main.cpp tests/engine-command-test.cpp
git commit -m "feat(prod-control): engine meeting_control branch — resolve, gate, call SDK, report verdict"
```

---

### Task 4: Plugin sender and result stash — `ZoomEngineClient`

The plugin needs a sender that emits the verbatim wire line Task 3 parses, and a `handle_event` branch that logs the engine's verdict line verbatim and stashes it for polling — exactly the `talkback_probe` pattern (`src/zoom-engine-client.cpp:1351-1363`): log verbatim (a stage that doesn't reach the log may as well not have been reported), stash under `m_mtx` with the lock scope kept to the copy alone.

**Files:**
- Modify: `src/zoom-engine-client.h` (declare `meeting_control()` + `meeting_control_status()` beside `talkback_probe()` at line 153; member `m_meeting_control_status` beside `m_talkback_probe_status` at line 395)
- Modify: `src/zoom-engine-client.cpp` (sender beside `talkback_probe` at line 907; `handle_event` branch beside the `talkback_probe` branch at line 1351)
- Test: `tests/engine-command-test.cpp` (the sender's verbatim output lines are already pinned by Tasks 1 and 3 — this task must keep the sender byte-identical to them)

**Interfaces:**
- Consumes: `IPC_CMD_MEETING_CONTROL` literal (via the emitted string), `json_escape` (already in `zoom-engine-client.cpp`).
- Produces:

```cpp
void ZoomEngineClient::meeting_control(const std::string &action,
    const std::string &participant, const std::string &new_name, bool allow_unmute);
std::string ZoomEngineClient::meeting_control_status() const; // raw last verdict line, "" before the first
```

- [ ] **Step 1: Confirm the wire pin exists (the failing state).** The check is Task 3's `routes(...)` lines: the sender below must produce those bytes. Run:

```sh
cd build && ctest -C Release -R CoreVideoEngineCommand --output-on-failure
```

Expected: PASS — and any drift the sender introduces later must be reflected there first (that file doubles as the wire-format record; a sender emitting a line the record does not contain is the failure this step guards).

- [ ] **Step 2: Implement the sender.** In `src/zoom-engine-client.cpp`, after `talkback_probe()`:

```cpp
void ZoomEngineClient::meeting_control(const std::string &action,
                                       const std::string &participant,
                                       const std::string &new_name,
                                       bool allow_unmute)
{
    if (!m_running.load(std::memory_order_acquire)) return;
    // "action" is emitted FIRST after cmd and is never participant-controlled;
    // the participant-controlled strings come last. Same first-match-scan
    // reasoning as talkback_nominate()'s "attempt" ordering: the engine's
    // json_str() is not a real JSON parser, so nothing an operator or a
    // participant types may sit between a needle and its value.
    std::string json = R"({"cmd":"meeting_control","action":")" +
                       json_escape(action) + "\"";
    if (!allow_unmute) json += R"(,"allow_unmute":false)";
    if (!participant.empty())
        json += R"(,"participant":")" + json_escape(participant) + "\"";
    if (!new_name.empty())
        json += R"(,"new_name":")" + json_escape(new_name) + "\"";
    json += "}";
    write_json(json);
}

std::string ZoomEngineClient::meeting_control_status() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_meeting_control_status;
}
```

In `handle_event()`, beside the `talkback_probe` branch:

```cpp
    if (cmd == "meeting_control") {
        // Verbatim, like every fire-and-acknowledge verdict in this file: the
        // ack over the socket promised nothing except that THIS line would
        // eventually say what happened.
        blog(LOG_INFO, "[obs-zoom-plugin] meeting_control: %s", line.c_str());
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_meeting_control_status = line;
        }
        return;
    }
```

Declarations in `src/zoom-engine-client.h` beside `talkback_probe()` (line 153) and member `std::string m_meeting_control_status;` beside `m_talkback_probe_status` (line 395). Clear it in the `cmd == "left"` world-reset branch (`handle_event()`, the same place `m_roster` is cleared — a verdict from the previous meeting must not answer a poll about this one).

- [ ] **Step 3: Build and run the suite.**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: all green.

- [ ] **Step 4: Commit.**

```sh
git add src/zoom-engine-client.h src/zoom-engine-client.cpp
git commit -m "feat(prod-control): plugin meeting_control sender + verbatim verdict stash"
```

---

### Task 5: Control API — `meeting_control` and `meeting_control_status`

The TCP control API (`src/zoom-control-server.cpp`, 127.0.0.1:19870) gains the operator-facing command, mirroring `talkback_nominate`'s shape exactly (`zoom-control-server.cpp:888-941`): validate synchronously with named errors, check `is_running()` BEFORE acking (F6's lesson — `meeting_control()` early-returns on a dead pipe, so acking first would ack a silently dropped command), fire, and ack with a note pointing at the async verdict. The verdict is polled via `meeting_control_status`.

**Files:**
- Modify: `src/zoom-control-server.cpp` (new handlers after the `talkback_key` block; add both command names to the `help` list at line 471)
- Test: request validation is pinned by `tests/meeting-control-plan-test.cpp` (Task 2's `meeting_control_validate` checks); this task wires it.

**Interfaces:**
- Consumes: `meeting_control_action_of` / `meeting_control_validate` (Task 2); `ZoomEngineClient::meeting_control()` / `meeting_control_status()` (Task 4).
- Produces: control-API commands `{"cmd":"meeting_control","action":"mute","participant":"Sarah Muller","allow_unmute":true}` and `{"cmd":"meeting_control_status"}`.

- [ ] **Step 1: The failing check.** Add to `tests/meeting-control-plan-test.cpp` (this is the exact contract the handler enforces — write it first):

```cpp
    // The control server must refuse a rename that names nobody new — pinned
    // here because the server itself needs Qt+libobs to compile and the two
    // Majors this repo shipped in Task-5-shaped wiring were both unreachable
    // by any host test until their logic moved into a header.
    check(meeting_control_validate(meeting_control_action_of("rename"), "", "New Name") ==
              "participant_required",
          "rename with a new_name but no participant is refused participant_required first");
```

- [ ] **Step 2: Run to verify it fails, then passes** (it passes immediately if Task 2's ordering already checks participant before new_name — if it does, keep the check as a pin and note it compiled red-green with Task 2's header absent):

```sh
cmake --build build --config Release --target CoreVideoMeetingControlPlanTest --parallel 8
cd build && ctest -C Release -R CoreVideoMeetingControlPlan --output-on-failure
```

Expected: PASS.

- [ ] **Step 3: Implement the handlers.** In `src/zoom-control-server.cpp` after the `talkback_key` block (include `"meeting-control-plan.h"` at the top):

```cpp
    // Production push-control: fire-and-acknowledge, talkback_nominate's
    // shape. The ack only confirms the trigger; the SDKError verdict arrives
    // as a "meeting_control" OBS log line and is polled below.
    if (cmd == "meeting_control") {
        const std::string action_str  = req.value("action").toString().toStdString();
        const std::string participant = req.value("participant").toString().toStdString();
        const std::string new_name    = req.value("new_name").toString().toStdString();
        const MeetingControlAction action = meeting_control_action_of(action_str);
        const std::string invalid =
            meeting_control_validate(action, participant, new_name);
        if (!invalid.empty()) {
            write_response(socket, {{"ok", false},
                                    {"error", QString::fromStdString(invalid)}});
            return;
        }
        // F6's lesson: is_running() BEFORE the ack — meeting_control() drops
        // the command silently on a dead pipe.
        if (!ZoomEngineClient::instance().is_running()) {
            write_response(socket, {{"ok", false}, {"error", "engine_not_running"},
                {"message", "The Zoom engine is not running."}});
            return;
        }
        if (ZoomEngineClient::instance().state() != MeetingState::InMeeting) {
            write_response(socket, {{"ok", false}, {"error", "not_in_meeting"},
                {"message", "Join the meeting before sending meeting control."}});
            return;
        }
        blog(LOG_INFO, "[obs-zoom-plugin] Control API: meeting_control action=%s",
             action_str.c_str());
        ZoomEngineClient::instance().meeting_control(action_str, participant,
            new_name, req.value("allow_unmute").toBool(true));
        write_response(socket, {{"ok", true},
            {"note", "sent; poll meeting_control_status or watch the OBS log "
                     "for the meeting_control verdict"}});
        return;
    }

    if (cmd == "meeting_control_status") {
        write_response(socket, {{"ok", true},
            {"last_result", QString::fromStdString(
                 ZoomEngineClient::instance().meeting_control_status())}});
        return;
    }
```

Add `"meeting_control"` and `"meeting_control_status"` to the `help` command list.

- [ ] **Step 4: Build, run the suite, and drive it live once.**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Then against a real meeting (single OBS instance; host or co-host account): `join`, `start_engine` twice, then `{"cmd":"meeting_control","action":"mute","participant":"<name>"}` — expect `ok:true` on the socket, a `meeting_control ... "ok":true,"code":0` OBS log line, and the dock roster's mute badge flipping; then the same from a NON-privileged join — expect `"reason":"not_host_or_cohost"` in the verdict, never silence. Send `{"cmd":"leave"}` before closing OBS.

- [ ] **Step 5: Commit.**

```sh
git add src/zoom-control-server.cpp tests/meeting-control-plan-test.cpp
git commit -m "feat(prod-control): control API meeting_control + polled verdict"
```

---

### Task 6: Dock — per-participant context menu

The dock's participant list (`m_participant_list`, `src/zoom-dock.h:113`, rebuilt at `src/zoom-dock.cpp:2227-2248`) already stores the display name at `Qt::UserRole + 1` precisely so by-name actions never resolve off the id (the comment at `zoom-dock.cpp:2236-2240` says exactly this). A right-click menu offers the state-appropriate actions; which entries appear is pure logic in the plan header, because the dock needs libobs and Qt to compile and this repo's rule is that no decision lives where no test can reach it.

**Files:**
- Modify: `src/meeting-control-plan.h` (menu helper), `tests/meeting-control-plan-test.cpp`
- Modify: `src/zoom-dock.cpp` (context-menu policy where `m_participant_list` is constructed; handler)

**Interfaces:**
- Consumes: `ZoomEngineClient::meeting_control()` (Task 4), `ZoomEngineClient::roster()`.
- Produces: `std::vector<MeetingControlMenuItem> meeting_control_menu(bool is_muted);`

- [ ] **Step 1: The failing test.** Append to `tests/meeting-control-plan-test.cpp`:

```cpp
    // ── Dock menu composition (Task 6) ──────────────────────────────────────
    {
        const auto unmuted = meeting_control_menu(false);
        check(unmuted.size() == 2 &&
                  unmuted[0].action == MeetingControlAction::Mute &&
                  unmuted[1].action == MeetingControlAction::Rename,
              "unmuted participant offers Mute then Rename — never Ask to Unmute");
        const auto muted = meeting_control_menu(true);
        check(muted.size() == 2 &&
                  muted[0].action == MeetingControlAction::AskUnmute &&
                  muted[1].action == MeetingControlAction::Rename,
              "muted participant offers Ask to Unmute then Rename — never Mute");
    }
```

- [ ] **Step 2: Run to verify it fails.**

```sh
cmake --build build --config Release --target CoreVideoMeetingControlPlanTest --parallel 8
```

Expected: compile FAILURE, `'meeting_control_menu': identifier not found`.

- [ ] **Step 3: Implement.** In `src/meeting-control-plan.h`:

```cpp
struct MeetingControlMenuItem {
    MeetingControlAction action;
    const char *label; // UI text; the dock uses it verbatim
};

// Offering "Mute" on someone already muted (or "Ask to Unmute" on someone
// talking) is a menu that can only refuse — the dock-keys milestone's m3/m4
// rule: never offer a press whose only possible outcome is a refusal.
inline std::vector<MeetingControlMenuItem> meeting_control_menu(bool is_muted)
{
    std::vector<MeetingControlMenuItem> items;
    if (is_muted)
        items.push_back({MeetingControlAction::AskUnmute, "Ask to Unmute"});
    else
        items.push_back({MeetingControlAction::Mute, "Mute"});
    items.push_back({MeetingControlAction::Rename, "Rename..."});
    return items;
}
```

In `src/zoom-dock.cpp`, where `m_participant_list` is constructed, set `m_participant_list->setContextMenuPolicy(Qt::CustomContextMenu);` and connect:

```cpp
    connect(m_participant_list, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) {
        auto *item = m_participant_list->itemAt(pos);
        if (!item) return;
        // By NAME, from UserRole+1 — the id in UserRole is meeting-scoped and
        // this menu can outlive a roster tick (the stored-name comment at the
        // rebuild site is the law here).
        const std::string name =
            item->data(Qt::UserRole + 1).toString().toStdString();
        bool is_muted = false;
        for (const auto &p : ZoomEngineClient::instance().roster())
            if (p.display_name == name) { is_muted = p.is_muted; break; }
        QMenu menu(m_participant_list);
        for (const auto &entry : meeting_control_menu(is_muted)) {
            const MeetingControlAction action = entry.action;
            menu.addAction(QString::fromUtf8(entry.label), [this, action, name]() {
                std::string new_name;
                if (action == MeetingControlAction::Rename) {
                    bool accepted = false;
                    new_name = QInputDialog::getText(this, tr("Rename Participant"),
                        tr("New display name:"), QLineEdit::Normal,
                        QString::fromStdString(name), &accepted).toStdString();
                    if (!accepted || new_name.empty()) return;
                }
                const char *action_str =
                    action == MeetingControlAction::Mute      ? "mute" :
                    action == MeetingControlAction::AskUnmute ? "ask_unmute" :
                                                                "rename";
                ZoomEngineClient::instance().meeting_control(action_str, name,
                                                             new_name, true);
            });
        }
        menu.addSeparator();
        menu.addAction(tr("Mute All"), []() {
            ZoomEngineClient::instance().meeting_control("mute_all", "", "", true);
        });
        menu.addAction(tr("Lower All Hands"), []() {
            ZoomEngineClient::instance().meeting_control("lower_all_hands", "", "", true);
        });
        menu.exec(m_participant_list->mapToGlobal(pos));
    });
```

Add `#include <QMenu>`, `#include <QInputDialog>`, and `#include "meeting-control-plan.h"` to `zoom-dock.cpp`'s includes. The verdict surfaces exactly as everywhere else: the `meeting_control` OBS log line, and the roster badge flipping on the next `participants` update — no new label.

- [ ] **Step 4: Run to verify it passes.**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: full build, all green. Then live: right-click a participant in the dock while host — mute badge flips within one roster tick.

- [ ] **Step 5: Commit.**

```sh
git add src/meeting-control-plan.h tests/meeting-control-plan-test.cpp src/zoom-dock.cpp
git commit -m "feat(prod-control): dock participant context menu (mute / ask-unmute / rename / mute-all / lower-hands)"
```

---

### Task 7: Companion actions

Five new actions in `companion/companion-module-corevideo-obs/src/actions.ts`, sending `meeting_control` over the same control API via `inst.sendPlugin()`. Participants are stored BY NAME (the `participantChoices` dropdown at `actions.ts:24-27` already exists and is already rebuilt on roster change by `index.ts:203`) — and unlike `zoom_assign`, these actions send the NAME itself, never a resolved id: the control API takes names, so no `resolveParticipantId` call, no staleness window at all. The module has vitest (`state.test.ts`), so this task is genuinely test-first.

**Files:**
- Create: `companion/companion-module-corevideo-obs/src/actions.test.ts`
- Modify: `companion/companion-module-corevideo-obs/src/actions.ts`
- Modify: `companion/companion-module-corevideo-obs/package.json` (version 1.0.2 → 1.0.3, BOTH the top-level `"version"` and the `companion.version` block) and `companion/companion-module-corevideo-obs/companion/manifest.json` (`"version": "1.0.3"`; leave `runtime.apiVersion` at 2.1.3)

**Interfaces:**
- Consumes: control-API `meeting_control` (Task 5) via `inst.sendPlugin`.
- Produces: actions `zoom_mute_participant`, `zoom_ask_unmute`, `zoom_mute_all`, `zoom_rename_participant`, `zoom_lower_all_hands`.

- [ ] **Step 1: Write the failing test.** Create `src/actions.test.ts`:

```ts
import { describe, expect, it, vi } from 'vitest'
import { buildActions } from './actions.js'
import { defaultState } from './state.js'
import type { CoreVideoInstance } from './index.js'

function stubInstance() {
	const sendPlugin = vi.fn()
	const state = defaultState()
	state.zoom.participants = [
		{ id: 101, name: 'Sarah Muller', has_video: true, is_talking: false, is_muted: false },
		{ id: 102, name: 'Luis Ortiz', has_video: true, is_talking: false, is_muted: true },
	]
	const inst = {
		state,
		sendPlugin,
		sendSidecar: vi.fn(),
		obsRequest: vi.fn(),
		log: vi.fn(),
	} as unknown as CoreVideoInstance
	return { inst, sendPlugin }
}

describe('meeting_control actions', () => {
	it('sends mute by NAME, never by id', () => {
		const { inst, sendPlugin } = stubInstance()
		const actions = buildActions(inst) as Record<string, { callback: (a: unknown) => unknown }>
		actions.zoom_mute_participant.callback({ options: { participant: 'Sarah Muller' } })
		expect(sendPlugin).toHaveBeenCalledWith({
			cmd: 'meeting_control', action: 'mute', participant: 'Sarah Muller',
		})
	})

	it('sends ask_unmute — the action is a request, so it is not named unmute', () => {
		const { inst, sendPlugin } = stubInstance()
		const actions = buildActions(inst) as Record<string, { callback: (a: unknown) => unknown }>
		actions.zoom_ask_unmute.callback({ options: { participant: 'Luis Ortiz' } })
		expect(sendPlugin).toHaveBeenCalledWith({
			cmd: 'meeting_control', action: 'ask_unmute', participant: 'Luis Ortiz',
		})
	})

	it('sends mute_all with allow_unmute carried through', () => {
		const { inst, sendPlugin } = stubInstance()
		const actions = buildActions(inst) as Record<string, { callback: (a: unknown) => unknown }>
		actions.zoom_mute_all.callback({ options: { allow_unmute: false } })
		expect(sendPlugin).toHaveBeenCalledWith({
			cmd: 'meeting_control', action: 'mute_all', allow_unmute: false,
		})
	})

	it('sends rename with the new name', () => {
		const { inst, sendPlugin } = stubInstance()
		const actions = buildActions(inst) as Record<string, { callback: (a: unknown) => unknown }>
		actions.zoom_rename_participant.callback({
			options: { participant: 'Sarah Muller', new_name: 'Sarah M (Panel)' },
		})
		expect(sendPlugin).toHaveBeenCalledWith({
			cmd: 'meeting_control', action: 'rename',
			participant: 'Sarah Muller', new_name: 'Sarah M (Panel)',
		})
	})

	it('sends lower_all_hands with no target', () => {
		const { inst, sendPlugin } = stubInstance()
		const actions = buildActions(inst) as Record<string, { callback: (a: unknown) => unknown }>
		actions.zoom_lower_all_hands.callback({ options: {} })
		expect(sendPlugin).toHaveBeenCalledWith({ cmd: 'meeting_control', action: 'lower_all_hands' })
	})
})
```

- [ ] **Step 2: Run to verify it fails.**

```sh
cd companion/companion-module-corevideo-obs && npm run test
```

Expected: FAIL — `actions.zoom_mute_participant` is `undefined`.

- [ ] **Step 3: Implement.** In `src/actions.ts`, add a section after `zoom_cancel_recovery` (inside the returned object). The participant dropdowns reuse the existing `participantChoices` const (rebuilt on roster change per the module's CLAUDE rules):

```ts
		// ── Zoom: Meeting control (push) ────────────────────────────────────────
		// All by NAME — the control API resolves to a live id engine-side at
		// execution time, so unlike zoom_assign there is no id resolution (and
		// no staleness window) in this module at all. Verdicts are async: the
		// plugin ack only confirms the trigger; the SDKError verdict lands in
		// the OBS log and in meeting_control_status.

		zoom_mute_participant: {
			name: 'Zoom: Mute Participant',
			options: [
				{
					type: 'dropdown', id: 'participant', label: 'Participant (by name)',
					default: '', allowCustom: true, choices: participantChoices,
				},
			],
			callback: (a) => inst.sendPlugin({ cmd: 'meeting_control', action: 'mute',
				participant: a.options.participant }),
		},

		zoom_ask_unmute: {
			// "Ask", honestly: the SDK's UnMuteAudio on another user raises the
			// host's unmute prompt on THEIR screen — it cannot force a mic hot.
			name: 'Zoom: Ask Participant to Unmute',
			options: [
				{
					type: 'dropdown', id: 'participant', label: 'Participant (by name)',
					default: '', allowCustom: true, choices: participantChoices,
				},
			],
			callback: (a) => inst.sendPlugin({ cmd: 'meeting_control', action: 'ask_unmute',
				participant: a.options.participant }),
		},

		zoom_mute_all: {
			name: 'Zoom: Mute All',
			options: [
				{ type: 'checkbox', id: 'allow_unmute', label: 'Allow self-unmute', default: true },
			],
			callback: (a) => inst.sendPlugin({ cmd: 'meeting_control', action: 'mute_all',
				allow_unmute: a.options.allow_unmute }),
		},

		zoom_rename_participant: {
			name: 'Zoom: Rename Participant',
			options: [
				{
					type: 'dropdown', id: 'participant', label: 'Participant (by name)',
					default: '', allowCustom: true, choices: participantChoices,
				},
				{ type: 'textinput', id: 'new_name', label: 'New display name', default: '' },
			],
			callback: (a) => inst.sendPlugin({ cmd: 'meeting_control', action: 'rename',
				participant: a.options.participant, new_name: a.options.new_name }),
		},

		zoom_lower_all_hands: {
			name: 'Zoom: Lower All Hands',
			options: [],
			callback: () => inst.sendPlugin({ cmd: 'meeting_control', action: 'lower_all_hands' }),
		},
```

Then bump the version to 1.0.3 in `package.json` (both places) and `companion/manifest.json` — Companion refuses to overwrite a module version already on disk, so an unbumped rebuild silently keeps testing the old bundle.

- [ ] **Step 4: Run to verify it passes, then build the installable bundle.**

```sh
cd companion/companion-module-corevideo-obs && npm run test && npm run build
node -e "import('./dist/index.js').then(m => { if (typeof m.default !== 'function') { console.error('entrypoint is not a constructor'); process.exit(1); } console.log('entrypoint OK'); })"
```

Expected: vitest green, `tsc` clean, `entrypoint OK` (the ESM/constructor check the Companion loader itself performs).

- [ ] **Step 5: Commit.**

```sh
git add companion/companion-module-corevideo-obs/src/actions.ts companion/companion-module-corevideo-obs/src/actions.test.ts companion/companion-module-corevideo-obs/package.json companion/companion-module-corevideo-obs/companion/manifest.json
git commit -m "feat(prod-control): Companion actions — mute / ask-unmute / mute-all / rename / lower-all-hands"
```

---

Closing notes for the implementer: update `CLAUDE.md` in the same change as the substantive work (standing directive — docs-updated is part of done): the control-API section gains `meeting_control`/`meeting_control_status`, and the Companion section's action list grows by five. Nothing in this plan touches the media path, the audio ring, or teardown ordering; every invariant in CLAUDE.md's list is untouched by construction because `meeting_control` is a pure control-plane round trip.
