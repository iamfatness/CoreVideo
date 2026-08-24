# Zoom Talkback Design

**Status:** approved design, not yet implemented
**Date:** 2026-08-24
**Goal:** Let a director talk privately to talent inside a Zoom meeting — to everyone nominated at once, or to one person — from a Stream Deck, the control API, or a keyboard, without a word of it ever reaching program or the ISO recordings.

---

## Why

Zoom shipped talkback in ZoomISO. It is the feature a director reaches for constantly and the one thing CoreVideo cannot do at all: **CoreVideo has never sent audio anywhere.** Every media path in the codebase runs engine → plugin. `engine/src/engine-audio.cpp` receives; `src/zoom-source.cpp` renders; nothing anywhere calls `setExternalAudioSource`, constructs an `IZoomSDKAudioRawDataSender`, or touches any outbound audio API. Talkback is the first path in the other direction, and that — not the SDK call — is the design problem.

The operator need is the ordinary one from live production: brief the panel during a package, cue a guest before their question, tell one person their camera is off, without the audience or the other participants hearing any of it.

---

## What the SDK gives us

Verified against the headers this repo already builds against — Zoom Windows Meeting SDK **7.1.5.43953**, `ZOOM_SDK_DIR` per `build_x64/CMakeCache.txt:420`. Talkback itself shipped in Meeting SDK **7.0.0** ("Support talkback audio feature"); we are two minor versions past it.

`IMeetingService::GetMeetingTalkbackController()` (`meeting_service_interface.h:1318`) returns `IMeetingTalkbackController` (`meeting_service_components/meeting_talkback_ctrl_interface.h`). It is a genuine broadcast-talkback design, not a virtual-microphone workaround:

| Capability | Signature | Constraint |
|---|---|---|
| Create channels | `CreateChannel(count)` | **Max 16 channels** |
| Membership | `BeginBatchInviteUsers` → `AddUserToInvite` → `ExecuteBatchInviteUsers` | **Max 10 users per channel**, asynchronous, confirmed per user |
| Send audio | `SendAudioDataToChannel(channelID, pcm, len, sampleRate, channel)` | PCM 16-bit, mono/stereo, 32 kHz or 48 kHz recommended |
| Duck the meeting | `SetChannelBackgroundVolume(channelID, 0.0–2.0)` | Lowers *main meeting* audio for people in the channel |
| Meeting gate | `IsMeetingSupportTalkBack()` | — |
| Per-user gate | `IUserInfo::IsSupportTalkback()` (`meeting_participants_ctrl_interface.h:181`) | — |

Callbacks on `IMeetingTalkbackCtrlEvent` cover create/destroy/join/leave responses with a `TalkbackError` code, plus `onJoinTalkbackChannel`, `onLeaveTalkbackChannel`, and `onInviterAudioLevel(inviterID, 0–15)`.

`SetChannelBackgroundVolume` deserves note: it is the dim/duck of a real talkback panel, delivered by Zoom rather than built by us.

### The one thing that is inference, not fact

Neither the headers nor Zoom's documentation state what entitles talkback. The 7.0.0 changelog says only *"Support talkback audio feature"* and lists Permission denied among the error codes. Our working assumptions:

1. **In-meeting role: host or co-host.** Supported by `TALKBACK_ERROR_NOPERMISSION` and by the sibling controller in the same SDK family, `IMeetingProductionStudioController`, which documents its equivalent outright: *"Only host or co-host can start production studio mode."*
2. **Account entitlement: Zoom Enhanced Media.** `IsMeetingSupportTalkBack()` is meeting-level; Enhanced Media is what licenses the Liminal apps (ZoomISO, ZoomOSC, Tiles), and talkback shipped in ZoomISO. Zoom states only the host needs the add-on. **The account CoreVideo signs in as is Enhanced Media entitled**, so this is expected to be satisfied — but it is unproven.

A standalone probe was considered and deliberately skipped. Instead **Milestone 1 is a thin live vertical slice** that proves the SDK path before anything is built on top of it (see Milestones). If either assumption is wrong, it fails there, at the cheapest possible point.

---

## Requirements

Decisions taken during design, all locked:

