# CoreVideo Soak Remediation Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task by task. Steps use checkbox syntax for tracking. The user approved implementation on September 6, 2026; live validation and release gates still apply.

**Goal:** Make late recording permission, waiting-room admission, and breakout transitions recover predictably without an error flood, while preserving the sustained video stability observed in the September 6 soak.

**Architecture:** Repair macOS SDK lifecycle handling at the engine boundary and keep pending/recovering states separate from persistent failures in the OBS client. Preserve existing director, shared-memory, audio, and watchdog policies; use focused reproductions to determine whether switching and resolution need behavioral changes.

**Tech Stack:** C++17, Objective-C++, Zoom Meeting SDK, OBS/Qt, CMake/CTest; plain executable regression tests.

**Spec:** Sanitized evidence and requirements are included below; full local report is retained outside the repository.

## Global constraints

- Mac validation baseline: CoreVideo v0.1.45-beta.1 with OBS 32.2.1; preserve Windows compatibility.
- Signing certificates stay in the login Keychain. Use local notarytool profile `CoreVideo Plugin`; never export credentials to GitHub.
- Do not change the currently installed package while implementing. Test a separately versioned candidate after OBS is closed.
- Preserve renderer delegate detachment before destruction, nonblocking SDK frame callbacks, shared-memory generation ownership, and single-publisher audio during director handovers.
- Never call global output resubscription for ordinary automatic speaker cuts.
- Distinguish SDK code 21 from a proven licensing issue. Distinguish shared-memory generation releases from dropped video frames.
- Keep meeting data local. Tests use synthetic participant IDs and contain no credentials.
- Use separate reviewable changes for lifecycle recovery, watchdog mapping, diagnostics, director investigation, and resolution investigation. Do not make all fixes contingent on solving HD negotiation.

## Evidence and baseline gate

Observed: permission granted and raw recording started successfully at 09:55:52.577; breakout entry at 09:56:05.599; 24 subscription errors across eight tiles from 09:56:07.409 to 09:56:31.516. Manual stop/start at 09:57 restored delivery. Those are one error burst, counted once. The user experienced it when recording rights became available; timing alone cannot isolate permission grant from breakout entry.

Following recovery, approximately 108 minutes passed without another observed subscription failure. Final logs counted 190 handovers (maximum 75 ms) and 26 OBS rendering-lag frames out of 192,125 attempted drawn frames. This is the stability baseline, not proof of audio or HD quality.

Read-only code review used `/Users/jwallace/Developer/CoreVideo` at `65d288b84a998188ff4ccc78a53bf60a9bb7cb4a`. The signed PR 251 checkout is at `e96b15ba0c0389c7a6255a6f767294d74fcefb5c`. The implementation must first fetch current main in an isolated worktree and compare these code paths; the local Developer checkout is not asserted to be current main. Do not overwrite that checkout.

- [ ] Record current main SHA, installed candidate version, SDK version, and relevant diff from the signed beta.
- [ ] Read repository instructions in the implementation worktree and establish a passing CTest baseline before editing.
- [ ] Copy this plan and its sanitized evidence summary into `docs/superpowers/plans/2026-09-06-soak-remediation.md` in the implementation branch. Keep raw logs outside the repository.

## Task 1 — Highest priority: permission grant and breakout recovery

**Files to inspect/modify:** `engine/src/main-macos.mm` (`CVRecordDelegate`, `handle_start_media`, `handle_stop_media`, `resubscribe_raw_media`, `video_subscribe`, `CVMeetingDelegate`); `src/zoom-engine-client.cpp` and `.h` for readiness events; `src/engine-ipc.h` for protocol documentation if fields are added. Add `src/raw-media-lifecycle.h`, `tests/raw-media-lifecycle-test.cpp`, and its CMake test only for a small SDK-independent decision model. Do not move SDK ownership into that helper.

**Interface contract:** Meeting callbacks and explicit Start/Stop intent feed one lifecycle decision owner on the SDK main queue. Existing `raw_media_ready` consumers remain compatible. The engine owns whether a start is requested, permission is pending/denied/granted, a room transition is underway, and raw media is actually started. A successful start alone must not masquerade as proof that all video sources are healthy.

