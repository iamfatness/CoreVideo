# Broadcast Audio Pipeline Design

**Status:** approved design, not yet implemented
**Date:** 2026-08-16
**Goal:** Make CoreVideo's audio lossless, sample-accurate and measurably in sync with video, to the standards live production actually holds itself to.

---

## Why

The operator reported "audio is very bad" during a live show on 2026-08-16. Investigation found three compounding defects, all of them structural rather than incidental.

### 1. The audio transport loses samples

`engine/src/engine-audio.cpp:224-236` writes every Zoom audio buffer into a **single shared-memory slot**, overwriting whatever is there:

```cpp
hdr->sequence = seq;              // odd = write in progress
memcpy(shm.ptr + sizeof(ShmAudioHeader), data->GetBuffer(), byte_len);
hdr->sequence = seq + 1;          // even = readable
```

The seqlock prevents the reader observing a *torn* buffer. It does nothing about **loss**. Zoom delivers ~100 buffers/second (10 ms each); if the plugin does not read between two writes, the earlier 10 ms is destroyed and unrecoverable. When the reader does hit a torn read it retries three times and then `if (!copied) return;` (`src/zoom-source.cpp`), silently discarding that buffer too.

This is a latest-value mailbox. It is the correct pattern for video — newest frame wins, a dropped frame is nearly invisible — and the wrong one for audio, where the ear detects a 10 ms hole instantly. Audio and video were given the same transport. That is the root mistake.

Measured context: the operator's box runs 70% CPU with 10 sources at 1080p, so reader stalls are routine, not exceptional.

### 2. There is no clock

Every audio publish site stamps the buffer with wall-clock arrival time:

| File | Line |
|---|---|
| `src/zoom-participant-audio-source.cpp` | 342 |
| `src/zoom-source.cpp` | 1698 |
| `src/zoom-audio-delegate.cpp` | 108, 135 |
| `src/zoom-interpretation-audio-source.cpp` | 48 |
| `src/zoom-share-delegate.cpp` | 42 |

Zoom's buffers are exactly 10 ms apart, but they cross an IPC pipe and a shared-memory hop, so *arrival* is jittery. Stamping arrival hands OBS a stream whose timestamps advance by 8 ms, then 14 ms, then 3 ms. OBS reconciles that against its own audio clock by stretching, dropping and resampling — continuously, not just at cuts.

There is no master clock anywhere in the audio path. Both vMix and Viz Engine have one.

### 3. Nothing can measure the A/V offset

Neither `ShmFrameHeader` (`src/engine-ipc.h:81`) nor `ShmAudioHeader` carries a timestamp. No latency instrumentation exists anywhere in the plugin. Any claim about lip-sync accuracy today is an assertion, not a measurement.

---

## What this is designed against

See the `broadcast-av-latency-standards` reference for the full table. The load-bearing points:

- **Lip-sync is asymmetric.** ITU-R BT.1359-1: audio leading is detectable at **+45 ms**, lagging only at **−125 ms**. **If we must err, err audio-late.**
- **EBU R37 per stage: +5 / −15 ms.** End-to-end ≈ +40 / −60 ms.
- **Maintain coincidence on the stream; do not pre-compensate for downstream delays.**
- **Broadcast sample rate is 48 kHz** — which is what Zoom delivers.
- **Delay audio to match video, never the reverse.** vMix offers video delay in frames and documents that it is rarely used: costs RAM, less precise for lip-sync.
- **Buffering audio slightly is correct, not a compromise.** vMix deliberately runs *larger* audio buffers (10–20 ms ASIO) because video processing is heavy; DAW-level ultra-low buffers would make audio arrive earlier than video and break sync.
- **Viz Engine ring-buffers audio in blocks**, ~10 ms default output delay, tunable via "Block Read Forward".

CoreVideo's video path is already the slow one (Zoom decode → engine → SHM → plugin → 33.4 ms OBS composite tick), so audio buffering spends slack that already exists. It moves us *toward* alignment.

---

## Design

### Section 1 — Lossless transport (ring buffer)

Replace the single-slot audio region with a lock-free single-producer/single-consumer ring.

```
header : { write_index, slot_count, slot_bytes, sample_rate, channels }
slots[N]: { sequence, capture_ns, byte_len, pcm[] }
```

The engine fills a slot then advances `write_index`. The plugin holds its own `read_index` and drains **every** slot between `read_index` and `write_index`, publishing them **in order**. A late reader catches up instead of losing audio.

**Sizing: N = 8 slots (80 ms at 10 ms/buffer).** This is *capacity*, not added latency — a reader keeping up sees roughly one slot of delay. It only costs latency while actually absorbing a stall, which is precisely when that is the desired behaviour.

**Overrun policy.** If the reader falls more than N slots behind, the writer overwrites oldest and emits a **hard error** (`cmd:"error", msg:"audio_overrun"`) carrying the number of slots lost. It must **not** block: the writer runs on the Zoom SDK callback thread, and blocking there risks the SDK dropping us entirely. Loss becomes loud and countable rather than silent and invisible — which is why this defect survived until now.

**Wire format.** `ShmAudioHeader` changes shape. Engine and plugin ship as a pair, so this is safe; there is no cross-version compatibility requirement. The `shm_generation` mechanism already handles region rebuilds.

### Section 2 — Master clock (`src/audio-timeline.h`)

