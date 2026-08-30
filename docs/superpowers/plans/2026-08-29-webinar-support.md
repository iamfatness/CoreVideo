# Zoom Webinar Support — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make CoreVideo join Zoom webinars deliberately — detecting webinar mode and our role, completing the register/screen-name handshakes a webinar join demands, offering promote/demote panelist controls, and refusing (with a named reason) every CoreVideo feature that cannot work for the role we hold — instead of joining one by accident and failing feature-by-feature in silence.

**Architecture:** The engine detects webinar mode via `IMeetingInfo::GetMeetingType()` and our role via `IUserInfo::GetUserRole()`, and reports both over E2P as a new `meeting_mode` event, latest-wins like `awaiting_admission`. All availability decisions (given mode + role → which CoreVideo features work, with named reasons) live in one pure header, `src/webinar-capability.h`, consumed by both processes and pinned by a host test. Promote/demote is a thin engine wrapper whose two SDK call lines are isolated in a separate TU compiled only when the full SDK drop supplies `meeting_webinar_ctrl_interface.h` — the same `EXISTS` gate CMakeLists.txt:1168 already uses for the talkback header.

**Tech Stack:** C++17, Zoom Windows Meeting SDK 7.1.5.43953, CMake + CTest, named-pipe line-JSON IPC, Qt6.

**Spec:** This document doubles as the spec. Requirements:

1. Joining a webinar URL/ID must reach `MEETING_STATUS_INMEETING` without a human dismissing an SDK dialog: the engine redirects the webinar register and screen-name prompts to itself (`RedirectWebinarNeedRegister` / `RedirectWebinarNameInputDialog`, meeting_configuration_interface.h:915/922) and answers them from the join command's own fields.
2. The plugin must know, and show, "this is a webinar, and you are attendee/panelist/host" — the dock and the control API both surface it.
3. Every gated feature refuses with a **named reason**, never silently: per-participant raw video subscription, per-participant/ISO audio, talkback, and the Active Speaker director are all panelist-or-better in a webinar. The raw-data receive path rides the local-recording privilege, which a webinar **attendee** cannot hold — attendees have no per-participant raw streams at all.
4. Promote/demote panelist by **display name** (host/co-host only), reported with the SDK's own error code.
5. Webinar attendees have **no waiting room**: the pre-broadcast state is the practice session/backstage (`IUserInfo::IsInWebinarBackstage()`, meeting_participants_ctrl_interface.h:274, is the signal). The waiting-room-shaped machinery (`awaiting_admission`, the join-watchdog exemption in `src/join-watchdog.h`) must not misread backstage as a waiting room, and nothing may attempt waiting-room admission of webinar attendees.
6. Talkback in a webinar is **unverified**: the tracked SDK tree says nothing about talkback×webinar interaction. It fails CLOSED (`webinar` reason) until the Task 6 live gate proves otherwise — a to-verify, not an assumption.

**SDK ground truth (hard constraint).** The tracked vendored SDK tree (`third_party/zoom-sdk/h`) has **no** `meeting_webinar_ctrl_interface.h`. It has only the forward declaration `class IMeetingWebinarController;` (meeting_service_interface.h:957) and `IMeetingService::GetMeetingWebinarController()` (meeting_service_interface.h:1148). So `IMeetingWebinarController`'s **definition** — including the promote/demote methods (named approximately `PromptePanelist2Attendee` / `DepromptAttendee2Panelist`, Zoom's own spellings, which must be **copied from the full local SDK drop's `meeting_webinar_ctrl_interface.h` at implementation time, never guessed**) — is unavailable to any TU this repo's CI compiles. The repo already has this exact situation and its answer: CMakeLists.txt:1168 gates `CoreVideoEngineTalkbackSelectTest` on `EXISTS "${ZOOM_SDK_INCLUDE_DIR}/meeting_service_components/meeting_talkback_ctrl_interface.h"`, because the full local SDK drop supplies headers the tracked tree lacks. Task 5 uses the identical gate, and is structured so everything except the two SDK call lines compiles and tests against the tracked tree.

What the tracked tree **does** verifiably have (each read for this plan):

- meeting_service_interface.h — `MEETING_TYPE_NONE/NORMAL/WEBINAR/BREAKOUTROOM` (:185-191), `IMeetingInfo::GetMeetingType()` (:703, on `IMeetingInfo`, class at :672, reached via `IMeetingService::GetMeetingInfo()` :1061), `GetMeetingWebinarController()` (:1148), `GetMeetingConfiguration()` (:1185).
- meeting_participants_ctrl_interface.h — `enum UserRole` with `USERROLE_PANELIST` / `USERROLE_ATTENDEE` (:18-33), `WebinarAttendeeStatus { bool allow_talk; }` (:38-46), `IUserInfo::GetUserRole()` (:164), `GetWebinarAttendeeStatus()` (:206), `IsInWebinarBackstage()` (:274), `GetMySelfUser()` (:526), `LowerAllHands(bool forWebinarAttendees)` (:570).
- meeting_configuration_interface.h — `IWebinarNeedRegisterHandler` + ByUrl/ByEmail subclasses (:95-166, `InputWebinarRegisterEmailAndScreenName` :157), `IWebinarInputScreenNameHandler` (:172-191, `InputName` :181, `Cancel` :188), `onWebinarNeedRegisterNotification` (:375), `onWebinarNeedInputScreenName` (:387), `RedirectWebinarNeedRegister` (:915), `RedirectWebinarNameInputDialog` (:922), `PrePopulateWebinarRegistrationInfo` (:1002), `IMeetingConfiguration::SetEvent` (:1033).

Grepped the whole repo for `webinar` (case-insensitive) outside `third_party/`: **zero hits**. There is no existing webinar handling to preserve or migrate.

## Global Constraints

- Build: `cmake --build build --config Release --parallel 8`. Test: `cd build && ctest -C Release --output-on-failure` — must be N/N green after every task (each task states its expected delta; the suite must never shrink).
- Tests are plain executables, no framework, `check()`-style, one file per invariant cluster in `tests/`, registered in `CMakeLists.txt` with `add_executable` + `add_test`.
- Commands are routed by **exact match** on the declared `cmd` field (`src/engine-command.h`), pinned in `tests/engine-command-test.cpp` — never substrings.
- Participants are addressed **by display name**, resolved to a meeting-scoped `userID` at use time. Never persist a raw Zoom user ID.
- Comments state the constraint the code cannot show; when a change is motivated by a live failure, say what happened, with numbers.
- SDK headers missing from the tracked tree are gated with `EXISTS` on `${ZOOM_SDK_INCLUDE_DIR}` per the talkback precedent (CMakeLists.txt:1168) — never vendored from the full drop, never stubbed with invented declarations.
- Never run a second OBS instance while one is testing (pipe/SDK singleton collision, crash loop). Send `{"cmd":"leave"}` before closing OBS.
- An engine older than this feature emits no `meeting_mode` event, and a DLL-only install is this project's canonical mistake — every plugin-side default must therefore behave exactly as today (meeting semantics), with webinar gating engaging only on positive evidence.

---

### Task 1: The capability verdict — one pure header

