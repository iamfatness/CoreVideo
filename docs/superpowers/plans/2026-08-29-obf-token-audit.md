# Zoom OBF / App-Privilege Token — March 2026 Compliance Audit & Gap-Close Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove (with tests) that CoreVideo's existing join path already carries Zoom's on-behalf / ZAK / app-privilege tokens end-to-end, then close the four verified gaps — a named client-side error for the cross-account-without-token rejection, per-profile token persistence, operator docs, and a live gate against a meeting on another account — before Zoom's March 2, 2026 enforcement date.

**Architecture:** Two processes: the OBS plugin (`src/`) collects tokens from three inlets (dock combo+field, join-URL query params, control-API JSON) into `ZoomJoinAuthTokens`, and the engine (`engine/src/main.cpp`) copies them into `ZOOMSDK::JoinParam4WithoutLogin` verbatim. All new decision logic goes into the existing pure, SDK-free header `src/zoom-join-decision.h` so it is unit-testable without a live meeting; only thin wiring touches Qt/engine code.

**Tech Stack:** C++17, Zoom Windows Meeting SDK 7.1.5.43953, CMake + CTest, named-pipe line-JSON IPC, Qt6.

**Policy background (Zoom marketplace policy, NOT an SDK-header fact):** Zoom announced that from **March 2, 2026**, Meeting-SDK apps joining meetings hosted OUTSIDE the app's own account must supply an app privilege token (OBF) — or a ZAK / on-behalf token establishing user context — in the join parameters. Joins into cross-account meetings without one will be rejected **server-side**; the SDK headers ship the fields and the `MEETING_FAIL_*` codes but say nothing about the enforcement date. The client cannot ask Zoom "is this meeting on my account?" before joining (see audit finding A7), so client-side detection has to key off the rejection code plus what we know we sent.

**Spec:** This document doubles as the spec.

**Requirements — what already exists, verbatim from the audit (do not re-implement any of this):**