A new pure header, testable without OBS/Qt/engine — the treatment `audio-subscription-state.h` and `director-handover.h` already receive, and for the same reason: it is arithmetic whose only failure symptom is bad audio on air.

```
anchor_ns  : set on the first buffer of a timeline
samples    : cumulative, monotonic
timestamp  = anchor_ns + samples * 1'000'000'000 / sample_rate
```

Output advances by exactly one sample period per sample regardless of arrival time. Arrival jitter stops mattering because arrival stops being consulted.

**Re-anchor only on a genuinely new timeline:**
- participant re-subscribe
- engine process restart
- sample-rate change

**Do NOT re-anchor on a gap.** A mute is not a discontinuity — it is silence, and silence has a duration the timeline must honour. Re-anchoring on gaps would let audio walk out of sync across a long show, which is exactly the drift EBU R37 exists to prevent.

**Underrun emits silence at the correct timestamp** rather than emitting nothing, so the timeline stays continuous through a mute.

### Section 3 — Operator delay trim

vMix operators routinely dial 20–100+ ms to land sync. CoreVideo currently offers nothing. This is the control every vMix operator will look for first.

Applied as arithmetic on the timestamp section 2 now computes — not a buffer:

```
publish_ts = anchor + samples * 1e9 / sample_rate + delay_ns
```

Zero memory cost, sample-accurate. OBS's async path already holds timestamped audio until its time comes.

- **Global default + per-source override.** vMix needs per-source because its sources are heterogeneous (USB mic vs SDI). CoreVideo's are homogeneous — every participant takes the identical Zoom → engine → SHM route — so the systematic skew is one number. Global fixes the common case with one control; per-source is the escape hatch, in the Output Manager row beside the existing audio channel and role controls.
- **Range 0–500 ms, 1 ms granularity**, matching vMix's precision and inside OBS's async buffering ceiling.
- **Limitation, stated plainly:** delay can only push audio *later*. If audio ever arrived after video the fix would be delaying video, which this design does not do, for the reason vMix documents. Given video is the slow path this should never bind.

### Section 4 — Measurement

Add `capture_ns` — the engine's `os_gettime_ns()` at the instant the Zoom SDK handed over the data — to **both** the audio ring slot and `ShmFrameHeader`. Both processes are on one machine and `os_gettime_ns()` is QPC-based, so it is a shared monotonic clock with no domain problem.

```
audio latency = audio_publish_ns - audio_slot.capture_ns
video latency = video_publish_ns - frame_header.capture_ns
A/V offset    = video latency - audio latency     <- the EBU R37 number

(The two latencies share a formula but read different headers: the audio ring
slot and ShmFrameHeader respectively.)
```

Surfaced in `list_outputs`, the Diagnostics dock, and the control API.

This makes the trim self-serving (the measured offset tells the operator what to dial) and incidentally answers "what is our render latency with OBS", which could not be answered on 2026-08-16 because no such instrumentation existed.

**Alignment is manual, measurement-guided.** The plugin shows the number; the operator sets the trim. This is vMix's model and it is predictable on air — nothing moves unless a human moves it. Auto-align and closed-loop alignment can be layered on later without redesigning any of the above; a control loop that adjusts audio timing mid-show can oscillate or pump audibly and should not ship before the manual path is proven.

---

## Out of scope

- Video delay (in frames). vMix documents it as rarely used; audio-delay is the correct lever.
- Automatic or closed-loop alignment. Deliberately deferred — see section 4.
- The remaining audio publish sites (`zoom-source.cpp`, `zoom-audio-delegate.cpp` x2, `zoom-interpretation-audio-source.cpp`, `zoom-share-delegate.cpp`) adopt the timeline **after** the participant path is proven on air. `zoom-participant-audio-source.cpp` goes first because it is what the operator actually listens to.
- Video ring-buffering. Video's latest-value mailbox is correct for video.
- The director cut's audio handover (commit `74e7d66`). It is complementary, already committed, and **still unverified** — `speaker_director_take` bypasses the preview-cut path, so the attempted verification on 2026-08-16 tested nothing.

## Verification

Sections 2 and 3 are fully unit-testable and must be:
- feed jittery arrival times, assert output advances at exactly one sample period
- assert a 5-second mute produces exactly 5 seconds of timeline (no re-anchor)
- assert re-anchor *does* occur on subscribe / restart / rate change
- assert the delay trim offsets the timeline exactly, with no drift over 10^6 samples

Section 1 needs a live meeting: assert `audio_overrun` count is zero across a loaded show, where "loaded" means the 10-source 1080p configuration that produced the original report.

Section 4 is self-verifying: the measured A/V offset is the acceptance criterion, checked against EBU R37's +5 / −15 ms per-stage window.

**Honest note on what tests cannot cover:** none of this proves the audio *sounds* right. The acceptance test is the operator listening on a real show. Every measurement above is necessary and none is sufficient.

## Risks

- **Wire-format change** to the audio SHM region touches engine and plugin together. A mismatched pair produces silence, not a crash — so the generation/version guard needs to fail loudly.
- **Ring sizing is a guess.** 8 slots is chosen from the observed stall behaviour on one box at 70% CPU. The overrun counter from section 1 is what turns that guess into data.
- **`os_gettime_ns()` cross-process monotonicity** is assumed from QPC. Worth a direct check before relying on the measured offset.
- This design will not fix choppy *video* (12–28 fps instability, measured pre-existing on v0.1.39). That is a separate concurrent-stream/throughput problem.