Every feature this plan gates lives in a different file owned by a different subsystem (`src/speaker-director.cpp`, the talkback controller, `src/zoom-iso-recorder.cpp`, the outputs). Scattering "if webinar and attendee then refuse" across all of them is how the talkback feature shipped two Majors in wiring no test could reach — so the decision is made in exactly one Qt/OBS/SDK-free header, the same reason `src/talkback-plan.h` and `src/zoom-join-decision.h` exist, and everything else only asks it.

**Files:**
- Create: `src/webinar-capability.h`
- Test: `tests/webinar-capability-test.cpp` (new)
- Modify: `CMakeLists.txt` (register the test after the `CoreVideoEngineTalkbackSelect` block, CMakeLists.txt:1185)

**Interfaces:**
- Consumes: nothing.
- Produces: `enum class MeetingMode { Unknown, Meeting, Webinar }`; `enum class LocalRole { Unknown, Host, CoHost, Panelist, Attendee, BreakoutModerator }`; `enum class CoreVideoFeature { RawVideoSubscribe, PerParticipantAudio, IsoRecord, Talkback, SpeakerDirector, WaitingRoomAdmission, PromoteDemote }`; `struct FeatureVerdict { bool available; const char *reason; }`; `FeatureVerdict webinar_feature_verdict(MeetingMode, LocalRole, CoreVideoFeature)`; `LocalRole local_role_from_id(const std::string&)`; `const char *local_role_id(LocalRole)`. Tasks 2, 4 and 5 consume all of these.

- [ ] **Step 1: Write the failing test**

Create `tests/webinar-capability-test.cpp`:

```cpp
// tests/webinar-capability-test.cpp
// Pins the mode+role -> feature table. The reasons are load-bearing: they are
// what the operator sees instead of a silent failure, so a reason changing or
// emptying is a regression, not a refactor.
#include "webinar-capability.h"

#include <iostream>
#include <string>

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
    // --- An ordinary meeting gates nothing, for any role ---
    for (auto role : {LocalRole::Host, LocalRole::Attendee, LocalRole::Unknown}) {
        for (auto f : {CoreVideoFeature::RawVideoSubscribe, CoreVideoFeature::Talkback,
                       CoreVideoFeature::SpeakerDirector, CoreVideoFeature::IsoRecord}) {
            const auto v = webinar_feature_verdict(MeetingMode::Meeting, role, f);
            check(v.available, "a plain meeting gated a feature");
            check(std::string(v.reason).empty(), "an available verdict carried a reason");
        }
    }

    // --- Unknown mode = pre-webinar engine (DLL-only install): behave as today ---
    check(webinar_feature_verdict(MeetingMode::Unknown, LocalRole::Unknown,
                                  CoreVideoFeature::RawVideoSubscribe).available,
          "Unknown mode gated raw video -- an old engine would regress every meeting");

    // --- Webinar attendee: no raw media of any kind ---
    for (auto f : {CoreVideoFeature::RawVideoSubscribe,
                   CoreVideoFeature::PerParticipantAudio,
                   CoreVideoFeature::IsoRecord}) {
        const auto v = webinar_feature_verdict(MeetingMode::Webinar, LocalRole::Attendee, f);
        check(!v.available, "a webinar attendee was offered raw media");
        check(std::string(v.reason) == "webinar_attendee_no_raw_media",
              "attendee raw-media refusal reason wrong");
    }
    // --- Webinar panelist: raw media allowed (privilege path verified live in Task 6) ---
    check(webinar_feature_verdict(MeetingMode::Webinar, LocalRole::Panelist,
                                  CoreVideoFeature::RawVideoSubscribe).available,
          "a webinar panelist was refused raw video");

    // --- Talkback fails CLOSED in any webinar until the live gate proves it ---
    const auto tb = webinar_feature_verdict(MeetingMode::Webinar, LocalRole::Host,
                                            CoreVideoFeature::Talkback);
    check(!tb.available, "talkback offered in a webinar before the live gate verified it");
    check(std::string(tb.reason) == "talkback_unverified_in_webinar",
          "talkback webinar refusal reason wrong");

    // --- Director cannot direct as an attendee; waiting-room admission never exists ---
    check(!webinar_feature_verdict(MeetingMode::Webinar, LocalRole::Attendee,
                                   CoreVideoFeature::SpeakerDirector).available,
          "attendee was offered the speaker director");
    check(std::string(webinar_feature_verdict(MeetingMode::Webinar, LocalRole::Host,
              CoreVideoFeature::WaitingRoomAdmission).reason) == "webinar_has_no_waiting_room",
          "webinar waiting-room refusal reason wrong");

    // --- Promote/demote: host and co-host only ---
    check(webinar_feature_verdict(MeetingMode::Webinar, LocalRole::CoHost,
                                  CoreVideoFeature::PromoteDemote).available,
          "co-host was refused promote/demote");
    check(std::string(webinar_feature_verdict(MeetingMode::Webinar, LocalRole::Panelist,
              CoreVideoFeature::PromoteDemote).reason) == "promote_requires_host_or_cohost",
          "panelist promote refusal reason wrong");
    check(std::string(webinar_feature_verdict(MeetingMode::Meeting, LocalRole::Host,
              CoreVideoFeature::PromoteDemote).reason) == "not_a_webinar",
          "promote in a plain meeting must refuse with not_a_webinar");

    // --- Role ids round-trip; an unknown id is Unknown, never a crash ---
    for (auto r : {LocalRole::Host, LocalRole::CoHost, LocalRole::Panelist,
                   LocalRole::Attendee, LocalRole::BreakoutModerator}) {
        check(local_role_from_id(local_role_id(r)) == r, "role id did not round-trip");
    }
    check(local_role_from_id("someday_a_new_role") == LocalRole::Unknown,
          "an unrecognised role id did not map to Unknown");

    if (failures == 0)
        std::cout << "webinar-capability: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Register in `CMakeLists.txt` after the `CoreVideoEngineTalkbackSelect` block (after CMakeLists.txt:1185):

```cmake
    # The mode+role -> feature table for webinars. One pure header, because the
    # talkback feature's two Majors both lived in wiring spread across files
    # that no host test could reach. See src/webinar-capability.h.
    add_executable(CoreVideoWebinarCapabilityTest
        tests/webinar-capability-test.cpp
    )
    target_include_directories(CoreVideoWebinarCapabilityTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoWebinarCapability
             COMMAND CoreVideoWebinarCapabilityTest)
```

```sh
cmake -S . -B build && cmake --build build --config Release --target CoreVideoWebinarCapabilityTest --parallel 8
```

Expected: FAIL — `Cannot open include file: 'webinar-capability.h'`.

- [ ] **Step 3: Write minimal implementation**

Create `src/webinar-capability.h`:

```cpp
#pragma once
//
// webinar-capability.h — given the meeting mode and our role, which CoreVideo
// features exist, with a NAMED reason when they do not.
//
// Webinars are not meetings with more people. An attendee holds no raw-data
// streams at all (raw receive rides the local-recording privilege, which only
// panelist-and-better can hold), webinars have no waiting room (the
// pre-broadcast state is the practice session / backstage), and the tracked
// SDK tree says nothing about talkback inside a webinar — so talkback fails
// CLOSED here until the live gate (Task 6 of the 2026-08-29 webinar plan)
// proves it, exactly as the talkback probe treated its own entitlement.
//
// MeetingMode::Unknown behaves like Meeting on purpose: an engine older than
// this feature never emits meeting_mode, and a DLL-only install is this
// project's canonical mistake — gating on Unknown would regress every
// ordinary meeting behind a half-applied install.
//
// Free of Qt / OBS / Zoom SDK dependencies so the whole table is pinned by
// tests/webinar-capability-test.cpp with no engine and no meeting.
//
#include <string>

