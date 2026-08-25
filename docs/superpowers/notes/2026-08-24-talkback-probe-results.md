# Zoom Talkback — Milestone 1 probe results

**Date:** 2026-08-24
**Verdict:** ✅ **GATE PASSED.** Talkback is viable on this account. Milestones 2–8 may start.
**Build:** branch `feat/talkback` @ `5739919`, installed and byte-verified against
`build_x64/Release` (DLL `16E5266E…`, engine `3AE747A6…`).

---

## Run 1 — baseline, CoreVideo as a PLAIN PARTICIPANT

Meeting: a shared rig with 12 participants. Target: `John Wallace | 1338`.

```
stage=controller          ok=true
stage=meeting_supported   supported=true
stage=set_event           code=0
stage=create_channel      code=12        <-- SDKERR_NO_PERMISSION
```

Run twice, ~2 minutes apart, identical result — deterministic, not a flake.

**This converts the spec's two unproven assumptions into facts:**

1. **The meeting-level entitlement gate PASSES** (`IsMeetingSupportTalkBack() == true`).
   This was the single largest risk in the design — had it returned false, the feature
   was dead on this account. The Zoom Enhanced Media assumption holds.
2. **Host or co-host is REQUIRED.** A plain participant is refused at `CreateChannel`
   with `SDKERR_NO_PERMISSION` (= 12, counting `zoom_sdk_def.h`'s `SDKError` enum from
   `SDKERR_SUCCESS = 0`). The spec inferred this by analogy with
   `IMeetingProductionStudioController`; it is now measured.

The refusal is **clean**: the ladder stops at the synchronous error, sends no audio,
creates no channel, and leaves the phase terminal (a subsequent probe was accepted
rather than reporting `busy`, confirming no wedge).

## Run 2 — CoreVideo as HOST

Meeting: the operator's personal meeting room, 2 participants (operator + CoreVideo).
Target: `John Wallace | 1338`.

```
21:47:27.137  stage=controller                    ok=true
21:47:27.137  stage=meeting_supported             supported=true
21:47:27.137  stage=set_event                     code=0
21:47:27.138  stage=create_channel                code=0
21:47:27.221  stage=create_channel_response       channel=CCE7ADDE-B3E0-4E4E-99C5-BBB066222872 error=0
21:47:27.221  stage=participant_talkback_support  name="John Wallace | 1338" user_id=16778240 supported=true
21:47:27.221  stage=invite                        user_id=16778240 code=0
21:47:27.339  stage=invite_response               user_id=16778240 error=0
21:47:27.339  stage=background_volume             code=0
21:47:27.345  stage=send                          buffer=0 code=0
21:47:30.505  stage=sent                          buffers=300
21:47:30.516  stage=destroy                       code=0 attempt=1
21:47:30.563  stage=destroyed                     error=0
```

Every rung green. Channel created, invited, ducked, 300 buffers sent, destroyed on the
first attempt with no retry and no stray.

**Human confirmation:** the operator (the invited participant) **heard the tone**, and
reported it "warbles a bit".

---

## Measured: the tone is fed ~5% slower than real time

`send` at 21:47:27.345 → `sent` at 21:47:30.505 = **3.160 s wall for 3.000 s of audio**,
i.e. 10.53 ms spent per 10 ms buffer, **94.9% of real time**. Zoom's jitter buffer
starves gradually and the operator hears warble.

**This corrects the final review's estimate.** That review predicted ~64% of real time,
reasoning from Windows' default ~15.6 ms timer granularity against a `sleep_for(10ms)`
loop. Measured reality is 95% — something in the process (almost certainly OBS) has
already raised the system timer resolution, so the sleep lands near its nominal value.
The *diagnosis* stands (sleep-based cadence rather than deadline-anchored pacing); the
magnitude was wrong by a wide margin. Milestone 2's fix is unchanged — anchor each
buffer to `start + n*10ms` off a real clock, the same treatment `iso-video-pacer.h`
already gives ISO video — but it is a 5% correction, not a rewrite.

## NOT proven, and deliberately recorded as such

**Exclusivity.** Run 2 had only two participants: CoreVideo and the invited operator.
With nobody uninvited, there was no control participant to confirm they heard *nothing*.
The claim "only the invited participant hears talkback" — the core privacy property of
the whole feature — remains **unverified**. It needs a meeting with at least one
uninvited human. Do not treat it as tested.

## Incidental findings

- `IUserInfo::IsSupportTalkback()` returned **true** for the operator's client. The
  reporting of this value was added in the final review's fix wave (F2) specifically so
  "talent heard nothing" could be told apart from "talent's client cannot receive
  talkback" — it paid off on its first live run by removing that ambiguity outright.
- The driving thread called Zoom SDK APIs **off the message-pumping thread** (the first
  thread in this engine ever to do so) with no failure, no send error, and no engine
  crash. The thread-affinity worry raised by the final review is not borne out here,
  though a single 3-second run is weak evidence.
- Channel destroy succeeded on `attempt=1`, so the retry chain and the stray-channel
  queue were never exercised. Both remain untested in anger.
- Re-triggering the probe after a completed run was accepted normally (no false `busy`),
  confirming the re-entrancy guard and the driver-exit path compose correctly.

## Decision

**Gate passed. Milestone 2 may start.** Carry forward:

1. Deadline-anchored tone/audio pacing (measured 5% slip above).
2. The parked `leave`-mid-probe wedge (one-line fix: reset the phase on the Leave path).
3. An exclusivity test with an uninvited participant, as early as a body is available.
