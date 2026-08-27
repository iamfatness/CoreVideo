# Talkback pre-provisioned channels — live gate runbook (Task 6)

**Status: NOT RUN.** The build phase of `docs/superpowers/plans/2026-08-25-talkback-preprovisioned-channels.md`
is complete (all five tasks, final whole-branch review + verification: SHIP), but nothing
below has been measured against a real meeting. The milestone is not done until this gate
passes. Draft PR #231 stays draft until then.

## Why this gate exists

The whole milestone removes one measured defect: creating the channel ON the key press cost
a create+invite round-trip before audio could flow — logged live 2026-08-25 as
`no_channel_drops` on every press, i.e. the director's first words discarded every time.
Keying now only *selects* a channel provisioned at nomination. **The gate's first job is to
re-measure that.** Everything else is secondary.

## Setup — read this, it is what burned the 2026-08-25 attempt

The SDK client and the operator's Zoom **must be different Zoom accounts**. On 2026-08-25
they shared one account and displaced each other into separate PMI sessions; every invite
then targeted a user id in the engine's own dead session and returned `SDKERR_WRONG_USAGE`.
A control run of the known-good M1 probe failed identically — that is what proved the code
was not at fault.

Required:
- Engine signed in as the entitled account, **host or co-host**.
- A second device (phone) joined **as a guest — not signed in** — as the invited talent.
- A third guest (incognito browser) as the **uninvited control**. Exclusivity — "only the
  invited participant hears talkback" — has NEVER been verified, on any milestone.

Install: both binaries as a pair (DLL + `zoom-runtime\ZoomObsEngine.exe`), SHA256-verified,
OBS closed, elevation required. A DLL-only copy half-applies engine-side fixes — and one of
this milestone's fixes (`layout_mismatch` refusing the key instead of lying) exists
precisely because that mistake is routine.

Drive it over the control API (`127.0.0.1:19870`):
`talkback_nominate {"nominees":[...]}` → poll `talkback_status` for the `nomination` field →
`talkback_key {"state":"on","target":"all"|"<name>"}`. Engine-side stages arrive as OBS log
lines (`talkback_nominate: ...`), not on the socket.

## The measurement (gate pass/fail)

Speak a counted count-in ("one, two, three, four") the instant the OPEN cue sounds. The
invited guest reports the first word heard. Baseline 2026-08-25: the start of every press
was lost. Pass = the first syllable is there, repeatedly, including on rapid re-keys.

Note: `no_channel_drops` can now legitimately appear on a press — the tap opens before the
selection lands, so buffers crossing in that window are counted, not lost. Read it as a
measurement, not automatically a fault.

## Adversarial probes, ranked (from the review chain)

1. **Re-nominate mid-ladder, then key both lists.** 11+ names so the ladder is slow; send a
   second `talkback_nominate` before the first completes. Key an original name and a
   new-list name. Expected now: the record follows attempt identity; a superseded ladder
   completing may leave a deliberate fail-closed "no one has been nominated yet" state —
   **that is the fix working**; one re-nominate recovers. The failure to watch for:
   `nomination.requested` showing list B against ladder A's channel count.
2. **Key "all" mid-ladder, then a create failure.** Nominate 17+ names (privates run into
   the 16-cap). Key "all" as soon as its slices exist and HOLD it. If a create then fails:
   expected = the key CLOSES itself (CLOSE cue, tally drops, `session_state live:false
   reason:channels_destroyed`). The pre-fix behaviour was key open / tally red / zero audio.
3. **`nominate → leave → nominate` inside 10 s.** One operator mis-click; produced a defect
   in three consecutive review rounds. Expected: second nomination refused `create_busy`
   cleanly, then works after the timeout; no later `already_provisioned` wedge.
4. **Drop/rejoin a nominee — once normally, once during a `talkback_probe`.** Then key them
   privately. Check `members_present` against whether they *actually hear you*, and confirm
   a `member_invited` line for the NEW uid appears after the probe ends (it rides the next
   roster event — very likely, not guaranteed; if absent, that gap is the finding).
5. **Exclusivity.** While keyed to one guest, the uninvited control must hear NOTHING, and
   program/ISO recordings must contain no talkback audio.
6. **A `channel_stale` line in any log = a review derivation is wrong** (that state is
   believed unreachable). Grab the full log; it is the single most valuable diagnostic the
   gate can produce.