| Decision | Choice |
|---|---|
| Audio source | An **OBS audio source/track** — reuses OBS device handling; no capture code in the plugin |
| Topology | One **"all talent" channel + per-person override** |
| Keying | **Push-to-talk and latch**, both available |
| Surfaces | Companion/Stream Deck, TCP/OSC control API, OBS hotkey. **Not** the dock |
| Program/ISO leak | **Hard structural guarantee**, plus a warning when the chosen source is on a program track |
| Failure behaviour | **Fail closed everywhere, always.** A latch does not survive a reconnect |

---

## Architecture

Five components; one new shared-memory region.

```
Companion / OSC / hotkey
        │  talkback_key {target, mode}
        ▼
  plugin keying state ──► attach OBS tap ──► PCM 48k/16-bit
        │                                        │
        │  P2E: talkback_open + renewals         ▼
        │                                  talkback SHM ring
        ▼                                        │  notify edge
  engine talkback controller ◄───────────────────┘
        │
        └─► SendAudioDataToChannel() ──► only invited users hear it
```

Program and ISO do not appear in that diagram. That is the leak guarantee, structurally.

### 1. Talkback tap — `src/talkback-tap.{h,cpp}` (plugin)

Given an OBS source name, attaches `obs_source_add_audio_capture_callback` and receives `struct audio_data` post-processing. Converts planar float to interleaved 16-bit PCM at 48 kHz — OBS's native rate here and one of the two the SDK recommends.

A capture callback is a **tap, not a route**: it observes a source's audio and cannot add that source to any mix. This is what makes the leak guarantee structural rather than a promise.

The tap is attached only while a key is open and detached the instant it closes. An unkeyed talkback source costs nothing.

### 2. Talkback ring — `ZoomObsPlugin_<uuid>_talkback`

A new shared-memory region using the existing `ShmAudioHeader` (`src/engine-ipc.h:157`) unchanged. Only the direction is new: **plugin creates and writes, engine opens and reads.**

That already works. `shm_region_create` (`src/engine-ipc.h:361` Windows, `:454` POSIX) and `shm_region_open_readwrite` (`:402` / `:534`) exist for both platforms, and the read-*write* open exists precisely because a reader must be able to clear the `notify` flag. Naming goes through `shm_region_name` (`:328`) with its generation suffix — the fix for the Windows "named sections cannot grow while mapped" deadlock — so talkback inherits that correctness for free.

The notify protocol is unchanged; the edge event simply travels **P2E** instead of E2P. `audio_ring_notify_after_publish` (`:263`), `audio_ring_reader_done` (`:277`) and `audio_ring_reader_abandon` (`:292`) are free functions over a header pointer with no baked-in direction. We call them with the roles swapped. **No new concurrency design, and the seq_cst fence pair is not touched.**

Ring geometry matches the inbound rings: 8 slots, per-slot seqlock, free-running `write_index`.

**Why not the pipe.** Talking produces ~100 buffers/sec. This codebase has already measured what that shape does to the P2E/E2P pipes: engine→plugin latency of 58–90 ms under full gallery load versus 41–161 µs idle, with ring overruns at zero throughout — the ring never fell behind, the wakeups did (`src/engine-ipc.h:173-183`). That incident is why the ring exists. Talkback is the one feature where that latency is heard directly as a stutter in the director's voice.

### 3. Engine talkback controller — `engine/src/engine-talkback.{h,cpp}`

Owns `IMeetingTalkbackController` and the `IMeetingTalkbackCtrlEvent` sink. Drains the ring fully on every wakeup — per the standing invariant that **media events are prompts, not payloads** — and calls `SendAudioDataToChannel` for each channel the active target maps to. Owns channel creation, membership, gating, and the dead-man timer.

### 4. Keying state — `src/talkback-key.h` (plugin, header-only logic)

The decision logic extracted from Qt and libobs so it can be tested directly, the same treatment `src/join-watchdog.h` and `src/director-handover.h` get, and for the same reason: both failure directions are invisible until they happen on a live show.

### 5. Channel planner — `src/talkback-plan.h` (pure function)

Maps a nominated talent list onto channel IDs under the 16/10 caps. No SDK, no I/O.

---

## Keying and the dead-man switch

**Split of authority.** The plugin owns *intent* (target, PTT or latch); the engine owns the *open channel*. The engine never learns what a latch is — only "keyed to target X" and "renewals still arriving". This split is what makes a latch unable to survive anything the plugin doesn't survive.

