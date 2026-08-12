#pragma once
//
// zoom-tile-retry.h — retry pacing for the CoreVideo Tiles "revive a silent
// slot" sweep, as pure logic (unit-tested by tests/tile-retry-test.cpp).
//
// The sweep re-issues the subscription for any slot that has not produced a
// frame under its *current* assignment, so a tile cast at somebody who had not
// joined yet fills in when they arrive. It runs from the tiles source's roster
// callback, which fires on the engine reader thread and is driven by both
// "participants" and "active_speaker" events — and those fire continuously in a
// live meeting (every mute, unmute, camera toggle, name change, and speaker
// change; onUserActiveAudioChange in the engine emits an active_speaker line
// *and* a participants line for a single speaker change).
//
// Unpaced, each of those events took engine_mutex, ctx->mutex and every
// feed->mtx and issued a blocking pipe write per silent slot, all while holding
// the roster callback gate on the reader thread that dispatches frames for every
// source in the plugin. For a slot cast at an absent participant the engine
// answered each retry by building a fresh ParticipantSubscription, attempting
// createRenderer + setRawDataResolution + subscribe at 720p, failing, destroying
// the renderer and retrying at 360p — indefinitely, with no backoff, against a
// renderer destroy/recreate path this project already treats as hazardous.
//
// Two independent bounds, both required:
//
//   1. A minimum interval between whole sweeps. This is what actually decouples
//      the cost from the event rate, and it is why the sweep is NOT filtered to
//      roster-only events: the callback carries no discriminator, adding one
//      would change the RosterCallback signature shared with ZoomSource, and it
//      would not help anyway — a participants line accompanies most
//      active_speaker changes, so both event streams are frequent. Rate is the
//      problem; rate is what is bounded.
//
//   2. A per-slot exponential backoff and a hard attempt cap. After the cap a
//      slot stops retrying and stays silent rather than hammering the SDK. The
//      budget is tied to the slot's assignment epoch, so repointing a slot —
//      including the automatic reassignment an Auto-mode roster change performs
//      — hands it a fresh budget.
//
// The shape deliberately mirrors recover_stale_video() / stale_recover_cooldown_ns()
// in src/zoom-source.cpp rather than inventing a second retry idiom.
//
#include <cstdint>

// Minimum wall time between two sweeps, however many roster events land.
static constexpr uint64_t kTileSweepMinIntervalNs = 2'000'000'000ULL;

// First per-slot cooldown; doubles per attempt up to a 16x cap.
static constexpr uint64_t kTileRetryBaseCooldownNs = 10'000'000'000ULL;

// After this many attempts under one assignment, the slot is left silent. At
// the cooldowns below that is roughly five minutes of trying before giving up.
static constexpr uint32_t kTileRetryMaxAttempts = 6;

// 10s, 20s, 40s, 80s, then capped at 160s — the same curve as
// stale_recover_cooldown_ns() in zoom-source.cpp.
inline uint64_t tile_retry_cooldown_ns(uint32_t attempts)
{
    const uint32_t shift = attempts < 4 ? attempts : 4;
    return kTileRetryBaseCooldownNs << shift;
}

// True when a whole sweep may run now. last_sweep_ns == 0 means "never swept" —
// always due, so the first roster event after a cast still fills the wall
// promptly. A now_ns at or before last_sweep_ns (clock skew, or two events in
// the same tick) is treated as not due rather than wrapping the subtraction.
inline bool tile_sweep_due(uint64_t now_ns, uint64_t last_sweep_ns,
                           uint64_t min_interval_ns = kTileSweepMinIntervalNs)
{
    if (last_sweep_ns == 0) return true;
    if (now_ns <= last_sweep_ns) return false;
    return now_ns - last_sweep_ns >= min_interval_ns;
}

// True when one silent slot may be re-subscribed now. `attempts` is how many
// retries this slot has already spent under its current assignment.
inline bool tile_retry_due(uint64_t now_ns, uint64_t last_retry_ns,
                           uint32_t attempts,
                           uint32_t max_attempts = kTileRetryMaxAttempts)
{
    if (attempts >= max_attempts) return false;  // give up; leave it silent
    if (last_retry_ns == 0) return true;         // first try under this epoch
    if (now_ns <= last_retry_ns) return false;
    return now_ns - last_retry_ns >= tile_retry_cooldown_ns(attempts);
}