enum class MeetingMode { Unknown, Meeting, Webinar };
enum class LocalRole { Unknown, Host, CoHost, Panelist, Attendee, BreakoutModerator };

enum class CoreVideoFeature {
    RawVideoSubscribe,    // per-participant raw video (zoom-source outputs)
    PerParticipantAudio,  // isolated / audience audio sources
    IsoRecord,            // per-source ISO recording (needs raw A+V)
    Talkback,             // nominate / key
    SpeakerDirector,      // Active Speaker director
    WaitingRoomAdmission, // anything waiting-room shaped
    PromoteDemote,        // webinar promote/demote panelist controls
};

struct FeatureVerdict {
    bool available;
    const char *reason; // "" when available; a stable id otherwise, never prose
};

// Wire ids for the role, matching what the engine's meeting_mode event emits.
inline const char *local_role_id(LocalRole r)
{
    switch (r) {
    case LocalRole::Host:              return "host";
    case LocalRole::CoHost:            return "cohost";
    case LocalRole::Panelist:          return "panelist";
    case LocalRole::Attendee:          return "attendee";
    case LocalRole::BreakoutModerator: return "breakout_moderator";
    case LocalRole::Unknown:           return "unknown";
    }
    return "unknown";
}

inline LocalRole local_role_from_id(const std::string &id)
{
    if (id == "host")               return LocalRole::Host;
    if (id == "cohost")             return LocalRole::CoHost;
    if (id == "panelist")           return LocalRole::Panelist;
    if (id == "attendee")           return LocalRole::Attendee;
    if (id == "breakout_moderator") return LocalRole::BreakoutModerator;
    return LocalRole::Unknown;
}

inline FeatureVerdict webinar_feature_verdict(MeetingMode mode, LocalRole role,
                                              CoreVideoFeature feature)
{
    // Promote/demote is the one feature that exists ONLY in a webinar.
    if (feature == CoreVideoFeature::PromoteDemote) {
        if (mode != MeetingMode::Webinar)
            return {false, "not_a_webinar"};
        if (role == LocalRole::Host || role == LocalRole::CoHost)
            return {true, ""};
        return {false, "promote_requires_host_or_cohost"};
    }

    // Unknown = pre-webinar engine: behave exactly as today (see header).
    if (mode != MeetingMode::Webinar)
        return {true, ""};

    switch (feature) {
    case CoreVideoFeature::RawVideoSubscribe:
    case CoreVideoFeature::PerParticipantAudio:
    case CoreVideoFeature::IsoRecord:
        // Raw receive rides the local-recording privilege; an attendee cannot
        // hold it and has no per-participant streams to subscribe to anyway.
        if (role == LocalRole::Attendee || role == LocalRole::Unknown)
            return {false, "webinar_attendee_no_raw_media"};
        return {true, ""};
    case CoreVideoFeature::Talkback:
        // Fails closed for EVERY role: the tracked SDK tree is silent on
        // talkback x webinar. Flip only on a recorded Task 6 live pass.
        return {false, "talkback_unverified_in_webinar"};
    case CoreVideoFeature::SpeakerDirector:
        // Directing needs the raw streams it cuts between.
        if (role == LocalRole::Attendee || role == LocalRole::Unknown)
            return {false, "webinar_attendee_no_raw_media"};
        return {true, ""};
    case CoreVideoFeature::WaitingRoomAdmission:
        // Webinar attendees have no waiting room; the pre-broadcast state is
        // backstage (IUserInfo::IsInWebinarBackstage), which is not ours to admit.
        return {false, "webinar_has_no_waiting_room"};
    case CoreVideoFeature::PromoteDemote:
        break; // handled above
    }
    return {false, "unknown_feature"};
}
```

- [ ] **Step 4: Run test to verify it passes**

```sh
cmake --build build --config Release --target CoreVideoWebinarCapabilityTest --parallel 8
cd build && ctest -C Release -R CoreVideoWebinarCapability --output-on-failure
```

Expected: PASS, `webinar-capability: all tests passed`.

- [ ] **Step 5: Commit**

```sh
git add src/webinar-capability.h tests/webinar-capability-test.cpp CMakeLists.txt
git commit -m "feat(webinar): mode+role capability table with named refusal reasons"
```

---

### Task 2: Engine reports `meeting_mode` — webinar flag, role, backstage

The plugin cannot ask the SDK anything; the engine must tell it. The precedent is `awaiting_admission` (engine/src/main.cpp:998-1005): sent on every relevant change, carrying the full state, so neither side tracks edges. `meeting_mode` follows it exactly — emitted on `MEETING_STATUS_INMEETING` and again on every roster change (roles change via promote/demote and host handoff, and `EngineParticipants::roster_changed()` at engine/src/main.cpp:608 already fires on all five roster callbacks plus `onHostChangeNotification`, meeting_participants_ctrl_interface.h:351).

**Files:**
- Modify: `engine/src/main.cpp` — add `report_meeting_mode()` to `EngineParticipants` (near `send_roster()`, :640), call it from `roster_changed()` (:608) and from `EngineMeetingEvent`'s `MEETING_STATUS_INMEETING` branch (:1008, which already reaches `m_participants`).

**Interfaces:**
- Consumes: `local_role_id` from Task 1 (`src/webinar-capability.h`; `engine/src/` TUs already include `src/` headers — talkback-plan.h precedent).
- Produces: E2P event `{"cmd":"meeting_mode","webinar":<bool>,"role":"<id>","backstage":<bool>}`. Task 4 parses it.

- [ ] **Step 1: State the check**

No unit test — this member is SDK-bound (it reads `IMeetingInfo` and `IUserInfo` live), the same explicit statement Task 3 of the talkback Milestone 1 plan made. The compile is the check here; the observable behaviour is pinned by Task 4's parse test and verified live in Task 6.

- [ ] **Step 2: Write the implementation**

In `engine/src/main.cpp`, inside `EngineParticipants` next to `send_roster()` (:640), add:

```cpp
    // Mirrors awaiting_admission (see onMeetingStatusChanged): full state on
    // every send, so neither side tracks edges. Sent from roster_changed()
    // because promote/demote and host handoff arrive as roster/host callbacks,
    // and from INMEETING so the plugin learns the mode before the first roster.
    void report_meeting_mode()
    {
        if (!m_meeting_svc_ptr || !*m_meeting_svc_ptr) return;
        ZOOMSDK::IMeetingService *svc = *m_meeting_svc_ptr;

        bool webinar = false;
        if (ZOOMSDK::IMeetingInfo *info = svc->GetMeetingInfo())
            webinar = info->GetMeetingType() == ZOOMSDK::MEETING_TYPE_WEBINAR;

        LocalRole role = LocalRole::Unknown;
        bool backstage = false;
        if (auto *pc = svc->GetMeetingParticipantsController()) {
            if (ZOOMSDK::IUserInfo *self = pc->GetMySelfUser()) {
                switch (self->GetUserRole()) {
                case ZOOMSDK::USERROLE_HOST:     role = LocalRole::Host; break;
                case ZOOMSDK::USERROLE_COHOST:   role = LocalRole::CoHost; break;
                case ZOOMSDK::USERROLE_PANELIST: role = LocalRole::Panelist; break;
                case ZOOMSDK::USERROLE_ATTENDEE: role = LocalRole::Attendee; break;
                case ZOOMSDK::USERROLE_BREAKOUTROOM_MODERATOR:
                    role = LocalRole::BreakoutModerator; break;
                default:                         role = LocalRole::Unknown; break;
                }
                // Only meaningful in a webinar; false elsewhere by the SDK's
                // own contract, so no mode check is needed to send it.
                backstage = self->IsInWebinarBackstage();
            }
        }
        EngineIpc::write(std::string(R"({"cmd":"meeting_mode","webinar":)") +
            (webinar ? "true" : "false") +
            R"(,"role":")" + local_role_id(role) +
            R"(","backstage":)" + (backstage ? "true" : "false") + "}");
    }