**The renewal is the audio itself.** While a key is open the OBS tap delivers buffers continuously, including silence, because an active OBS source calls back whether or not anyone is talking. The ring's own traffic is therefore the liveness signal. The engine holds the channel open only while fresh buffers land and closes it after a gap (**default 250 ms**, a few buffer periods, configurable).

Nothing has to *notice* a failure:

| Failure | Why it closes |
|---|---|
| OBS quits / plugin crashes | Buffers stop; gap expires |
| Engine restarts | Channel died with the process; key does not reopen |
| Pipe drops | Notify edge stops; gap expires |
| Talkback source removed or goes inactive | Buffers stop; gap expires |
| Meeting rejoin | Channel and membership are meeting-scoped; both are gone |
| Operator releases the key | Explicit `talkback_close`, instant |

Normal release still sends an explicit close so the common path is deterministic. The gap is the backstop, not the mechanism.

### The failure the dead-man cannot catch

A **lost button release while the socket stays healthy**. Buffers keep flowing, the gap never expires, and the director is live to talent without knowing. So renewals must also run controller → plugin: a key opened over the control API stays open only while the controller keeps renewing it. Miss two renewals (default renewal 500 ms, so ≤1 s to close) and the plugin closes the key itself.

This is a **liveness renewal, not a maximum-open-time cap**. A deliberate latch may stay open indefinitely as long as something actively asserts it is still wanted.

For the OBS hotkey the release is in-process and reliable; we additionally close on OBS losing focus, so alt-tabbing mid-hold cannot leave a key open.

### Tally

Dock and Companion show open/closed, the target, and a level meter computed from **our own PCM as we write it** — not from `onInviterAudioLevel`, which is a callback *to* channel members about the inviter and is not ours to rely on as the sender.

The tally reflects the **engine's confirmed state, never the plugin's intent**, so a key that failed to open never shows as live.

---

## Channels, identity and the caps

16 channels, 10 users each. These two numbers drive the design.

### A target maps to one *or more* channels

"All talent" with a 24-person panel cannot be one channel — it is three, and the same PCM fans out to all three in one drain pass. The controller works in terms of a **target** (a named set of people); each target owns `ceil(n/10)` channel IDs. This falls out of the caps rather than being extra machinery, and panel size stops being a special case.

### Private channels are pre-provisioned

Invites are asynchronous and confirmed by callback. Swapping one member in and out of a shared "private" channel at key time would mean the director presses *talk to Sarah* and the first words are clipped waiting for `onChannelUserJoinResponse`.

Instead each nominated participant gets a standing channel with exactly one member, provisioned at nomination. Keying private is then **choosing a channel ID** — no membership change, no latency, no clipped words.

Budget: 24 talent = 3 all-talent + 13 private, inside the 16 cap. When it runs out we **report which people have no private channel** rather than silently dropping them.

### Identity is by name

Zoom user IDs are meeting-scoped. A nominated list holding raw IDs points at nobody after a rejoin and at the wrong person once IDs are recycled — the exact defect the Companion module fixed in v0.1.44. The nomination list stores **names**; on `roster_changed` we re-resolve, invite anyone newly present, drop anyone gone. A rejoin rebuilds membership from names automatically.

### Gates are surfaced, never swallowed

- `IUserInfo::IsSupportTalkback()` is per participant. A director who briefs someone who never heard a word is worse than a feature that refuses to arm, so unreachable nominees are shown as unreachable in the dock and the tally.
- `IsMeetingSupportTalkBack()` and `TALKBACK_ERROR_NOPERMISSION` gate the feature. When unavailable it greys out **with the actual reason**, and `talkback_status` reports it over the control API so Companion can show it too.

---

## The leak guarantee

Two independent halves, because our side alone is only half the promise.

**Structural (ours).** A capture callback cannot route a source into a mix, and ISO records inbound audio only — talkback is outbound and never enters an ISO path. Pinned by `talkback-isolation-test` so a later refactor cannot quietly undo it.