## Known-accepted residuals (do not re-litigate at the desk)

- Two creates can transiently exist *at Zoom* (not at the arbiter) after an expiry; orphans
  are destroyed on arrival.
- A failed-invite talent who rejoins under a new uid with no observed absence edge is not
  auto-retried (`failed` is name-keyed); count stays honest, re-nominate recovers.
- A participant literally named "all" (exact, case-insensitive) is refused at nomination.
- Control characters in display names: refused, documented at the emit site.

## Gate run 1 — 2026-08-26 20:04 (real meeting) — FAILED, fixed, re-gate required

**Finding: Zoom rate-limits back-to-back `CreateChannel` calls, and the ladder had no
spacing at all.** One nominee plans 2 channels (all-talent + private). Channel 1 was
created and its nominee invited; then the ladder issued channel 2's `CreateChannel`
**synchronously from inside channel 1's `onCreateChannelResponse`** — both log lines
stamped `20:04:37.291`, a 0 ms gap — and Zoom refused it with code **18 =
`SDKERR_TOO_FREQUENT_CALL`** (verified as enum position 18 in `zoom_sdk_def.h`). The
ladder then aborted terminally and correctly (`channels_destroyed:true` — the Task 5
teardown/report machinery all worked, which is the one good news in the trace):

```
20:04:37.193 stage=plan channels=2 all_talent_complete=true attempt=1
20:04:37.193 stage=create_channel code=0
20:04:37.291 stage=channel_created channel=727C... is_all_talent=true members=1
20:04:37.291 stage=invite name="Random User" code=0
20:04:37.291 stage=create_channel code=18        <-- TOO_FREQUENT_CALL, 0ms after response
20:04:37.291 stage=channel_destroyed
20:04:37.291 stage=nominate ok=false reason=create_channel_failed channels_destroyed=true
```

Every real talent list plans more than one channel, so **no nomination could ever have
succeeded live**. No unit test could have caught it: the fake controller has no rate limit
and no notion of elapsed time between calls. This is exactly the class of finding a live
gate exists for.

**Fix (same day, on `feat/talkback`):**

- The ladder is **paced**. `onCreateChannelResponse` no longer calls
  `nomination_create_next()`; it arms a not-before deadline
  (`kNominationCreateSpacing = 300 ms`) and `EngineTalkback::nomination_tick()` issues the
  create when it comes due.
- `nomination_tick()` is **not** `tick()`, and cannot be. `tick()` has exactly one driver:
  the thread `main.cpp` spawns when `probe()` returns true. During a nomination that thread
  does not exist (`main.cpp` joins it before `nominate()`, and `nominate()` refuses while
  `has_pending_work()`), so a create scheduled into `tick()` would never be issued — and if
  it were, it would be issued off the **probe's** thread, breaking both
  "CreateChannel is command-loop-thread-only" and fact 2 of `tick()`'s batch-destroy chain.
  The pump instead rides the command loop's existing 50 ms idle turn: the
  `MsgWaitForMultipleObjects` timeout inside `ipc_read_line_with_message_pump()`, which now
  takes an `on_idle` callback. 50 ms granularity against 300 ms spacing.
- **Code 18 is a wait, not a failure.** It backs off and retries the SAME channel
  (`kNominationRateLimitBackoff = 500 ms`, doubling: 500/1000/2000/4000) with a per-channel
  cap of `kMaxNominationCreateRetries = 4`. Every other synchronous failure keeps the
  terminal abort. Cap exhaustion is terminal with reason **`create_rate_limited`**, not the
  generic `create_channel_failed` — run 1 spent its first pass suspecting permissions and
  channel budget for a problem that was neither.
- The arbiter claim is **taken at issue, not held across the wait**: a scheduled create is
  not outstanding (Zoom has never seen it), and claiming early would arm
  `m_nomination_create_deadline` (`kAwaitTimeout`) against a request never issued, so a
  spacing wait would self-expire the ladder it is pacing. The ~300 ms window that opens is
  closed on the one path that could abuse it — `nominate()` now refuses `create_busy` while
  a create is *scheduled* as well as while one is outstanding, the same non-destructive
  early refusal — and left open on the other (a probe taking the arbiter ends the ladder
  through the existing, tested `nomination_abort_ladder("create_busy")` path).
