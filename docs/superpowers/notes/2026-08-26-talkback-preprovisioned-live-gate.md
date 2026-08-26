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