- **A1 — Engine join plumbing is complete.** `engine/src/main.cpp:1444-1446` parses `on_behalf_token`, `user_zak`, `app_privilege_token` from the join JSON; `1448-1454` acks presence booleans (`has_on_behalf_token`, `has_user_zak`, `has_app_privilege_token` — never values); `1465-1467` copies into persistent globals `g_wide_on_behalf_token` / `g_wide_user_zak` / `g_wide_app_privilege_token` (declared ~1317-1327, wide on Windows) so the `JoinParam` raw pointers outlive the async `Join()`; `1487-1489` sets `p.onBehalfToken`, `p.userZAK`, `p.app_privilege_token` (nullptr when empty). A host-start fallback (`main.cpp:831-859`) re-joins as host with `p.userZAK = m_host_start_zak.c_str()` when code 63 arrives and a ZAK was supplied (`1101-1107`, `try_host_start_after_external_join_failure`).
- **A2 — The SDK struct has the fields.** `third_party/zoom-sdk/h/meeting_service_interface.h:250-290`, `tagJoinParam4WithoutLogin`: `meetingNumber`, `vanityID`, `userName`, `psw`, `app_privilege_token` (doc comment: "app_privilege_token."), `userZAK` ("ZOOM access token."), `customer_key`, `webinarToken`, `isVideoOff`, `isAudioOff`, `join_token` ("Join token."), `onBehalfToken` ("On behalf token."), `isMyVoiceInMix`, `hDirectShareAppWnd`/`isDirectShareDesktop` (WIN32), `isAudioRawDataStereo`, `eAudioRawdataSamplingRate`, `eVideoRawdataColorspace`. Note `tagJoinParam4NormalUser` (296-333) has `app_privilege_token` and `join_token` but **no** `onBehalfToken`/`userZAK` — the engine always joins `SDK_UT_WITHOUT_LOGIN` (`main.cpp:1482`), which is the union arm that has all three.
- **A3 — MeetingFailCode candidates** (`meeting_service_interface.h:117-148`): `MEETING_FAIL_FORBID_TO_JOIN_INTERNAL_MEETING = 60`, `MEETING_FAIL_HOST_DISALLOW_OUTSIDE_USER_JOIN = 62`, `MEETING_FAIL_UNABLE_TO_JOIN_EXTERNAL_MEETING = 63`, `MEETING_FAIL_BLOCKED_BY_ACCOUNT_ADMIN = 64`, `MEETING_FAIL_APP_PRIVILEGE_TOKEN_ERROR = 500`, `MEETING_FAIL_AUTHORIZED_USER_NOT_INMEETING = 501`, `MEETING_FAIL_ON_BEHALF_TOKEN_CONFLICT_LOGIN_ERROR = 502`, `MEETING_FAIL_USER_LEVEL_TOKEN_NOT_HAVE_HOST_ZAK_OBF = 503`, `MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING = 504`, `MEETING_FAIL_ON_BEHALF_TOKEN_INVALID = 505`, `MEETING_FAIL_ON_BEHALF_TOKEN_NOT_MATCH_MEETING = 506`, `MEETING_FAIL_JMAK_USER_EMAIL_NOT_MATCH = 1143`. Which of these the server actually sends for "cross-account, no token" is **unknown until the Task 6 live gate** — 63 is the working hypothesis (the engine already special-cases it for host-start retry).
- **A4 — URL parser extracts all three tokens.** `src/obs-utils.h:6-11` (`ParsedJoin` carries `on_behalf_token`, `user_zak`, `app_privilege_token`); `src/obs-utils.cpp:134-143` accepts query keys `obf`/`on_behalf_token`/`onbehalftoken`, `zak`/`user_zak`/`userzak`, `app_privilege_token`/`appprivilegetoken`/`apptoken`/`app_token`.
- **A5 — Both operator inlets accept tokens.** Dock: token-type combo (`src/zoom-dock.cpp:753-756`: "Zoom sign-in"/`auto_zak`, "User ZAK"/`user_zak`, "App privilege token"/`app_privilege_token`) + password-mode `m_join_token` field (762-768); join click maps typed token by type into `ZoomJoinAuthTokens` (2564-2586) and refuses a manual type with an empty field. Control API: `src/zoom-control-server.cpp:735-741` accepts `on_behalf_token`/`user_zak`/`app_privilege_token` request fields, falling back to the URL-parsed values.
- **A6 — A pure join-decision unit exists** (`src/zoom-join-decision.h`, pinned by `tests/join-decision-test.cpp`, 63 checks): `plan_join()` reasons about `token_type`, ZAK sourcing, and sign-in blocking errors; `classify_meeting_fail()` already maps 500/502/503/505/506 → `OnBehalfTokenInvalid`, 60/62/63/64 → `MissingApproval`, 504/82/23 → `NeedsSignIn`. `zoom_error_message()` (`src/zoom-engine-client.cpp:82`, `meeting_failed` branch at 116) hand-writes messages for 63/504/505/506.
- **A7 — What does NOT exist (the gaps this plan closes):** (G1) Nothing distinguishes "cross-account join attempted with **no** token" from a generic approval failure — code 63 without any token yields the app-approval message, which sends the operator to the Marketplace instead of to the token field; nothing refuses or warns **before** the SDK round-trip, and nothing can (the SDK's `IAuthService::GetLoginStatus()`/`GetAccountInfo()` (`auth_service_interface.h:328-334`) reflect SDK login, which this app never uses — there is no pre-join "whose meeting is this" query). (G2) The typed token and token type are **not persisted** — only `last_meeting_id`/`last_display_name`/`last_was_webinar` are saved on join (`zoom-dock.cpp:2754-2758`; `ZoomPluginSettings` in `src/zoom-settings.h` has no join-token fields) — yet the repo already has DPAPI secret storage (`protect_secret`/`unprotect_secret`, `src/zoom-settings.cpp:176/206`, used for OAuth tokens at 349-352), so persisting the token there is consistent with what exists. (G3) Operator docs: `README.md:343` describes the automatic ZAK flow only; neither README nor `wiki/Support.md` documents the token combo, OBF URLs, or the March 2026 policy. (G4) No live verification that a cross-account join with a token succeeds and without one fails with the code we classify.
- **A8 — `sidecar/` is not a broker.** It is `CoreVideoSidecar`, a Qt6 Widgets companion UI (layout templates, look library, scenes/macros panels, an OBS websocket client). Nothing in it mints or brokers tokens; no integration is planned here.

## Global Constraints

- Build: `cmake --build build --config Release --parallel 8`. Test: `cd build && ctest -C Release --output-on-failure` — must be **N/N green** (63 tests before this plan; each task states the expected count after it).
- Tests are plain executables, no framework, `check()`-style, one file per invariant cluster in `tests/`, registered in `CMakeLists.txt` with `add_executable` + `add_test`.
- **Never log a token VALUE.** Log presence booleans only — the engine already acks `has_app_privilege_token`; every new log line and IPC field follows that rule. `zoom_join::redacted_tail()` exists for identifiers that must appear at all; join tokens do not even get a tail.
- Comments state the constraint the code cannot show; when a change is motivated by a live failure, say what happened, with numbers.
- Never run a second OBS instance while one is testing (pipe/SDK singleton collision, crash loop). Send `{"cmd":"leave"}` before closing OBS.
- Task 1 is an AUDIT pin: its tests are expected to pass immediately. A Task 1 test that FAILS is an audit finding — stop, report it, do not "fix" the test.

---

### Task 1: Pin the existing plumbing — SDK fields, fail codes, URL parser, decision unit

The whole point of this plan being an audit first: the March 2026 requirement is already mostly met, and the riskiest way to "comply" would be to rebuild working plumbing. This task converts audit findings A2-A6 into compiled assertions so an SDK upgrade that renames `app_privilege_token`, renumbers `MEETING_FAIL_APP_PRIVILEGE_TOKEN_ERROR`, or a refactor that drops a URL query alias fails CI instead of failing a show. The SDK-header pin gets its own executable because it is the only test that includes `meeting_service_interface.h`.

**Files:**
- Create: `tests/obf-join-fields-test.cpp`
- Modify: `tests/join-input-test.cpp` (append checks before the final pass/fail block), `CMakeLists.txt` (register `CoreVideoObfJoinFieldsTest` next to `CoreVideoJoinInputTest`, ~line 616)
- Test: both of the above

**Interfaces:**
- Consumes: `ZOOMSDK::JoinParam4WithoutLogin`, `ZOOMSDK::MeetingFailCode` (`third_party/zoom-sdk/h/meeting_service_interface.h`), `zoom_join_utils::parse_join_input` (`src/obs-utils.h`).
- Produces: nothing new — pins only. Task 2 relies on the pinned numeric codes.

- [ ] **Step 1: Write the pin tests (expected to PASS — a failure is an audit finding)**

`tests/obf-join-fields-test.cpp`:

```cpp
// Pins audit findings A2/A3 (docs/superpowers/plans/2026-08-29-obf-token-audit.md):
// the Meeting SDK struct fields and fail codes the March 2026 OBF requirement
// rides on. If an SDK upgrade renames or renumbers any of these, this fails at
// build/test time instead of at join time on a live show.
#include "meeting_service_interface.h"
#include <cstdio>
#include <string>

static int failures = 0;
static void check(bool cond, const std::string &name)
{
    if (!cond) { ++failures; std::printf("FAIL: %s\n", name.c_str()); }
}

int main()
{
    // Field-name pin: assignability is the compile-time assertion. The engine
    // joins SDK_UT_WITHOUT_LOGIN only (engine/src/main.cpp:1482) -- the union
    // arm that carries all three token fields (NormalUser lacks onBehalfToken
    // and userZAK entirely).
    ZOOMSDK::JoinParam4WithoutLogin p{};
    p.app_privilege_token = nullptr;
    p.userZAK             = nullptr;
    p.onBehalfToken       = nullptr;
    p.join_token          = nullptr;
    check(p.app_privilege_token == nullptr, "JoinParam4WithoutLogin token fields exist");

    // Numeric pins for every code classify_meeting_fail() and the engine's
    // meeting_fail_name() switch on. These are wire values from Zoom's server;
    // the classifiers hold ints so they stay SDK-header-free.
    check(ZOOMSDK::MEETING_FAIL_FORBID_TO_JOIN_INTERNAL_MEETING == 60,  "code 60");
    check(ZOOMSDK::MEETING_FAIL_HOST_DISALLOW_OUTSIDE_USER_JOIN == 62,  "code 62");
    check(ZOOMSDK::MEETING_FAIL_UNABLE_TO_JOIN_EXTERNAL_MEETING == 63,  "code 63");
    check(ZOOMSDK::MEETING_FAIL_BLOCKED_BY_ACCOUNT_ADMIN == 64,         "code 64");
    check(ZOOMSDK::MEETING_FAIL_APP_PRIVILEGE_TOKEN_ERROR == 500,       "code 500");
    check(ZOOMSDK::MEETING_FAIL_AUTHORIZED_USER_NOT_INMEETING == 501,   "code 501");
    check(ZOOMSDK::MEETING_FAIL_ON_BEHALF_TOKEN_CONFLICT_LOGIN_ERROR == 502, "code 502");
    check(ZOOMSDK::MEETING_FAIL_USER_LEVEL_TOKEN_NOT_HAVE_HOST_ZAK_OBF == 503, "code 503");
    check(ZOOMSDK::MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING == 504, "code 504");
    check(ZOOMSDK::MEETING_FAIL_ON_BEHALF_TOKEN_INVALID == 505,         "code 505");
    check(ZOOMSDK::MEETING_FAIL_ON_BEHALF_TOKEN_NOT_MATCH_MEETING == 506, "code 506");
    check(ZOOMSDK::MEETING_FAIL_JMAK_USER_EMAIL_NOT_MATCH == 1143,      "code 1143");

    if (failures == 0) { std::printf("obf-join-fields: all checks passed\n"); return 0; }
    return 1;
}
```

Append inside `main()` in `tests/join-input-test.cpp`, before its final pass/fail block:

```cpp
    // --- Audit pin A4: every documented token query-key alias still parses ---
    {
        const auto p = zoom_join_utils::parse_join_input(
            "https://zoom.us/j/123456789?pwd=abc&obf=OBF_T1&zak=ZAK_T1&apptoken=APT_T1");
        check(p.meeting_id == "123456789",       "token url: id");
        check(p.passcode == "abc",               "token url: passcode");
        check(p.on_behalf_token == "OBF_T1",     "token url: obf -> on_behalf_token");
        check(p.user_zak == "ZAK_T1",            "token url: zak -> user_zak");
        check(p.app_privilege_token == "APT_T1", "token url: apptoken -> app_privilege_token");
    }
    {
        const auto p = zoom_join_utils::parse_join_input(
            "https://us02web.zoom.us/j/987654321?onbehalftoken=OB2&userzak=Z2&app_privilege_token=A2");
        check(p.on_behalf_token == "OB2",     "token url long keys: on_behalf");
        check(p.user_zak == "Z2",             "token url long keys: zak");
        check(p.app_privilege_token == "A2",  "token url long keys: app_privilege");
    }
```

CMakeLists.txt, after the `CoreVideoJoinInput` registration (~line 624):

```cmake
    add_executable(CoreVideoObfJoinFieldsTest
        tests/obf-join-fields-test.cpp
    )
    target_include_directories(CoreVideoObfJoinFieldsTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zoom-sdk/h)
    add_test(NAME CoreVideoObfJoinFields
             COMMAND CoreVideoObfJoinFieldsTest)
```

- [ ] **Step 2: Build and run**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: **64/64 green** (63 existing + `CoreVideoObfJoinFields`). Every new check passes on first run. If any fails, STOP: that is a real divergence between this plan's audit and the tree — report it before continuing.

- [ ] **Step 3: Commit**

```sh
git add tests/obf-join-fields-test.cpp tests/join-input-test.cpp CMakeLists.txt
git commit -m "test(obf): pin SDK join-token fields, MEETING_FAIL codes, and URL token aliases"
```

---

### Task 2: Name the cross-account-without-token failure in the pure decision unit

Today a March-2026 rejection surfaces as whatever `classify_meeting_fail(63)` says — `MissingApproval`, whose guidance sends the operator to Marketplace app publication. That is the right message when a token WAS sent and Zoom still refused; it is the wrong message when no cross-account token was sent at all, where the fix is "paste the OBF token" and takes thirty seconds. The distinction needs exactly one bit the classifier doesn't have: did the join carry an on-behalf or app-privilege token? Keep the fix in `src/zoom-join-decision.h` — pure, SDK-free, already the single home for join error taxonomy (its own header comment says so) — and follow its existing id/guidance/classifier pattern exactly.

**Files:**
- Modify: `src/zoom-join-decision.h` (enum `ZoomJoinError` ~line 68, `join_error_id` ~84, `join_error_guidance` ~104, new classifier next to `classify_meeting_fail` ~188)
- Test: `tests/join-decision-test.cpp` (append checks)

**Interfaces:**
- Consumes: `zoom_join::classify_meeting_fail(int code)` (existing).
- Produces: `zoom_join::ZoomJoinError::CrossAccountTokenRequired`; `inline ZoomJoinError zoom_join::classify_meeting_fail_with_tokens(int code, bool had_cross_account_token)`. Task 3 wires both into `zoom_error_message()`.

- [ ] **Step 1: Write the failing test**

Append inside `main()` in `tests/join-decision-test.cpp`:

```cpp
    // --- March 2026 OBF policy: cross-account rejection without a token gets
    // --- its own named, actionable category (gap G1 in the 2026-08-29 plan).
    {
        using zoom_join::classify_meeting_fail_with_tokens;
        // No cross-account token sent: 63 and 504 mean "you needed one".
        check(classify_meeting_fail_with_tokens(63, false) ==
                  ZoomJoinError::CrossAccountTokenRequired,
              "obf: 63 without token -> cross_account_token_required");
        check(classify_meeting_fail_with_tokens(504, false) ==
                  ZoomJoinError::CrossAccountTokenRequired,
              "obf: 504 without token -> cross_account_token_required");
        // A token WAS sent: same codes keep their existing meaning.
        check(classify_meeting_fail_with_tokens(63, true) ==
                  ZoomJoinError::MissingApproval,
              "obf: 63 with token -> unchanged (missing approval)");
        check(classify_meeting_fail_with_tokens(505, false) ==
                  ZoomJoinError::OnBehalfTokenInvalid,
              "obf: token-shaped codes unaffected by the flag");
        check(std::string(zoom_join::join_error_id(
                  ZoomJoinError::CrossAccountTokenRequired)) ==
                  "cross_account_token_required",
              "obf: stable error id");
        check(std::string(zoom_join::join_error_guidance(
                  ZoomJoinError::CrossAccountTokenRequired)).find("app privilege") !=
                  std::string::npos,
              "obf: guidance names the app privilege token");
    }
```

- [ ] **Step 2: Run to verify it fails**

```sh
cmake --build build --config Release --parallel 8
```

Expected: **compile failure** in `join-decision-test.cpp` — `classify_meeting_fail_with_tokens` and `CrossAccountTokenRequired` do not exist yet.

- [ ] **Step 3: Minimal implementation**

In `src/zoom-join-decision.h` — add the enum value after `OnBehalfTokenInvalid` (line 79), and the id/guidance arms in the matching switches:

```cpp
    CrossAccountTokenRequired, // cross-account meeting, no OBF/on-behalf token sent
```

```cpp
    case ZoomJoinError::CrossAccountTokenRequired:
        return "cross_account_token_required";
```

```cpp
    case ZoomJoinError::CrossAccountTokenRequired:
        return "Zoom refused this join because the meeting is hosted on a "
               "different Zoom account and no app privilege (OBF) or on-behalf "
               "token was sent. Since March 2, 2026 Zoom requires one for "
               "cross-account joins. Get an app privilege token from the "
               "meeting's host account (Marketplace app or ISV flow), select "
               "'App privilege token' in the dock and paste it, or append "
               "?obf=<token> to the join URL, then retry.";
```

And the classifier, directly below `classify_meeting_fail()`:

```cpp
// March 2026 marketplace policy (not an SDK-header fact): joins into meetings
// hosted OUTSIDE this app's account are rejected server-side unless the join
// carried an OBF / on-behalf / app-privilege token. The rejection code is the
// SAME one an unapproved app gets (63; 504 for the anonymous variant), so the
// only way to tell "get approved" apart from "paste the token" is whether we
// sent one. The caller passes presence, never the token itself.
inline ZoomJoinError classify_meeting_fail_with_tokens(int code,
                                                       bool had_cross_account_token)
{
    if (!had_cross_account_token &&
        (code == 63 ||   // MEETING_FAIL_UNABLE_TO_JOIN_EXTERNAL_MEETING
         code == 504))   // MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING
        return ZoomJoinError::CrossAccountTokenRequired;
    return classify_meeting_fail(code);
}
```

- [ ] **Step 4: Run again**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: **64/64 green** (`CoreVideoJoinDecision` now includes the six new checks).

- [ ] **Step 5: Commit**

```sh
git add src/zoom-join-decision.h tests/join-decision-test.cpp
git commit -m "feat(obf): classify cross-account-without-token joins as their own named error"
```

---### Task 3: Surface the named error in the dock and control API

The classifier is useless until the wiring feeds it the token bit. `zoom_error_message()` (`src/zoom-engine-client.cpp:82`) is a static free function fed only the engine's JSON — it cannot know what the join carried, so `ZoomEngineClient::join()` must record presence (booleans only, mirroring the engine's `has_*` ack) and pass it through. This is exactly the wiring-vs-pure split that bit the talkback feature twice (CLAUDE.md, Task 5 fix rounds: "both Majors lived in wiring no test could reach"), so the presence-to-flag mapping itself goes through a tiny pure helper that a host test can drive.