```

Add `#include "webinar-capability.h"` beside main.cpp's other `src/` includes. In `roster_changed()` (:608), add `report_meeting_mode();` after `send_roster();`. In `onMeetingStatusChanged`'s `MEETING_STATUS_INMEETING` branch (:1008), after the `m_participants->attach(...)` call, add:

```cpp
            if (m_participants)
                m_participants->report_meeting_mode();
```

- [ ] **Step 3: Build and run the full suite**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: engine links clean; suite green — this task adds no tests but must break none.

- [ ] **Step 4: Commit**

```sh
git add engine/src/main.cpp
git commit -m "feat(webinar): engine reports meeting_mode (webinar flag, local role, backstage)"
```

---

### Task 3: Webinar join handshakes — register and screen-name redirects

A headless engine joining a registration-required webinar today would hit an SDK dialog no one can click. `IMeetingConfiguration` (acquired via `GetMeetingConfiguration()`, meeting_service_interface.h:1185 — the engine currently never acquires it; grep confirms zero call sites) can redirect both prompts to callbacks we answer from the join command's own fields. The ByUrl registration variant cannot be answered headlessly at all — the honest move is to report the URL over E2P and cancel, so the operator registers in a browser and rejoins, instead of hanging the join silently.

**Files:**
- Modify: `engine/src/main.cpp` — new `EngineConfigEvent : public ZOOMSDK::IMeetingConfigurationEvent` beside `EngineMeetingEvent` (:739); acquisition + redirects in the `IpcCommand::Join` branch (:1440-1498); a new optional join field `registration_email`.
- Modify: `src/zoom-control-server.cpp` (:719 join command) and `src/zoom-engine-client.cpp`'s `join()` — pass `registration_email` through when supplied.

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: E2P events `{"cmd":"webinar_join","stage":"register_by_email"|"register_by_url"|"screen_name","code":<int>}` and `{"cmd":"webinar_register_url","url":"..."}`. Join command gains optional `"registration_email"`.

- [ ] **Step 1: State the check**

No unit test — SDK-bound (every line touches `IMeetingConfiguration` or a handler object the SDK owns and destroys after one call, per meeting_configuration_interface.h:179/186). The compile is the check; Task 6 Step 3 exercises both handshakes live. This is the same explicit statement the talkback plan's Tasks 3/4 made.

- [ ] **Step 2: Write the implementation**

In `engine/src/main.cpp`, beside `EngineMeetingEvent` (:739):

```cpp
// Answers the two webinar join prompts the SDK would otherwise raise as
// dialogs a headless engine can never dismiss. IMeetingConfigurationEvent has
// many pure virtuals beyond these two (it also inherits
// IMeetingConfigurationFreeMeetingEvent); every one not shown here is
// overridden with an empty body — the compiler enumerates the full list, and
// inventing behaviour for prompts we have never seen live would be guessing.
class EngineConfigEvent : public ZOOMSDK::IMeetingConfigurationEvent {
public:
    // The join command's display name / registration email, captured before
    // Join() because the SDK fires these callbacks mid-join.
    void set_join_identity(std::string display_name, std::string email)
    {
        m_display_name = std::move(display_name);
        m_email = std::move(email);
    }

    void onWebinarNeedRegisterNotification(
        ZOOMSDK::IWebinarNeedRegisterHandler *handler) override
    {
        if (!handler) return;
        if (handler->GetWebinarNeedRegisterType() ==
            ZOOMSDK::IWebinarNeedRegisterHandler::WebinarReg_By_Email_and_DisplayName) {
            const std::wstring wemail(m_email.begin(), m_email.end());
            const std::wstring wname(m_display_name.begin(), m_display_name.end());
            auto *by_email =
                static_cast<ZOOMSDK::IWebinarNeedRegisterHandlerByEmail *>(handler);
            const ZOOMSDK::SDKError e = by_email->InputWebinarRegisterEmailAndScreenName(
                wemail.c_str(), wname.c_str());
            EngineIpc::write(
                R"({"cmd":"webinar_join","stage":"register_by_email","code":)" +
                std::to_string(static_cast<int>(e)) + "}");
            return;
        }
        // ByUrl cannot be completed headlessly: report the URL so the operator
        // can register in a browser, then cancel the join rather than hang it.
        auto *by_url = static_cast<ZOOMSDK::IWebinarNeedRegisterHandlerByUrl *>(handler);
        EngineIpc::write(R"({"cmd":"webinar_register_url","url":")" +
            json_escape(zchar_to_utf8(by_url->GetWebinarRegisterUrl())) + R"("})");
        by_url->Cancel();
        EngineIpc::write(
            R"({"cmd":"webinar_join","stage":"register_by_url","code":-1})");
    }

    void onWebinarNeedInputScreenName(
        ZOOMSDK::IWebinarInputScreenNameHandler *handler) override
    {
        if (!handler) return;
        const std::wstring wname(m_display_name.begin(), m_display_name.end());
        const ZOOMSDK::SDKError e = handler->InputName(wname.c_str());
        EngineIpc::write(R"({"cmd":"webinar_join","stage":"screen_name","code":)" +
            std::to_string(static_cast<int>(e)) + "}");
    }

    // ... every remaining pure virtual of IMeetingConfigurationEvent (and its
    // IMeetingConfigurationFreeMeetingEvent base): empty override bodies ...

private:
    std::string m_display_name;
    std::string m_email;
};
```