- [ ] Reproduce delayed grant after denial without entering a breakout. Capture permission callbacks, start calls, subscription attempts, error callbacks, and first-frame success.
- [ ] Repeat with permission granted immediately before breakout entry; also transfer with permission already established. Compare against the soak timeline before choosing the recovery transition.
- [ ] Add a deterministic event-sequence regression for the reproduced failure. Assert effects (number of start/request/subscribe actions), not private field layout. Include these cases:

| Synthetic sequence | Required result |
| --- | --- |
| Start → permission pending → duplicate Start | One host request; pending state; no subscription flood |
| Pending → denied → granted | Start once after grant; recover queued eligible outputs |
| Both grant callbacks arrive | No duplicate raw start or duplicate subscription set |
| Start → pending → explicit Stop → late grant | Media stays stopped |
| Active → breakout transition → in meeting | Revalidate readiness; restore current eligible subscriptions once ready |
| Recovering → source removed → readiness restored | Removed source is not recreated |
| Active → permission revoked → granted | Explicit pending/recovering state; recovery respects user intent |
| SDK start permanently fails | Finite retries; actionable terminal state; meeting remains joined |

- [ ] Run the new test against the failing behavior before implementing the state transition. Use `CoreVideoRawMediaLifecycle` as the new CTest name.
- [ ] Preserve desired subscription assignments during transient recovery. Invalidate only SDK resources that the reproduction shows are invalid; detach delegates before renderer destruction and execute SDK calls on the main queue.
- [ ] Make start/grant handling idempotent, and cancel queued work on explicit stop, leave, or replacement meeting. Do not reuse a destructive manual stop path for transient recovery if it discards desired subscriptions.
- [ ] Use a finite recovery schedule: proposed delays 0, 1, 2, 4, and 8 seconds after a readiness transition. Stop immediately on success, explicit stop, leave, or confirmed denial. Confirm this budget in reproduction; do not run simultaneous tile and session retry loops.
- [ ] Run the focused regression and existing `CoreVideoTileRetry`, `CoreVideoAudioSubscriptionState`, and shared-memory tests. Repeat the real SDK scenarios because a pure state test cannot validate SDK resource lifetime.
- [ ] Commit lifecycle recovery with its tests and update `CLAUDE.md` with the demonstrated invariant.

**Acceptance:** Eligible video returns without manual Start/Stop in five repeated delayed-grant trials and five breakout round trips. Duplicate grants cause no duplicate starts. Cancellation prevents late callbacks from resurrecting media. An unrecoverable failure stops retrying and remains visible. Record measured recovery time; target first frames within 15 seconds of actual SDK readiness, not 15 seconds after host permission alone.

## Task 2 — Highest priority: prevent the error flood and clear recovered errors

**Files:** `src/zoom-engine-client.cpp/.h` (`handle_event`, error and notice callbacks, `last_error`); `src/zoom-privilege-notice.h`; `src/zoom-dock.cpp` (notice/error presentation); `src/zoom-source.cpp`, `src/zoom-output-manager.cpp/.h` (source health/removal); `tests/zoom-privilege-notice-test.cpp`; new `src/media-failure-state.h` and `tests/media-failure-state-test.cpp` if needed for independent aggregation tests; `CMakeLists.txt`.

**Code evidence:** Pending recording privilege already uses a notice path, but `video_subscribe_failed` for each known participant sets the global error and invokes error callbacks. The notice text also tells the operator to click Start Engine again although grant callbacks attempt automatic startup. Review current main before changing either behavior.

**Interface contract:** Consume lifecycle readiness and source-specific failure/recovery events. Produce one current operator status for a shared recovery episode plus source-specific diagnostics. Preserve existing status fields; additive structured fields must be backward compatible with Windows and control clients. Do not infer source recovery merely from `raw_media_ready`.