**Files:**
- Modify: `src/zoom-engine-client.h` (member near the other join-state fields; helper declaration), `src/zoom-engine-client.cpp` (`join()` records presence; `zoom_error_message()` gains the flag parameter and consults `classify_meeting_fail_with_tokens` FIRST in its `meeting_failed` branch, before the hand-written per-code strings at lines 117-135)
- Modify: `src/zoom-join-decision.h` (pure helper `join_tokens_carry_cross_account_context`)
- Test: `tests/join-decision-test.cpp` (append)

**Interfaces:**
- Consumes: `ZoomJoinAuthTokens` (`src/zoom-types.h:25`), `bool ZoomEngineClient::join(const std::string &meeting_id, const std::string &passcode, const std::string &display_name, MeetingKind kind, const ZoomJoinAuthTokens &tokens)` (`src/zoom-engine-client.h:142-145`), Task 2's classifier.
- Produces: `inline bool zoom_join::join_tokens_carry_cross_account_context(bool has_on_behalf, bool has_app_privilege)`; `std::atomic<bool> ZoomEngineClient::m_last_join_cross_account_token`. The operator-facing message for `cross_account_token_required` flows through the existing error pipeline: `zoom_error_message()` feeds the same string the dock shows in its failure QMessageBox and the control API surfaces in its error events, so both surfaces get the named error from this one change.

