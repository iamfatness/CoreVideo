# Task 1 report — macOS raw-media lifecycle candidate

## Outcome and evidence boundary

Implemented the demonstrable lifecycle defects found in `main-macos.mm`; offline
regressions and the real SDK build pass. **The five delayed-grant trials and five
breakout round trips are not performed; this is not a live-accepted fix.** No OBS
launch, meeting join, install, signing, push, or publication was performed.

Controller ruling: a host-controlled reproduction is unavailable in this run.
Use the existing soak chronology, code, installed SDK headers and deterministic
regressions; explicitly retain the live acceptance gate. The soak shows raw start
09:55:52, breakout status14 at09:56:05, 24 tile subscribe failures code21 during
09:56:07–31, and manual restart recovery around09:57. That chronology identifies
a missing lifecycle boundary but does not prove a transient licensing error or a
safe retry interval. Earlier request status2 at09:48:48 is **Timeout**, not denial.

Installed SDK evidence:
`/Users/jwallace/Developer/zoom-sdk-macos/ZoomSDK.framework/Headers/ZoomSDKErrors.h`
has NoPermission=6, NoLicense=21, InMeeting=3, Join_Breakout_Room=14,
Leave_Breakout_Room=15, and Granted=0 / Denied=1 / Timeout=2 for recording request
status. `ZoomSDKMeetingRecordController.h` documents canStartRawRecording and
startRawRecording but promises no timer-based recovery from NoLicense.

## Demonstrated code defects and changes

- The old Granted request callback unconditionally called start, unlike the
  other grant callback. Duplicate grants could start twice; late grants after
  explicit Stop could resurrect raw media. The new pure lifecycle owner separates
  Start intent, actual room readiness, pending permission, and active raw start.
  State is committed before SDK calls; duplicate callbacks emit no second start
  or restoration. Retired record-delegate blocks compare identity on the main
  queue, so Stop/leave/replacement cancels old queued grant work.
- Breakout status14/15 formerly fell through default, leaving active=true and
  old SDK resources attached. Transition now marks recovering and suspends SDK
  renderers/audio once while retaining source bindings and SHM generations.
  InMeeting rebuilds roster, checks permission/raw start, and restores once.
  Missing room-scoped participant IDs remain pending for plugin rebinding.
- Permission revocation previously did nothing. It now invalidates readiness,
  suspends active resources, reports denied, and recovers once on a later grant
  only while Start is still wanted. Timeout has its own reason and never revokes
  an already-active grant.
- Failed video subscribe attempts formerly retained the source-to-participant map
  but no VideoSubscription/target placeholder. Recovery could lose such intent.
  Placeholders now persist through failures; existing placeholders can retry;
  recovery uses current bindings, not a captured pre-transition source list.
  Source removal and rebinding use the same existing table. Every target retains
  requested_resolution for the later shared-renderer quality task.
- Only actual NoPermission asks the host, once per meeting including manual
  Stop/Start cycles. Other check/start/request failures terminate with an actual
  SDK code, actionable raw_media_start_failed detail, and no meeting leave.
- raw_media_ready remains a compatible successful-session-start event. It does
  not assert healthy sources; per-source subscribe errors and frame events remain
  independent evidence. Additive state events invalidate client readiness.
- First-frame diagnostic counters restart after suspension, while SHM generations
  and mappings survive. Detach-before-destroy, SDK main queue ownership, the one
  global audio helper subscription, and automatic-speaker-cut isolation remain.

## Retry-budget ruling

**No new timed session retry loop was added.** There is one attempt per actual
permission/room readiness transition, and terminal start errors wait for an
explicit Start or a fresh room transition. NoLicense21 is not documented as a
transient permission error; the proposed 0/1/2/4/8-second budget cannot be
confirmed without live reproduction. Existing tile retry policy is unchanged;
no concurrent new session loop amplifies it. This is a deliberate partial-scope
ruling, not evidence that the plan's proposed timer or 15-second acceptance target
has been satisfied.

## RED / GREEN and validation

Before implementing the new transitions, added CoreVideoRawMediaLifecycle using
an SDK-free extraction of the old callback decisions: Start/Grant always check,
one host request, successful starts restore, Stop clears active/request state,
no transition/revocation/intent handling. This is **a model of legacy decisions,
not execution of the Objective-C SDK glue**. It failed these ten assertions:

1. duplicate pending Start has one host request and one check
2. pending remains visible
3. denial then both grant callbacks start and restore once
4. late grant cannot resurrect explicit Stop
5. transition suspends once and grant cannot start inside transfer
6. revocation invalidates active readiness
7. revocation then grant recovers once
8. permanent start failure has finite attempts and terminal error
9. terminal failure remains visible
10. replacement meeting cancels old intent

Exact RED command: `ctest --test-dir build -R '^CoreVideoRawMediaLifecycle$' --output-on-failure`
returned exit8, 0/1 passing, 0.20seconds. The helper was then replaced with the
production lifecycle owner and the same regression returned 1/1 passing,
0.25seconds. Additional cases cover timeout/grant ordering, Start before meeting
readiness, per-meeting request budget across manual Stop/Start and reset, and
non-permission check failures. Assertions count check/request/start/restore/
suspend/error effects and inspect visible state; the restoration harness reads
its current desired set after a source removal. This does not exercise actual
renderer/SHM lifetime or prove real SDK restoration succeeds.

- `cmake -S . -B build`: succeeded (required to register the new CTest target).
- `cmake --build build --parallel 8`: succeeded for real ZoomObsEngine and plugin.
  An intermediate build caught using json_str in the Qt client; corrected to
  its existing QJsonObject API before the passing build.
- `ctest --test-dir build -R 'CoreVideo(RawMediaLifecycle|TileRetry|AudioSubscriptionState|Shm)' --output-on-failure`:
  **7/7 passed**, 2.07seconds, including frame reader, resubscribe, generation,
  engine restart, audio subscription state, lifecycle, and tile retry.
- `ctest --test-dir build --output-on-failure`: **71/71 passed**, 3.64seconds.
- `git diff --check`: passed.

Logs: `task-1-build-final.log` and `task-1-ctest.log` in this plan directory.
No measurement of live first-frame recovery time is available.

## Files and interfaces for following tasks

- `src/raw-media-lifecycle.h`: SDK-free event/action owner, used by macOS engine.
- `tests/raw-media-lifecycle-test.cpp`, `CMakeLists.txt`: new CTest registration.
- `engine/src/main-macos.mm`: owner integration, grant/transition callbacks,
  suspension preserving desired bindings, failed-subscription placeholders.
- `src/zoom-engine-client.cpp`: non-active lifecycle events clear m_media_active.
- `src/engine-ipc.h`: additive protocol documentation.
- `CLAUDE.md`: demonstrated invariants and explicit live validation limit.

Task2 can consume debug `stage=raw_media_state`, `state` one of stopped,
waiting_permission, denied, recovering, starting, active, failed, with `reason`.
Timeout reports `reason=privilege_request_timeout`, with current state unchanged.
The two starting substates intentionally share one public name. Terminal errors
retain `msg=raw_media_start_failed`, `reason`, `code`, and actionable `detail`.
Pending request keeps the existing `privilege_requested=true` error shape for
compatibility. A raw_media_ready event clears existing pending notice behavior.
Task2 should surface recovery/denial/terminal states without turning them into a
join failure or assuming active means every source is healthy.

Task5 can use `VideoTarget::requested_resolution`; `VideoSubscription::resolution`
is currently the effective candidate after success and remains the existing
single-renderer field. Quality arbitration remains intentionally unimplemented.

## Open acceptance / review concerns

1. Verify five delayed grants (including after real denial versus timeout), grants
   immediately before breakout, and five round trips with established permission.
   Measure first frames from actual room/raw SDK readiness, target under15seconds.
2. Verify the SDK's real renderer and audio-helper lifetime at status14/15. The
   code safely detaches delegates before destroy, but only a live run establishes
   that resource retirement timing is accepted by Zoom in both directions.
3. NoLicense21 root cause remains unproven. Raw start success must not clear a
   source licensing failure; repeated recovery will not repair a real entitlement.
4. Roster IDs are room-scoped: validate plugin rebinding and removed outputs during
   transfer. The decision test does not execute the production binding table.
5. Request timeout is accurately distinguished but Task2 still needs user-facing
   lifecycle notices. Live repeated errors/modals are its separate scope.