- Cosmetic, from the same log: the raw `create_channel_response` trace line reported under
  `"cmd":"talkback_probe"` during a nomination ladder. Moved below the arbiter claim and
  routed by owner (the same `ReportSink` split `resolve_participant()` already does).

**New tests** in `tests/engine-talkback-select-test.cpp` (engine TU): spacing is not issued
inside the response nor before its deadline; code-18 retries the same channel and the
ladder completes; retries exhausted gives exactly one terminal abort naming the rate limit;
a non-18 failure still aborts immediately with no retry; and — found by mutation, not by
review — a re-nomination inside the spacing window is refused without touching the running
ladder. Four mutants proved: unpace the ladder, delete the 18 retry, delete the retry cap,
delete the scheduled-create half of `nominate()`'s gate. All four fail deterministically.

**What run 2 must re-test.** Everything in "Adversarial probes" above is untested: run 1
never got past the first nomination. Add two:

7. **Watch the gaps.** A multi-channel nomination should now show ~300 ms between
   consecutive `create_channel` lines. If any `code=18` still appears at that spacing,
   **raise `kNominationCreateSpacing`** rather than leaning on the retry — Zoom publishes no
   rate for this and 300 ms is an engineering guess with margin, not a known limit.
8. **Time a big plan.** 11 nominees = 13 channels ≈ 4 s of provisioning. That is paid once,
   at nomination, never at key time — but confirm the operator is not tempted to key during
   it (a key mid-ladder is refused `provisioning_incomplete`, by design).

## Gate run 2 — 2026-08-26 20:25-21:02 (real meeting) — **PASSED**

Setup that finally worked: engine joined the PMI via control API as "OBS" (own OAuth
ZAK), operator's phone as guest "Random User" (not signed in). One nominee → plan of 2
channels (all-talent + private), provisioned by the PACED ladder — the run-1
`TOO_FREQUENT_CALL` fix held: `channels:2, done:true, has_private_channel:["Random User"]`.

**The core measurement: PASSED.** Operator spoke "ONE, two, three, four" from silence on
a fresh key press; the invited guest heard **"ONE" clearly, nothing clipped**. Baseline
2026-08-25: the start of every press was lost. Then three rapid key cycles: all three
words heard, each keyed onto the SAME standing channel (`F147F48D…` in all three
`session_live` lines) — select-on-key working end to end.

**Second product defect found and fixed live: stereo is silently discarded** (commit
`0589d29`). The mono probe tone was heard; the stereo session voice was not, with every
`SendAudioDataToChannel` returning `SDKERR_SUCCESS` — proven by the ring probe
(`peek-talkback.py`: voice at peak ~4900 against a ~10 noise floor flowing tap → ring →
drain while the guest heard silence). The SDK header says "mono or stereo"; reality is
stereo-accepted-never-heard. The engine now downmixes at the SDK boundary.

**Operational truths learned at the desk (they cost ~30 min of debugging):**
- **The tap's source must sit on ≥1 mixer track.** Setting all six tracks off stopped
  libobs's audio-capture callbacks instantly (dead-man closed the key at the moment the
  tracks went off — measured, not inferred). The working setup: a DEDICATED
  `Talkback Mic` source on an otherwise-unused track (track 6 here), keeping talkback
  off program structurally while callbacks still flow.
- **Studio Mode duplicates the program scene at transition time** — a source added to
  the live scene object is NOT in the program's copy until the next transition, and
  `open()`'s obs_source_active refusal fires (correctly). Transition once after adding.
- The dead-man switch caught every real dead-path (hardware-muted GoXLR, zero-track
  source, OBS still settling) exactly as designed; every "mystery" close was it working.
- The probe's tone warble/cutouts ("it cuts out") = the KNOWN deferred 94.9%%-of-real-time
  probe pacing defect, probe-only; the session path (clocked by OBS delivery) had no
  dropouts across ~15 s of continuous speech.
- GoXLR rigs: the mic lives on "Chat Mic (TC-HELICON GoXLR)", and the GoXLR's hardware
  mute silences it while WASAPI keeps the stream alive (delivering zeros) — a key can
  hold open on silence, which is correct fail-open behaviour for a mute (publish
  silence, keep the path alive).

**NOT tested, carried forward:** exclusivity (no third uninvited device was available)
— still the one untested promise; first item next session. The adversarial probes
(re-nominate mid-ladder, forced create failure under a live "all" key, drop/rejoin
during a probe) also remain for a longer session.