- [ ] **Step 1: Write the failing test**

Append inside `main()` in `tests/join-decision-test.cpp`:

```cpp
    // --- G1 wiring rule: which token kinds satisfy the cross-account
    // --- requirement. A user ZAK is the SIGNED-IN account's token -- it
    // --- establishes user context but Zoom still rejects it for a foreign
    // --- account's meeting when app-level privilege is what's missing, and
    // --- the auto_zak path always sends one. Counting it would make the
    // --- named error unreachable on the exact path (sign-in + external
    // --- meeting) the policy is about.
    check(!zoom_join::join_tokens_carry_cross_account_context(false, false),
          "obf wiring: nothing sent -> no cross-account context");
    check(zoom_join::join_tokens_carry_cross_account_context(true, false),
          "obf wiring: on-behalf token counts");
    check(zoom_join::join_tokens_carry_cross_account_context(false, true),
          "obf wiring: app privilege token counts");
```

- [ ] **Step 2: Run to verify it fails**

```sh
cmake --build build --config Release --parallel 8
```

Expected: compile failure — `join_tokens_carry_cross_account_context` does not exist.

- [ ] **Step 3: Minimal implementation**

`src/zoom-join-decision.h`, next to `classify_meeting_fail_with_tokens()`:

```cpp
// Which token kinds count as "we sent cross-account context". Deliberately
// NOT the user ZAK: auto_zak sends one on every signed-in join, so counting
// it would route every real March-2026 rejection back to the generic
// approval message. Presence booleans only -- callers never pass values.
inline bool join_tokens_carry_cross_account_context(bool has_on_behalf,
                                                    bool has_app_privilege)
{
    return has_on_behalf || has_app_privilege;
}
```