- [ ] Reproduce/count actual UI error callbacks for eight eligible failed tiles repeated three times. Preserve all 24 diagnostic events but require at most one user-facing notice for the shared failure episode.
- [ ] Add regression sequences: pending request repeated; denied then granted; 24 errors with one root cause; one source recovers while another remains failed; removed source; unknown/absent participant; a new failure after full recovery; fatal engine disconnect during media recovery.
- [ ] Implement aggregation by meeting/recovery episode and cause, retaining source membership. Repeated failures update details instead of opening another modal. Persistent failure transitions from recovering to one actionable error; a new independent fatal error is never swallowed.
- [ ] Clear a source's current failure after confirmed fresh delivery or removal. Clear the aggregate only when no relevant failed sources remain. Keep historical events in diagnostics.
- [ ] Replace manual-restart permission guidance after automatic recovery is proven: pending copy says `Waiting for the host to allow recording. Media will start automatically when permission is granted.` A denial supplies a host-action instruction; persistent SDK failure supplies Retry and diagnostic context without a dialog storm.
- [ ] Run `CoreVideoPrivilegeNotice`, new `CoreVideoMediaFailureState`, and `CoreVideoOutputHealth`; manually verify dock and status API agree during wait, recovery, partial failure, and success.
- [ ] Commit diagnostics separately from lifecycle recovery.

**Acceptance:** The recorded eight-tile/three-batch scenario creates no sequence of per-tile modals. A shared pending/recovering state stays visible. Real terminal failures remain actionable. Successful or removed tiles do not leave a stale global error. This task does not pass by merely hiding errors.

## Task 3 — High priority: macOS waiting-room watchdog mapping

**Files:** `engine/src/main-macos.mm` (`CVMeetingDelegate::onMeetingStatusChange`); compare `engine/src/main.cpp` admission-event handling; `src/zoom-engine-client.cpp` (`awaiting_admission`); `src/zoom-dock.cpp` watchdog timer; `src/join-watchdog.h`; `tests/join-watchdog-test.cpp`.

**Code evidence:** The existing watchdog already holds its window while awaiting admission. The reviewed macOS callback handles in-meeting, ended/disconnecting, and failed states, but emits no `awaiting_admission` event. Test this missing integration rather than weakening the already-tested 120-second policy.

**Interface contract:** Reuse `{"cmd":"awaiting_admission","active":true}` for SDK waiting-room/waiting-for-host states. Emit false when leaving those states; retain joined/left/failure clearing in the client.

- [ ] Verify installed SDK enum symbols and trace the Mac callback through the existing client/dock path.
- [ ] Add a regression covering callback-state translation plus watchdog evaluation: waiting at 180 seconds holds; admission then a genuinely stalled phase receives a fresh 120-second window; leave/rejoin does not retain admission state.
- [ ] Implement admission-state emission using SDK symbols, preserving the Windows wire contract.
- [ ] Run `CoreVideoJoinWatchdog` and `CoreVideoJoinDecision` plus the callback-mapping regression.
- [ ] Join using the actual dock button and wait at least five minutes for host admission. Repeat waiting for host. API-only join is insufficient: it does not arm the dock watchdog.
- [ ] Commit the mapping correction and its tests.

**Acceptance:** No automatic leave during either legitimate host-controlled wait; a real stalled join still times out once. Dock copy accurately describes the wait.

## Task 4 — Medium priority: explain and, if defective, correct fast speaker cuts

**Files:** `src/speaker-director.cpp/.h`, `src/zoom-dock.cpp`, `src/zoom-source.cpp`, `src/director-handover.h`, `tests/speaker-director-test.cpp`, `tests/speaker-settings-merge-test.cpp`, `tests/director-handover-test.cpp`.

**Interface contract:** Preserve automatic hold/sensitivity behavior, immediate intentional manual takes, and existing eligibility exceptions. Add bounded diagnostic attribution at each actual promotion: reason, effective hold/sensitivity, elapsed hold, candidate age, and synthetic/session-local IDs. No participant names or per-frame logging.

- [ ] Reproduce alternating/overlapping speakers with 1,200 ms hold and 250 ms sensitivity; separately test manual takes, video eligibility changes, and settings updates.
- [ ] Attribute each cut to automatic promotion, manual take, or forced vacancy. Existing forced-vacancy logic can legitimately bypass the full hold; short intervals alone are not proof of a broken timer.
- [ ] Add a deterministic regression only for an unintended bypass. Required behavior: ordinary automatic cuts cannot occur before both candidate sensitivity and incumbent hold are met; manual takes remain immediate; repeated eligibility changes follow the documented vacancy policy without resetting settings unexpectedly.
- [ ] Fix the smallest proven path, or deliver a documented explanation with traces if all cuts follow intended policy. Do not globally increase hold or disable intentional exceptions.
- [ ] Run `CoreVideoSpeakerDirector`, `CoreVideoSpeakerSettingsMerge`, `CoreVideoDirectorHandover`, and `CoreVideoDirectorPreviewFrameGuard`; verify unrelated fixed outputs are not resubscribed by automatic cuts.
- [ ] Commit the demonstrated correction or diagnostic improvement independently.