**Advisory (OBS's).** If the operator picks a source that is *itself* live on a program track, their voice reaches the stream through OBS's own routing, entirely outside our path. We read the source's enabled mixer tracks and **warn in the dock**. The recommended configuration — all program tracks unchecked in Advanced Audio Properties — is documented in the dock's help text.

Without the advisory half the guarantee is only half true. Both ship together.

---

## Control surfaces

### Control API (line-JSON, `127.0.0.1:19870`)

| Command | Purpose |
|---|---|
| `talkback_config` | Choose the OBS audio source |
| `talkback_nominate` | Set the talent list, by name |
| `talkback_key` | `{target, mode: ptt\|latch, state: on\|off}` |
| `talkback_renew` | Liveness renewal for an open key |
| `talkback_status` | State, target, gating reason, per-nominee reachability |

Events: `talkback_state`, `talkback_unavailable`. OSC maps onto the same verbs, consistent with the existing OSC surface.

### Companion

`talkback_talk` wired to **press and release** for true push-to-talk; `talkback_latch` as a separate toggle; a target dropdown reusing the by-name resolver shipped in v0.1.44, so a talkback button survives a rejoin exactly like an assign button. `talkback_live` feedback turns the key red while open, driven by the engine's confirmed state. Variables `$(cv:talkback_target)` and `$(cv:talkback_state)`. The module renews on a timer while a key is held — this is what makes a lost release fail closed.

### OBS hotkey

Two entries via `obs_hotkey_register_frontend`: talk to all talent, talk to selected. Release in-process, plus close on focus loss.

### Dock

Configuration and tally only, by operator preference — no talk button. Source selection, nomination, per-person reachability, live tally and meter, program-track warning.

---

## Testing

House pattern: plain executables, no framework, one `check()`-style file per invariant cluster, pinning invariants rather than implementations.

| Test | Pins |
|---|---|
| `talkback-key-state-test` | PTT/latch transitions, renewal expiry, lost release closing after missed renewals, every row of the failure table closing the key |
| `talkback-plan-test` | `ceil(n/10)` fan-out, 16-channel budget, which nominees get private channels, overflow reported not dropped |
| `talkback-ring-test` | Ring driven in reverse — notify and seqlock invariants hold with roles swapped |
| `talkback-pcm-test` | Planar float → interleaved 16-bit at 48 kHz |
| `talkback-isolation-test` | Talkback PCM structurally cannot enter program or ISO |

**Live verification is required before this ships**, in the house style — real meeting, numbers recorded: a nominated participant hears the tone and confirms it; a second, non-nominated participant confirms they hear nothing; program and ISO recordings from the same session contain no talkback audio; and each row of the failure table exercised live, not only in tests.

---

## Milestones

1. **Thin live vertical slice.** Engine only: get the controller, `IsMeetingSupportTalkBack()`, create one channel, invite one participant, send a generated tone, destroy. **Proves the two unproven assumptions (host/co-host, Enhanced Media) before anything is built on top.** Throwaway UI, real SDK path.
2. **The ring, in reverse.** Region, plugin writer, engine reader, notify over P2E. Tests first.
3. **The tap and PCM conversion.** OBS source → 48 kHz 16-bit → ring.
4. **Keying state machine and dead-man.** `talkback-key.h`, both renewal directions, failure table green.
5. **Channels, planner, identity.** Targets, fan-out, pre-provisioned private channels, by-name re-resolution on roster change.
6. **Control surfaces.** Control API + OSC, then Companion, then hotkey.
7. **Dock configuration, tally, and the program-track warning.**
8. **Live verification pass** against the checklist above.

Milestone 1 is a gate: if it fails, the remaining milestones do not start.

---

## Risks

- **Entitlement (highest).** If talkback needs something beyond host/co-host plus Enhanced Media, Milestone 1 fails and the feature may be unbuildable on this account. Deliberately front-loaded.
- **The 10-user cap** makes a large panel consume channels quickly. Mitigated by fan-out and explicit overflow reporting; a panel beyond 16 channels' worth of talent cannot have full private coverage, by Zoom's design, not ours.
- **OBS buffering** sits between the microphone and the ring. If measured latency proves too high for comfortable direction, the fallback is direct WASAPI capture in the plugin — an approach considered and set aside during design, not a redesign.
- **macOS.** The engine on mac is scaffold only (`CMakeLists.txt:429-448`); talkback lands Windows-first like the rest of the engine. The shared-memory helpers used here already have POSIX implementations, so nothing in this design blocks the mac port further than it already is.