`src/zoom-engine-client.h` — add near the other join bookkeeping members:

```cpp
    // Presence only, mirroring the engine's has_on_behalf_token /
    // has_app_privilege_token ack -- token VALUES never live here. Read by
    // zoom_error_message() wiring when a meeting_failed event arrives, which
    // is on the pipe reader thread while join() runs elsewhere: atomic.
    std::atomic<bool> m_last_join_cross_account_token{false};
```

`src/zoom-engine-client.cpp` — in `join()`, before writing the join command:

```cpp
    m_last_join_cross_account_token.store(
        zoom_join::join_tokens_carry_cross_account_context(
            !tokens.on_behalf_token.empty(),
            !tokens.app_privilege_token.empty()),
        std::memory_order_release);
```

`zoom_error_message()` becomes `static std::string zoom_error_message(const QJsonObject &obj, bool last_join_cross_account_token)` (update both call sites), and the TOP of its `meeting_failed` branch (line 116) — before the existing hand-written 63/504/505/506 strings, which stay as-is for the token-was-sent cases — gains:

```cpp
    if (msg == "meeting_failed") {
        const zoom_join::ZoomJoinError category =
            zoom_join::classify_meeting_fail_with_tokens(
                code, last_join_cross_account_token);
        if (category == zoom_join::ZoomJoinError::CrossAccountTokenRequired) {
            std::string out = zoom_join::join_error_guidance(category);
            out += " (code " + std::to_string(code) + ", id=";
            out += zoom_join::join_error_id(category);
            out += ")";
            return out;
        }
        // ... existing per-code messages unchanged below ...
```