**Acceptance:** Every reproduced short cut is attributable; normal automatic cuts respect the configured hold. Preserve smooth preview coverage and single-source audio during handover.

## Task 5 — Medium priority: determine why requested HD remains 360p

**Files:** `src/zoom-source.cpp/.h` (quality retry/cooldown); `src/zoom-output-manager.h`; `engine/src/main-macos.mm` video subscription/resolution selection; `tests/output-health-test.cpp`; output diagnostics UI only if a proven missing status needs presentation.

**Interface contract:** Keep requested resolution, actual frame dimensions, cooldown, and attempt counts distinct. A fresh 360p feed is lower resolution, not stale or disconnected.

- [ ] Use a known HD-capable sender and confirm actual Zoom meeting/account HD settings; record sender configuration and the SDK's accepted request/result without presuming entitlement from camera capability.
- [ ] Compare one fixed output at 360p, 720p, and 1080p requests, then the same sender through Active Speaker and multiple tiles. Record actual dimensions for at least 60 seconds per case.
- [ ] Identify whether the limiter is upstream, SDK negotiation, or CoreVideo retry/subscription policy. Compare the Participant 2 quality retry with the other outputs that never attempted one.
- [ ] If CoreVideo is responsible, add a regression for the specific selection/cooldown defect before changing it. Keep retries finite and avoid repeatedly tearing down fresh media for unavailable HD.
- [ ] Run `CoreVideoOutputHealth` and the added regression; repeat the controlled sender comparison. If upstream-limited, document the limitation and accurate UI state instead of manufacturing an upgrade loop.

**Acceptance:** HD appears when the controlled SDK session delivers it; otherwise diagnostics accurately show the requested and received quality with a supported explanation. No new stalls or unbounded upgrades.

## Out of scope: missing ATEM device

The user confirmed the missing ATEM device is their OBS configuration. No CoreVideo fix, configuration task, or release gate is required. Retain the observation only to distinguish this known log noise from plugin errors.

## Final integration and release gate

- [ ] Build the plugin and engine together from the implementation worktree using the established macOS configuration, including the known OpenGL/AGL workaround if that environment still requires it.
- [ ] Run `cmake --build build --config Release --parallel 8` and `ctest --test-dir build -C Release --output-on-failure` in the configured implementation worktree. Run Windows CI for shared client/policy changes. Require all applicable tests to pass.
- [ ] Perform a dedicated test meeting: delayed grant/denial/regrant, five breakout round trips, five-minute waiting room, normal/manual speaker cuts, screen share start/stop, intended audio monitoring, and a short recording/playback. Record uncovered paths explicitly; do not claim ISO support validated unless actually exercised.
- [ ] Run a two-hour soak after the last behavioral change. Require no manual media restart, no repeated modal errors, no persistent recovered errors, no unexpected disconnects, fresh frames, and no material rendering/audio degradation relative to this baseline under comparable load.
- [ ] Update `CLAUDE.md`, changelog, and operator documentation to match demonstrated behavior. Update website claims only where behavior or downloads change; regenerate public content through the site build rather than editing generated files.
- [ ] Build a new versioned Mac installer, sign and notarize locally, validate signatures and ticket, and install on a second Mac. Retain the current working signed beta for rollback.
- [ ] Review and merge approved changes, then publish the verified installer/checksum and update website links. Never remove the known-good download before its replacement is verified. Certificates remain local.

## Review checkpoints

1. Lifecycle + error burst: review together for end-to-end recovery, as separate commits.
2. Waiting-room mapping: independently reviewable and can ship alongside checkpoint 1.
3. Speaker/HD: ship only proven corrections; unresolved upstream questions do not block the urgent lifecycle fixes.
4. Release: tests, controlled meeting, soak, signing, second-Mac install, then publication.

No production code, OBS settings, releases, or website content were changed while preparing this plan.
