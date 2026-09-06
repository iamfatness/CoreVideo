# Task 3 report — macOS waiting-room watchdog mapping

## Result

The macOS meeting callback now emits the existing Windows-compatible
`awaiting_admission` event on every SDK status change. The installed SDK symbols
`ZoomSDKMeetingStatus_WaitingForHost` and
`ZoomSDKMeetingStatus_InWaitingRoom` emit `active:true`; all other statuses emit
`active:false`. The event is sent inside the existing fresh-callback/epoch gate
and immediately before the existing status switch. Delegate ownership, terminal
delivery, leaving behavior, and raw-media lifecycle callbacks were not changed.

The 120-second watchdog policy and dock copy were already correct and remain
unchanged. The client continues to assign the event's boolean directly and to
clear it on joined, left, failure, and lifecycle resets.

## Files

- `engine/src/macos-admission-state.h`: SDK-symbolic status-to-wire mapping.
- `engine/src/main-macos.mm`: emits the mapping for every delivered status.
- `tests/macos-admission-state-test.mm`: compiles the production mapping against
  ZoomSDK 7.1.5 and feeds its emitted event into the watchdog policy.
- `CMakeLists.txt`: registers the SDK-backed regression on macOS when the engine
  is available.
- `CLAUDE.md`: records the mapping and regression invariant.

## RED / GREEN evidence

RED command:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target CoreVideoMacosAdmissionStateTest --parallel 8
```

Result: exit 2, with the expected compile failure:
`fatal error: 'macos-admission-state.h' file not found`. This demonstrated that
the regression depended on the missing production mapping seam.

GREEN focused command:

```sh
cmake --build build --target CoreVideoMacosAdmissionStateTest CoreVideoJoinWatchdogTest CoreVideoJoinDecisionTest --parallel 8 && ctest --test-dir build -C Release --output-on-failure -R '^(CoreVideoMacosAdmissionState|CoreVideoJoinWatchdog|CoreVideoJoinDecision)$'
```

Result: exit 0; 3/3 passed:
`CoreVideoJoinDecision`, `CoreVideoJoinWatchdog`, and
`CoreVideoMacosAdmissionState`.

The regression verifies the actual production wire mapping using SDK symbols:
both host-controlled waits hold at 180 seconds, leaving the wait clears the
wire state and grants a fresh 120-second window, a genuine stall fires after
that window, and leave/rejoin sequences do not retain admission state.

## Full verification

Build command (log: `task-3-build.log`):

```sh
cmake --build build --config Release --parallel 8
```

Result: exit 0. The real `ZoomObsEngine` Objective-C++ target compiled and
linked against the configured ZoomSDK 7.1.5 framework; the OBS plugin and all
test targets also built.

Suite command (log: `task-3-ctest.log`):

```sh
ctest --test-dir build -C Release --output-on-failure
```

Result: exit 0; 73/73 tests passed in 3.83 seconds.

Additional inspection command:

```sh
git diff --check
```

Result: exit 0, no whitespace errors.

## Unavailable validation

The required live dock-button trials (five minutes waiting-room admission and
waiting-for-host) were explicitly unavailable for this task. OBS was not
launched, no meeting was joined, and nothing was installed, signed, or pushed.