- [ ] **Step 4: Run again**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: **64/64 green**, plugin compiles with the new parameter at every `zoom_error_message` call site.

- [ ] **Step 5: Commit**

```sh
git add src/zoom-join-decision.h src/zoom-engine-client.h src/zoom-engine-client.cpp tests/join-decision-test.cpp
git commit -m "feat(obf): surface cross_account_token_required through the engine-client error pipeline"
```

---

### Task 4: Persist the join token and token type, DPAPI-protected

An operator who pastes an OBF token today loses it on OBS restart (gap G2) — and for a standing weekly show on a client's Zoom account, re-obtaining the token every session is exactly the friction that gets a show run without one. The repo already made the storage-safety decision for us: OAuth access/refresh tokens are stored in `global.ini` DPAPI-encrypted via `protect_secret()`/`unprotect_secret()` (`src/zoom-settings.cpp:176/206`, entangled with the machine+user; macOS uses the Keychain, other platforms refuse loudly at lines 51-63). The join token is the same class of secret and uses the same mechanism; the token TYPE is not a secret and stores plain. What is testable without libobs's config API is the persistence DECISION — what to store and when to clear — so that lives in a pure header, following `src/talkback-dock-state.h`'s precedent for dock wiring.

**Files:**
- Create: `src/join-token-persistence.h`, `tests/join-token-persistence-test.cpp`
- Modify: `src/zoom-settings.h` (fields `join_token_type` / `join_token`), `src/zoom-settings.cpp` (load: `unprotect_secret(config_get_string(cfg, SECTION, "JoinToken"), "JoinToken")`, plain `JoinTokenType`; save: mirror lines 349-352's OAuth pattern with `protect_secret`), `src/zoom-dock.cpp` (seed combo+field from settings at construction ~753; persist via the decision helper in the existing save-on-successful-join block at 2754-2758), `CMakeLists.txt` (register test)
- Test: `tests/join-token-persistence-test.cpp`

**Interfaces:**
- Consumes: `ZoomPluginSettings::load()/save()`, `protect_secret`/`unprotect_secret` (file-static in zoom-settings.cpp — the new fields ride the existing save/load functions, no new linkage).
- Produces: `struct zoom_join::JoinTokenPersistDecision { std::string token_type; std::string token_value; }` and `inline JoinTokenPersistDecision join_token_persist_decision(const std::string &token_type, const std::string &typed_token)`.

- [ ] **Step 1: Write the failing test**

`tests/join-token-persistence-test.cpp`:

```cpp
// Pins the join-token persistence DECISION (gap G2, 2026-08-29 OBF plan).
// The rules the dock wiring cannot show: auto_zak mode must CLEAR any stored
// token (a stale OBF from last week's client silently riding along on an
// own-account join is a support nightmare), and a manual mode with an empty
// field keeps the stored token (the operator relying on persistence is the
// feature working, not a clear request).
#include "join-token-persistence.h"
#include <cstdio>
#include <string>

static int failures = 0;
static void check(bool cond, const std::string &name)
{
    if (!cond) { ++failures; std::printf("FAIL: %s\n", name.c_str()); }
}

int main()
{
    using zoom_join::join_token_persist_decision;
    {
        const auto d = join_token_persist_decision("app_privilege_token", "TOK_A");
        check(d.token_type == "app_privilege_token", "manual+typed: type stored");
        check(d.token_value == "TOK_A",              "manual+typed: value stored");
        check(d.keep_stored_value == false,          "manual+typed: overwrites");
    }
    {
        const auto d = join_token_persist_decision("user_zak", "");
        check(d.token_type == "user_zak",   "manual+empty: type stored");
        check(d.keep_stored_value == true,  "manual+empty: stored token kept");
    }
    {
        const auto d = join_token_persist_decision("auto_zak", "");
        check(d.token_type == "auto_zak",   "auto: type stored");
        check(d.token_value.empty(),        "auto: value cleared");
        check(d.keep_stored_value == false, "auto: stored token NOT kept");
    }
    {
        const auto d = join_token_persist_decision("", "ignored");
        check(d.token_type == "auto_zak",   "empty type: normalized to auto_zak");
        check(d.keep_stored_value == false, "empty type: clears like auto");
    }
    if (failures == 0) { std::printf("join-token-persistence: all checks passed\n"); return 0; }
    return 1;
}
```

CMakeLists.txt, next to the `CoreVideoJoinInput` block:

```cmake
    add_executable(CoreVideoJoinTokenPersistenceTest
        tests/join-token-persistence-test.cpp
    )
    target_include_directories(CoreVideoJoinTokenPersistenceTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME CoreVideoJoinTokenPersistence
             COMMAND CoreVideoJoinTokenPersistenceTest)
```

- [ ] **Step 2: Run to verify it fails**

```sh
cmake --build build --config Release --parallel 8
```

Expected: compile failure — `join-token-persistence.h` does not exist.

- [ ] **Step 3: Minimal implementation**

`src/join-token-persistence.h`:

```cpp
#pragma once
// Join-token persistence decision (gap G2, 2026-08-29 OBF plan). Pure and
// Qt/OBS-free so tests/join-token-persistence-test.cpp can pin the rules;
// the dock and ZoomPluginSettings only execute what this decides. The token
// VALUE passes through here on its way to DPAPI storage and must never be
// logged -- there is deliberately no formatter in this header.
#include <string>

namespace zoom_join {

struct JoinTokenPersistDecision {
    std::string token_type;         // normalized; always persisted (not a secret)
    std::string token_value;        // value to store when !keep_stored_value
    bool        keep_stored_value = false; // true: leave the stored secret alone
};

inline JoinTokenPersistDecision
join_token_persist_decision(const std::string &token_type,
                            const std::string &typed_token)
{
    JoinTokenPersistDecision d;
    d.token_type = token_type.empty() ? "auto_zak" : token_type;
    if (d.token_type == "auto_zak") {
        // Auto mode clears: a stale cross-account token must not survive a
        // return to own-account joins. token_value stays empty -> store "".
        return d;
    }
    if (typed_token.empty()) {
        d.keep_stored_value = true; // operator is USING persistence, not clearing
        return d;
    }
    d.token_value = typed_token;
    return d;
}

} // namespace zoom_join
```

Then the wiring (no new decisions, just execution): `ZoomPluginSettings` gains `std::string join_token_type = "auto_zak";` and `std::string join_token;` loaded/saved beside the OAuth tokens (`JoinToken` via `protect_secret`/`unprotect_secret` with account `"JoinToken"`, `JoinTokenType` plain); the dock constructor seeds `m_join_token_type` (find by `currentData`) and `m_join_token` from settings; the successful-join save block (`zoom-dock.cpp:2754-2758`) applies the decision:

```cpp
                const auto persist = zoom_join::join_token_persist_decision(
                    token_type.toStdString(), typed_token);
                saved.join_token_type = persist.token_type;
                if (!persist.keep_stored_value)
                    saved.join_token = persist.token_value;
                saved.save();
```

(`token_type` and `typed_token` are already captured at `zoom-dock.cpp:2564-2567`; capture them into the join thread's lambda alongside the existing `tokens`.)

- [ ] **Step 4: Run again**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: **65/65 green**.

- [ ] **Step 5: Commit**

```sh
git add src/join-token-persistence.h tests/join-token-persistence-test.cpp src/zoom-settings.h src/zoom-settings.cpp src/zoom-dock.cpp CMakeLists.txt
git commit -m "feat(obf): persist join token type + DPAPI-protected token across OBS restarts"
```

---

### Task 5: Operator documentation — README, wiki, CLAUDE.md

Gap G3: `README.md:343` documents only the automatic signed-in-ZAK flow; nothing tells an operator that joining a client's meeting after March 2, 2026 needs a token, where to get one, or that the dock combo / `?obf=` URL / control-API fields already accept it. Docs are part of done in this repo (standing directive), and the March deadline makes this the rare docs task with a compliance date attached.

**Files:**
- Modify: `README.md` (new subsection "Joining meetings on another Zoom account (OBF / app privilege tokens)" adjacent to the OAuth flow description at line 343), `wiki/Support.md` (troubleshooting entry keyed to the exact error id), `CLAUDE.md` (one paragraph in "Live testing" noting the control-API token fields and the `cross_account_token_required` id)
- Test: none (docs)

**Interfaces:**
- Consumes: the shipped behavior of Tasks 2-4 (error id `cross_account_token_required`, persistence semantics, existing URL aliases from A4).
- Produces: operator-facing prose only.

- [ ] **Step 1: Write the README subsection.** It must state, in this order: the March 2, 2026 Zoom policy (labelled as Zoom marketplace policy); that joins to meetings on the app's own account are unaffected; the three ways to supply a token (dock: select "App privilege token", paste — persisted DPAPI-encrypted until the type is switched back to "Zoom sign-in"; URL: `?obf=<token>` / `?zak=<zak>` / `?app_privilege_token=<token>`; control API: `"on_behalf_token"` / `"user_zak"` / `"app_privilege_token"` fields on `{"cmd":"join"}`); what the `cross_account_token_required` error means and its thirty-second fix; and that token values never appear in logs (presence booleans only).
- [ ] **Step 2: Write the wiki/Support.md entry.** Symptom-first: "Join fails with 'cross-account token required' (code 63/504)" → cause → the three supply paths → where the token comes from (the host account's Marketplace app / ISV flow — link Zoom's app privilege token docs).
- [ ] **Step 3: Update CLAUDE.md** "Live testing" with the token fields on `join` and the error id, per the keep-CLAUDE.md-current directive.
- [ ] **Step 4: Verify no token-shaped example strings look real** (use `<token>` placeholders in prose, never plausible base64), then commit:

```sh
git add README.md wiki/Support.md CLAUDE.md
git commit -m "docs(obf): operator instructions for March 2026 cross-account join tokens"
```

---

### Task 6: Live gate — cross-account join with and without a token

The classifier's central assumption — that Zoom's server answers a token-less cross-account join with code 63 (or 504) — is a hypothesis until a real Zoom server says so (A3). This repo's plans end in live gates for exactly this reason (milestone-1 plan, Task 6). This gate needs a meeting hosted on a Zoom account that is NOT the signed-in CoreVideo account, and an app privilege token minted for it; the owner must provide both. Record verdicts in a notes file; do not touch code in this task.

**Files:**
- Create: `docs/superpowers/notes/2026-08-29-obf-live-gate.md` (verdict record)
- Test: live, via the control API (TCP line-JSON, `127.0.0.1:19870`) and the dock

**Interfaces:**
- Consumes: everything shipped in Tasks 1-5.
- Produces: the recorded `MeetingFailCode` for the no-token case. **If it is not 63 or 504, reopen Task 2**: add the observed code to `classify_meeting_fail_with_tokens` with a comment citing this gate's log line, and re-run the suite.

- [ ] **Step 1: Leg A — no token, expect the named error.** With OBS running (never a second instance) and Zoom signed in, send over the control API, one JSON object per line:

```json
{"cmd":"join","meeting_id":"<external-account meeting URL>","display_name":"CoreVideo Gate"}
```

Expected: join is accepted (`ok:true` — the ZAK fetch succeeds; the rejection is server-side), then the meeting fails. Record from the OBS log: the engine's `{"cmd":"error","msg":"meeting_failed","code":<N>,"reason":"MEETING_FAIL_..."}` line, and the plugin's operator message — it MUST contain `id=cross_account_token_required`. Record N.
- [ ] **Step 2: Leg B — same meeting, token supplied.** Send:

```json
{"cmd":"join","meeting_id":"<same URL>","display_name":"CoreVideo Gate","app_privilege_token":"<token from host account>"}
{"cmd":"start_engine"}
{"cmd":"start_engine"}
{"cmd":"list_outputs"}
{"cmd":"leave"}
```

(`start_engine` twice — the second grants record rights, per CLAUDE.md.) Expected: the join reaches `MEETING_STATUS_INMEETING`, the engine ack shows `"has_app_privilege_token":true`, `list_outputs` returns a roster, and no log line anywhere contains the token value (grep the OBS log for the token's last 8 characters — zero hits required).
- [ ] **Step 3: Leg C — dock parity.** Repeat leg A from the dock Join button (the join watchdog only arms on the dock path — control-API joins never set `m_join_started_ms`, so leg A proved nothing about it): expect the failure QMessageBox to show the Task 2 guidance text, and after selecting "App privilege token" + pasting, a successful join; restart OBS and confirm the type and token survived (field populated, joins without re-pasting).
- [ ] **Step 4: Record and commit** the verdict file with: date, meeting account relationship, the observed fail code and reason string for leg A, pass/fail per leg, and the log-grep result for token leakage.

```sh
git add docs/superpowers/notes/2026-08-29-obf-live-gate.md
git commit -m "docs(obf): live gate verdicts for cross-account join with and without token"
```

**Gate rule:** this plan is not done until all three legs pass. A leg-A code other than 63/504 is not a failure of the gate — it is the gate doing its job; loop it back into Task 2 and re-run legs A and C.