(The `std::wstring(begin, end)` widening matches ASCII-only email/name; use main.cpp's existing `to_zstr()` helper instead if it is visible at this point in the file — it is, and it is the correct call: the sketch above is the fallback shape, `to_zstr` the required one.)

In the `IpcCommand::Join` branch, after `CreateMeetingService` / `SetEvent` (:1456-1458) and before `Join(jp)` (:1495):

```cpp
                const std::string registration_email = json_str(line, "registration_email");
                config_event.set_join_identity(display_name, registration_email);
                if (ZOOMSDK::IMeetingConfiguration *cfg =
                        meeting_svc->GetMeetingConfiguration()) {
                    cfg->SetEvent(&config_event);
                    cfg->RedirectWebinarNeedRegister(true);
                    cfg->RedirectWebinarNameInputDialog(true);
                    if (!registration_email.empty())
                        cfg->PrePopulateWebinarRegistrationInfo(
                            to_zstr(registration_email).c_str(),
                            g_wide_name.c_str());
                }
```

with `static EngineConfigEvent config_event;` declared beside the other long-lived engine objects (near :1290). Note `PrePopulateWebinarRegistrationInfo` takes raw pointers the SDK may hold across the async join — pass `g_wide_name.c_str()` and store the widened email in a new persistent `g_wide_reg_email` beside the other `g_wide_*` variables (:1312-1328), not a temporary.

Plugin side: `ZoomEngineClient::join()` and the control-server `join` handler (:719) forward an optional `registration_email` string field verbatim, defaulting to absent; `src/zoom-dock.cpp` is untouched this task (the dock gains nothing until an operator asks — control-API-first, like `talkback_probe`).

- [ ] **Step 3: Build and run the full suite**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: green, no delta.

- [ ] **Step 4: Commit**

```sh
git add engine/src/main.cpp src/zoom-engine-client.h src/zoom-engine-client.cpp src/zoom-control-server.cpp
git commit -m "feat(webinar): answer register/screen-name join prompts headlessly"
```

---

### Task 4: Plugin state + feature gating

The plugin learns the mode from `meeting_mode`, exposes it, and every gated surface asks `webinar_feature_verdict()` before acting. The refusal points are: the talkback controller's `key_on()`/nominate (already refusal-shaped — add one more named reason), the speaker-director enable path in `src/zoom-dock.cpp`'s 100 ms tick, and the control server's `talkback_nominate`/`assign_output` handlers. Raw-media subscription itself is NOT pre-refused plugin-side: for a panelist it must work, and for an attendee the engine's existing `video_subscribe_failed` path already reports per-source — the plugin adds the *explanation* (the verdict reason) to the dock status instead of a second gate that could false-refuse on a stale role.

**Files:**
- Modify: `src/zoom-engine-client.h` (near `is_awaiting_admission()`, :260), `src/zoom-engine-client.cpp` (`handle_event`, beside the `awaiting_admission` branch at :1440)
- Modify: `src/zoom-dock.cpp` (mode badge + director gating), `src/zoom-control-server.cpp` (refusals), talkback controller (`key_on` refusal)
- Test: `tests/webinar-capability-test.cpp` (extend — the gating decisions are already pure; this task adds the state-holder round-trip)

**Interfaces:**
- Consumes: `meeting_mode` event (Task 2), `webinar_feature_verdict` / `local_role_from_id` (Task 1).
- Produces: `MeetingMode ZoomEngineClient::meeting_mode() const`, `LocalRole ZoomEngineClient::local_role() const`, `bool ZoomEngineClient::is_webinar_backstage() const`. `talkback_status` and refusals gain the verdict reasons.

- [ ] **Step 1: Write the failing test**

The event→state mapping must be pure so a host test can reach it (the talkback feature's N5 lesson: wiring inlined in `handle_event` is untestable). Append to `tests/webinar-capability-test.cpp`, before the final `if (failures == 0)`:

```cpp
    // --- The wire mapping: what handle_event stores from a meeting_mode line ---
    {
        WebinarState st; // defaults must equal "old engine": Unknown/Unknown/false
        check(st.mode == MeetingMode::Unknown && st.role == LocalRole::Unknown &&
              !st.backstage, "WebinarState default is not the old-engine default");
        webinar_state_apply(st, /*webinar=*/true, /*role_id=*/"attendee",
                            /*backstage=*/true);
        check(st.mode == MeetingMode::Webinar && st.role == LocalRole::Attendee &&
              st.backstage, "meeting_mode apply did not store webinar/attendee/backstage");
        webinar_state_apply(st, false, "host", false);
        check(st.mode == MeetingMode::Meeting && st.role == LocalRole::Host,
              "meeting_mode apply did not overwrite latest-wins");
        webinar_state_reset(st);
        check(st.mode == MeetingMode::Unknown,
              "reset did not return to the old-engine default -- a stale Webinar "
              "verdict would gate the NEXT, ordinary meeting");
    }
```

- [ ] **Step 2: Run test to verify it fails**

```sh
cmake --build build --config Release --target CoreVideoWebinarCapabilityTest --parallel 8
```

Expected: FAIL to compile — `'WebinarState' was not declared`.

- [ ] **Step 3: Write minimal implementation**

Append to `src/webinar-capability.h`:

```cpp
// The plugin-side holder for the engine's meeting_mode event. Latest-wins,
// like awaiting_admission. reset() returns to the OLD-ENGINE default
// (Unknown), and is wired into the same three world-reset points the talkback
// nomination record uses (handle_event's "left" branch, stop_for_reconnect(),
// start() after its joins): a Webinar verdict surviving into the next meeting
// would gate features in an ordinary meeting.
struct WebinarState {
    MeetingMode mode = MeetingMode::Unknown;
    LocalRole role = LocalRole::Unknown;
    bool backstage = false;
};

inline void webinar_state_apply(WebinarState &st, bool webinar,
                                const std::string &role_id, bool backstage)
{
    st.mode = webinar ? MeetingMode::Webinar : MeetingMode::Meeting;
    st.role = local_role_from_id(role_id);
    st.backstage = backstage;
}

inline void webinar_state_reset(WebinarState &st) { st = WebinarState{}; }
```

Then wire it (Qt-bound, pinned only through the pure functions above):

- `src/zoom-engine-client.h`: `WebinarState m_webinar_state; mutable std::mutex` reuse `m_mtx`; accessors `meeting_mode()` / `local_role()` / `is_webinar_backstage()` reading under the lock, declared beside `is_awaiting_admission()` (:260).
- `src/zoom-engine-client.cpp` `handle_event`, beside the `awaiting_admission` branch (:1440):

```cpp
    if (cmd == "meeting_mode") {
        std::lock_guard<std::mutex> lk(m_mtx);
        webinar_state_apply(m_webinar_state, obj.value("webinar").toBool(),
                            obj.value("role").toString().toStdString(),
                            obj.value("backstage").toBool());
        return;
    }
```

  plus `webinar_state_reset(m_webinar_state)` in the `"left"` branch (:1453), in `stop_for_reconnect()`, and in `start()` after its thread joins — the three world-reset points, verbatim per the talkback N2/N7 history. Also `blog(LOG_INFO, ...)` any `webinar_join` / `webinar_register_url` line verbatim, exactly as `talkback_probe` lines are logged (:1351).
- Gating call sites, each a two-liner asking the verdict and surfacing `v.reason`: the talkback controller's `key_on()` and `ZoomControlServer`'s `talkback_nominate` refuse when `webinar_feature_verdict(mode, role, CoreVideoFeature::Talkback)` is unavailable; `src/zoom-dock.cpp`'s director group disables with the reason as tooltip when `SpeakerDirector` is unavailable; the dock header shows `Webinar — Attendee (limited)` from `meeting_mode()`/`local_role()` whenever mode is `Webinar`.

- [ ] **Step 4: Run test to verify it passes, then the full suite**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: PASS (`webinar-capability: all tests passed`), suite green.

- [ ] **Step 5: Commit**

```sh
git add src/webinar-capability.h tests/webinar-capability-test.cpp src/zoom-engine-client.h src/zoom-engine-client.cpp src/zoom-dock.cpp src/zoom-control-server.cpp
git commit -m "feat(webinar): plugin-side mode state, latest-wins, gated surfaces with named reasons"
```

---

### Task 5: Promote/demote panelist — commands, wrapper, and the EXISTS gate

Everything about promote/demote except two lines is expressible against the tracked tree: command routing, name resolution, the role gate, the refusal reporting. The two lines that are not — the actual `IMeetingWebinarController` method calls — go in their own TU, compiled only under the same `EXISTS` gate as the talkback test target (CMakeLists.txt:1168), because `meeting_webinar_ctrl_interface.h` exists only in the full local SDK drop. **The exact method names (approximately `PromptePanelist2Attendee` / `DepromptAttendee2Panelist` — Zoom's own spellings) are copied from that header at implementation time on a machine with the full drop; they are deliberately not written in this plan because guessing them wrong would compile on no machine and review as if real.** Without the gate satisfied, the commands still route, resolve and report — they just answer `sdk_header_missing`.

**Files:**
- Modify: `src/engine-ipc.h:26` (tokens after `IPC_CMD_TALKBACK_NOMINATE`), `src/engine-command.h:32-50` (enum), `:86-105` (routing)
- Create: `engine/src/engine-webinar.h`, `engine/src/engine-webinar.cpp` (always compiled), `engine/src/engine-webinar-sdk.cpp` (gate-compiled only)
- Modify: `engine/src/main.cpp` (two command branches beside `IpcCommand::TalkbackProbe`, :1500), `CMakeLists.txt` (ENGINE_SOURCES + the gate), `src/zoom-control-server.cpp` (control commands `webinar_promote` / `webinar_demote`), `src/zoom-engine-client.h/.cpp` (pass-through methods)
- Test: `tests/engine-command-test.cpp`

**Interfaces:**
- Consumes: `local_role_id` / `webinar_feature_verdict` (Task 1); `meeting_mode` state engine-side via the same `GetMySelfUser()` reads as Task 2.
- Produces: `IPC_CMD_WEBINAR_PROMOTE` (`"webinar_promote"`), `IPC_CMD_WEBINAR_DEMOTE` (`"webinar_demote"`), `IpcCommand::WebinarPromote/WebinarDemote`; `void EngineWebinar::promote(ZOOMSDK::IMeetingService *svc, const std::string &name)` and `void EngineWebinar::demote(ZOOMSDK::IMeetingService *svc, const std::string &name)`; internal seam `ZOOMSDK::SDKError webinar_sdk_promote(ZOOMSDK::IMeetingService *svc, unsigned int user_id)` / `webinar_sdk_demote(...)` (the gated TU); E2P report `{"cmd":"webinar_role","action":"promote"|"demote","name":"...","code":<int>}` or `{"cmd":"webinar_role","ok":false,"reason":"..."}`.

- [ ] **Step 1: Write the failing test**

Append inside `main()` in `tests/engine-command-test.cpp`, following the talkback commands' pattern:

```cpp
    // --- Webinar promote/demote route exactly, and do not collide ---
    check(ipc_command_of(R"({"cmd":"webinar_promote","name":"Sarah Muller"})") ==
              IpcCommand::WebinarPromote,
          "webinar_promote did not route to IpcCommand::WebinarPromote");
    check(ipc_command_of(R"({"cmd":"webinar_demote","name":"Sarah Muller"})") ==
              IpcCommand::WebinarDemote,
          "webinar_demote did not route to IpcCommand::WebinarDemote");
    check(ipc_command_of(R"({"cmd":"join","display_name":"webinar_promote"})") ==
              IpcCommand::Join,
          "a payload containing 'webinar_promote' hijacked the join branch");
    check(ipc_command_of(R"({"cmd":"webinar_promote_all"})") == IpcCommand::Unknown,
          "a longer command starting with webinar_promote matched it");
```

- [ ] **Step 2: Run test to verify it fails**

```sh
cmake --build build --config Release --target CoreVideoEngineCommandTest --parallel 8
```

Expected: FAIL to compile with `'WebinarPromote' is not a member of 'IpcCommand'`.

- [ ] **Step 3: Write minimal implementation**

`src/engine-ipc.h` after `IPC_CMD_TALKBACK_NOMINATE` (:26):

```c
#define IPC_CMD_WEBINAR_PROMOTE "webinar_promote"
#define IPC_CMD_WEBINAR_DEMOTE  "webinar_demote"
```

`src/engine-command.h`: append `WebinarPromote, WebinarDemote,` to the enum and, in `ipc_command_of` before the final `return`:

```cpp
    if (cmd == IPC_CMD_WEBINAR_PROMOTE) return IpcCommand::WebinarPromote;
    if (cmd == IPC_CMD_WEBINAR_DEMOTE)  return IpcCommand::WebinarDemote;
```

Create `engine/src/engine-webinar.h`:

```cpp
#pragma once
//
// engine-webinar.h — promote/demote webinar panelists by display name.
//
// The tracked SDK tree forward-declares IMeetingWebinarController
// (meeting_service_interface.h:957) but ships no
// meeting_webinar_ctrl_interface.h, so the controller's methods cannot be
// named in any always-compiled TU. The two calls live in
// engine-webinar-sdk.cpp, compiled only when the full local SDK drop supplies
// that header (same EXISTS gate as CoreVideoEngineTalkbackSelectTest,
// CMakeLists.txt:1168). This file and engine-webinar.cpp compile everywhere.
//
#include "zoom_sdk.h"
#include "meeting_service_interface.h"

#include <string>

// The gated seam. Real bodies in engine-webinar-sdk.cpp; without the header,
// engine-webinar.cpp's fallback bodies return SDKERR_WRONG_USAGE and promote()
// reports reason "sdk_header_missing" instead of pretending it tried.
ZOOMSDK::SDKError webinar_sdk_promote(ZOOMSDK::IMeetingService *svc, unsigned int user_id);
ZOOMSDK::SDKError webinar_sdk_demote(ZOOMSDK::IMeetingService *svc, unsigned int user_id);

class EngineWebinar {
public:
    void promote(ZOOMSDK::IMeetingService *svc, const std::string &name);
    void demote(ZOOMSDK::IMeetingService *svc, const std::string &name);

private:
    void act(ZOOMSDK::IMeetingService *svc, const std::string &name, bool to_panelist);
    void refuse(const char *action, const std::string &name, const char *reason);
};
```

Create `engine/src/engine-webinar.cpp`:

```cpp
#include "engine-webinar.h"
#include "engine-writer.h"        // EngineIpc::write — include, never forward-declare
#include "engine-json.h"          // json_escape / zchar_to_utf8
#include "webinar-capability.h"   // the role gate, same table the plugin uses

#include "meeting_participants_ctrl_interface.h"

void EngineWebinar::refuse(const char *action, const std::string &name,
                           const char *reason)
{
    EngineIpc::write(std::string(R"({"cmd":"webinar_role","action":")") + action +
        R"(","name":")" + json_escape(name) +
        R"(","ok":false,"reason":")" + reason + R"("})");
}

void EngineWebinar::act(ZOOMSDK::IMeetingService *svc, const std::string &name,
                        bool to_panelist)
{
    const char *action = to_panelist ? "promote" : "demote";
    if (!svc) { refuse(action, name, "not_in_meeting"); return; }

    // Same gate the plugin pre-checks — re-checked here because the engine is
    // the authority and a role can change between the two processes' reads.
    auto *pc = svc->GetMeetingParticipantsController();
    if (!pc) { refuse(action, name, "no_participants_controller"); return; }
    LocalRole my_role = LocalRole::Unknown;
    if (ZOOMSDK::IUserInfo *self = pc->GetMySelfUser()) {
        if (self->GetUserRole() == ZOOMSDK::USERROLE_HOST)   my_role = LocalRole::Host;
        if (self->GetUserRole() == ZOOMSDK::USERROLE_COHOST) my_role = LocalRole::CoHost;
    }
    bool webinar = false;
    if (ZOOMSDK::IMeetingInfo *info = svc->GetMeetingInfo())
        webinar = info->GetMeetingType() == ZOOMSDK::MEETING_TYPE_WEBINAR;
    const FeatureVerdict v = webinar_feature_verdict(
        webinar ? MeetingMode::Webinar : MeetingMode::Meeting, my_role,
        CoreVideoFeature::PromoteDemote);
    if (!v.available) { refuse(action, name, v.reason); return; }

    // Resolve by NAME at use time — ids are meeting-scoped and recycled.
    unsigned int uid = 0;
    if (ZOOMSDK::IList<unsigned int> *ids = pc->GetParticipantsList()) {
        for (int i = 0; i < ids->GetCount() && uid == 0; ++i) {
            ZOOMSDK::IUserInfo *u = pc->GetUserByUserID(ids->GetItem(i));
            if (u && zchar_to_utf8(u->GetUserName()) == name) uid = ids->GetItem(i);
        }
    }
    // A webinar ATTENDEE is not in this roster (attendees are invisible to
    // GetParticipantsList for panelist-side clients — verified live in Task 6
    // Step 4; if that finding differs, this comment and the reason change
    // together). no_participant_named therefore covers both "typo" and
    // "attendee not visible to us", and the report says so.
    if (uid == 0) { refuse(action, name, "no_participant_named"); return; }

    const ZOOMSDK::SDKError e = to_panelist ? webinar_sdk_promote(svc, uid)
                                            : webinar_sdk_demote(svc, uid);
    EngineIpc::write(std::string(R"({"cmd":"webinar_role","action":")") + action +
        R"(","name":")" + json_escape(name) +
        R"(","code":)" + std::to_string(static_cast<int>(e)) + "}");
}

void EngineWebinar::promote(ZOOMSDK::IMeetingService *svc, const std::string &name)
{ act(svc, name, true); }
void EngineWebinar::demote(ZOOMSDK::IMeetingService *svc, const std::string &name)
{ act(svc, name, false); }

#if !defined(COREVIDEO_HAS_WEBINAR_CTRL)
// Tracked-tree fallback: the controller's header does not exist here, so the
// calls cannot. Loud, distinct failure — never a silent no-op.
ZOOMSDK::SDKError webinar_sdk_promote(ZOOMSDK::IMeetingService *, unsigned int)
{ return ZOOMSDK::SDKERR_WRONG_USAGE; }
ZOOMSDK::SDKError webinar_sdk_demote(ZOOMSDK::IMeetingService *, unsigned int)
{ return ZOOMSDK::SDKERR_WRONG_USAGE; }
#endif
```

Create `engine/src/engine-webinar-sdk.cpp` (compiled only under the gate):

```cpp
// The ONLY TU that names IMeetingWebinarController's methods. Compiled solely
// when the full local SDK drop supplies the header (see CMakeLists gate).
//
// IMPLEMENTATION-TIME STEP, on a machine with the full drop: open
// meeting_service_components/meeting_webinar_ctrl_interface.h and copy the
// EXACT promote/demote method names and signatures (approximately
// PromptePanelist2Attendee / DepromptAttendee2Panelist — Zoom's own
// spellings) into the two marked lines. Do not guess them: this plan was
// written against the tracked tree, which does not contain that header.
#include "engine-webinar.h"
#include "meeting_service_components/meeting_webinar_ctrl_interface.h"

ZOOMSDK::SDKError webinar_sdk_promote(ZOOMSDK::IMeetingService *svc, unsigned int user_id)
{
    ZOOMSDK::IMeetingWebinarController *ctrl = svc->GetMeetingWebinarController();
    if (!ctrl) return ZOOMSDK::SDKERR_SERVICE_FAILED;
    return ctrl-> /* <copy the attendee→panelist method from the full-SDK header> */ (user_id);
}

ZOOMSDK::SDKError webinar_sdk_demote(ZOOMSDK::IMeetingService *svc, unsigned int user_id)
{
    ZOOMSDK::IMeetingWebinarController *ctrl = svc->GetMeetingWebinarController();
    if (!ctrl) return ZOOMSDK::SDKERR_SERVICE_FAILED;
    return ctrl-> /* <copy the panelist→attendee method from the full-SDK header> */ (user_id);
}
```

`CMakeLists.txt`: add `engine/src/engine-webinar.cpp` to `ENGINE_SOURCES` unconditionally, then beside it:

```cmake
# Same precedent as the talkback test gate at line ~1168: the tracked SDK tree
# has no meeting_webinar_ctrl_interface.h; the full local drop does. Without
# it the engine still builds, and promote/demote answers sdk_header_missing.
if(EXISTS "${ZOOM_SDK_INCLUDE_DIR}/meeting_service_components/meeting_webinar_ctrl_interface.h")
    list(APPEND ENGINE_SOURCES engine/src/engine-webinar-sdk.cpp)
    set(COREVIDEO_HAS_WEBINAR_CTRL TRUE)
endif()
```

and `target_compile_definitions(ZoomObsEngine PRIVATE COREVIDEO_HAS_WEBINAR_CTRL)` under the same condition. Wire `main.cpp`: `static EngineWebinar webinar;` beside `talkback` (:1290), and two branches beside `IpcCommand::TalkbackProbe` (:1500):

```cpp
        } else if (command == IpcCommand::WebinarPromote) {
            webinar.promote(meeting_svc, json_str(line, "name"));
        } else if (command == IpcCommand::WebinarDemote) {
            webinar.demote(meeting_svc, json_str(line, "name"));
```

When `COREVIDEO_HAS_WEBINAR_CTRL` is unset, map the `SDKERR_WRONG_USAGE` sentinel to reason `sdk_header_missing` inside `act()` (check the define there, before calling the seam) so the operator-facing reason names the real cause, not a generic code. Plugin side: `ZoomEngineClient::webinar_promote(name)` / `webinar_demote(name)` (fire-and-acknowledge, `webinar_role` lines logged verbatim like `talkback_probe`), and `ZoomControlServer` commands `webinar_promote` / `webinar_demote` taking `"name"`, pre-checked against `webinar_feature_verdict` for a fast local refusal.

- [ ] **Step 4: Run test to verify it passes, then the full suite**

```sh
cmake --build build --config Release --parallel 8
cd build && ctest -C Release --output-on-failure
```

Expected: routing test PASS; engine links on the tracked tree (fallback bodies); on a machine with the full SDK drop, configure again and confirm `engine-webinar-sdk.cpp` enters the build and links once the two marked calls are filled in from the real header.

- [ ] **Step 5: Commit**

```sh
git add src/engine-ipc.h src/engine-command.h tests/engine-command-test.cpp engine/src/engine-webinar.h engine/src/engine-webinar.cpp engine/src/engine-webinar-sdk.cpp engine/src/main.cpp CMakeLists.txt src/zoom-engine-client.h src/zoom-engine-client.cpp src/zoom-control-server.cpp
git commit -m "feat(webinar): promote/demote panelist by name behind the full-SDK EXISTS gate"
```

---

### Task 6: Live verification — THE GATE

The compile proves the tracked-tree API exists; only a real webinar proves the constraints this plan encodes. Three of the Spec's claims are stated as to-verify, and this task is where they become facts: (a) an attendee truly has no raw-data path, (b) the panelist role suffices for the local-recording privilege and per-participant subscription, (c) talkback's behaviour inside a webinar. Use a dedicated TEST webinar, never a live show; install the matched pair (both binaries, SHA256-verified) first.

**Files:**
- Create: `docs/superpowers/notes/2026-08-29-webinar-live-results.md` (the actual output, verbatim)

**Interfaces:**
- Consumes: everything above.
- Produces: the go/no-go facts for un-gating talkback (`talkback_unverified_in_webinar`) and for confirming the attendee-roster claim in `engine-webinar.cpp`'s comment.

- [ ] **Step 1: Join as ATTENDEE.** Schedule a webinar on the entitled account; join CoreVideo via the control API with the webinar URL in `meeting_id`. Record: the `webinar_join` stage lines (register/screen-name handshakes), the `meeting_mode` line (`"webinar":true,"role":"attendee"`), whether backstage/practice session reports `"backstage":true` before broadcast starts, and — critically — that the join watchdog and `awaiting_admission` machinery stay quiet (no waiting-room state exists to misreport).
- [ ] **Step 2: Attendee raw-data constraint.** `start_engine` twice, `assign_output` to a named panelist. Expected: the privilege request or `createRenderer` fails (record the exact `SDKError`); the dock shows the `webinar_attendee_no_raw_media` explanation. If raw media WORKS as an attendee, the capability table's central assumption is wrong — stop, record, and amend Task 1's table before anything ships.
- [ ] **Step 3: Promote to panelist, live.** From the host's Zoom client, promote CoreVideo. Record: a fresh `meeting_mode` line with `"role":"panelist"` arriving via `roster_changed()` (this is the event path Task 2 exists for), then repeat Step 2 — subscription and ISO must now work end-to-end (frames flowing, WAV/MP4 nonzero).
- [ ] **Step 4: Roster visibility + promote/demote round trip.** As panelist/co-host, run `{"cmd":"webinar_promote","name":"<attendee display name>"}`. Record whether the attendee appears in `GetParticipantsList` at all (the `no_participant_named` comment in engine-webinar.cpp depends on this answer — fix the comment if reality differs), the `webinar_role` result line, and the demote round trip. Also record `GetWebinarAttendeeStatus()->allow_talk` before/after the host allows an attendee to talk, if observable.
- [ ] **Step 5: Talkback in a webinar.** As co-host or host, on a machine with the full SDK drop, run `{"cmd":"talkback_probe","participant":"<panelist name>"}` after TEMPORARILY bypassing the `talkback_unverified_in_webinar` refusal (a local, uncommitted edit — the gate exists precisely so this bypass cannot ship). Record every rung. Only a full green ladder plus human confirmation justifies changing the Task 1 verdict; a `meeting_supported=false` or NOPERMISSION rung makes the fail-closed verdict permanent and documented.
- [ ] **Step 6: Record and decide.** Write all of it into `docs/superpowers/notes/2026-08-29-webinar-live-results.md`, update the CLAUDE.md invariants list in the same change (docs-updated is part of done), and commit:

```sh
git add docs/superpowers/notes/2026-08-29-webinar-live-results.md CLAUDE.md
git commit -m "docs(webinar): live gate results — attendee/panelist raw-data, promote round trip, talkback verdict"
```

---

## Self-Review

**Placeholder scan.** Task 5's two marked call sites in `engine-webinar-sdk.cpp` are the plan's one stated SDK gap, not a placeholder: the method names physically do not exist in the tracked tree (`third_party/zoom-sdk/h` has no `meeting_webinar_ctrl_interface.h` — verified by glob), the gap is fenced behind the same `EXISTS` gate the repo already uses at CMakeLists.txt:1168, a loud fallback (`sdk_header_missing`) covers the ungated build, and the fill-in step is written into the file itself. Task 3's `std::wstring(begin, end)` sketch names `to_zstr` as the required call. Tasks 2 and 3 declare "no unit test, SDK-bound" explicitly with the live verification route named — the same statement the talkback Milestone 1 plan made for its Tasks 3/4. Everything else carries real code.

**Cross-task type consistency.** `MeetingMode` / `LocalRole` / `CoreVideoFeature` / `FeatureVerdict` / `WebinarState` are defined once in Task 1's `src/webinar-capability.h` and consumed by Tasks 2 (engine, `local_role_id`), 4 (plugin, `webinar_state_apply` / `webinar_feature_verdict`) and 5 (engine, `webinar_feature_verdict`) with matching signatures. `webinar_sdk_promote/demote(ZOOMSDK::IMeetingService*, unsigned int)` is declared in Task 5's header and defined twice under mutually exclusive compilation (`#if !defined(COREVIDEO_HAS_WEBINAR_CTRL)` fallback vs. the gated TU) — exactly one definition links in either configuration. `IPC_CMD_WEBINAR_PROMOTE/DEMOTE` and the two enum values are produced and consumed only in Task 5. Every SDK symbol named in code was verified against the tracked headers by line number (listed in the Spec section); the only unverified names are the two the plan explicitly refuses to write.

**Honest constraints, restated once.** Attendees have no per-participant raw streams and cannot hold the local-recording privilege (fail-closed in Task 1, proven or amended in Task 6 Step 2). Talkback is meetings-only until Task 6 Step 5 says otherwise. Webinar attendees have no waiting room; backstage (`IsInWebinarBackstage`) is the pre-broadcast signal and is reported, never "admitted". `MeetingMode::Unknown` fails OPEN by design, because a DLL-only half-install is this repo's canonical mistake and gating on it would break every ordinary meeting.
