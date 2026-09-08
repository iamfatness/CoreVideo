// Unit tests for the tiles silent-slot retry pacing in zoom-tile-retry.h.
//
// The regression these exist for: the sweep was unbounded. It ran on every
// roster and active-speaker event — continuous in a live meeting — and for a
// slot cast at somebody who had not joined it drove an unbacked-off
// renderer create/destroy loop in the engine, on the reader thread that
// dispatches frames for every source in the plugin.

#include "zoom-tile-retry.h"
#include "media-failure-state.h"

#include <iostream>

static int g_failures = 0;

static void check(bool ok, const char *what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

static constexpr uint64_t kSec = 1'000'000'000ULL;

int main()
{
    // ── Sweep interval ──────────────────────────────────────────────────────
    check(tile_sweep_due(0, 0), "the first sweep is always due");
    check(tile_sweep_due(5 * kSec, 0), "never-swept is due whatever the clock");
    check(!tile_sweep_due(100 * kSec, 100 * kSec - 1),
          "a sweep 1ns after the last is not due");
    check(!tile_sweep_due(100 * kSec, 99 * kSec),
          "a sweep 1s after the last is not due (min interval is 2s)");
    check(tile_sweep_due(100 * kSec, 98 * kSec),
          "a sweep exactly at the min interval is due");
    check(tile_sweep_due(100 * kSec, 10 * kSec),
          "a sweep long after the last is due");
    // A burst of roster events inside one interval collapses to one sweep.
    {
        const uint64_t last = 100 * kSec;
        int swept = 0;
        for (int i = 0; i < 50; ++i)
            if (tile_sweep_due(last + static_cast<uint64_t>(i) * kSec / 100, last))
                ++swept;
        check(swept == 0, "50 events within 0.5s of a sweep trigger no sweep");
    }
    // Clock skew must not wrap the subtraction into "due".
    check(!tile_sweep_due(50 * kSec, 100 * kSec),
          "a clock that went backwards is not due");
    check(!tile_sweep_due(100 * kSec, 100 * kSec),
          "the same instant as the last sweep is not due");

    // ── Per-slot backoff curve ──────────────────────────────────────────────
    check(tile_retry_cooldown_ns(0) == 10 * kSec, "attempt 0 cooldown is 10s");
    check(tile_retry_cooldown_ns(1) == 20 * kSec, "attempt 1 cooldown is 20s");
    check(tile_retry_cooldown_ns(2) == 40 * kSec, "attempt 2 cooldown is 40s");
    check(tile_retry_cooldown_ns(3) == 80 * kSec, "attempt 3 cooldown is 80s");
    check(tile_retry_cooldown_ns(4) == 160 * kSec, "attempt 4 cooldown is 160s");
    check(tile_retry_cooldown_ns(9) == 160 * kSec, "the cooldown caps at 160s");
    check(tile_retry_cooldown_ns(4) == tile_retry_cooldown_ns(1000),
          "the cap holds for any attempt count");

    // ── Per-slot retry gate ─────────────────────────────────────────────────
    check(tile_retry_due(0, 0, 0), "a slot that has never retried is due");
    check(tile_retry_due(1000 * kSec, 0, 3),
          "a fresh epoch retries immediately even after prior attempts");
    check(!tile_retry_due(105 * kSec, 100 * kSec, 0),
          "5s after attempt 0 is inside the 10s cooldown");
    check(tile_retry_due(110 * kSec, 100 * kSec, 0),
          "10s after attempt 0 is due");
    check(!tile_retry_due(115 * kSec, 100 * kSec, 1),
          "15s after attempt 1 is inside the 20s cooldown");
    check(tile_retry_due(120 * kSec, 100 * kSec, 1),
          "20s after attempt 1 is due");
    check(!tile_retry_due(50 * kSec, 100 * kSec, 0),
          "a clock that went backwards is not due");

    // ── The hard cap: a slot gives up rather than hammering ─────────────────
    check(tile_retry_due(10'000 * kSec, 100 * kSec, kTileRetryMaxAttempts - 1),
          "the last attempt inside the cap is allowed");
    check(!tile_retry_due(10'000 * kSec, 100 * kSec, kTileRetryMaxAttempts),
          "at the cap the slot stops retrying");
    check(!tile_retry_due(10'000'000 * kSec, 0, kTileRetryMaxAttempts),
          "the cap outranks a zeroed last-retry time");
    check(!tile_retry_due(10'000'000 * kSec, 100 * kSec, 1000),
          "far past the cap the slot stays silent");

    // A slot cast at an absent participant: count the retries it ever issues.
    // Unbounded, this loop is what hammered the SDK.
    {
        uint64_t now = 0;
        uint64_t last_retry = 0;
        uint32_t attempts = 0;
        int issued = 0;
        // One simulated hour of roster events, 10 per second.
        for (int tick = 0; tick < 36'000; ++tick) {
            now += kSec / 10;
            if (!tile_retry_due(now, last_retry, attempts)) continue;
            ++issued;
            last_retry = now;
            ++attempts;
        }
        check(issued == static_cast<int>(kTileRetryMaxAttempts),
              "an hour of events at 10/s yields exactly kTileRetryMaxAttempts retries");
    }

    // Active media Start is a no-op, so an explicit action must reopen the
    // independent tile budget. Ordinary roster/speaker ticks cannot do this.
    {
        uint64_t last = 100 * kSec;
        uint32_t attempts = kTileRetryMaxAttempts;
        MediaFailureState health;
        health.assign("tile", 42);
        health.fail("tile", 42, "video_subscribe_failed", 1, true);
        int subscriptions = 0;
        auto retry = [&](TileRetryTrigger trigger, uint64_t now) {
            if (tile_retry_claim(now, last, attempts, trigger)) {
                ++subscriptions;
                health.assign("tile", 42); // production subscribe preserves health
            }
        };
        for (int i = 0; i < 100; ++i) retry(TileRetryTrigger::Automatic, (1000+i)*kSec);
        check(subscriptions == 0, "exhausted automatic ticks never reset budget");
        const auto ticket = health.ticket("tile", 42);
        retry(TileRetryTrigger::Manual, 1100*kSec);
        check(subscriptions == 1 && attempts == 1, "manual action issues one retry from exhausted budget");
        check(health.size() == 1 && ticket == health.ticket("tile",42), "manual retry preserves assignment and actual failure");
        retry(TileRetryTrigger::Automatic, 1100*kSec+1);
        check(subscriptions == 1, "automatic tick after manual retry observes cooldown");
        for (int i = 1; i <= 100; ++i) retry(TileRetryTrigger::Automatic, (1100+i*200)*kSec);
        check(subscriptions == static_cast<int>(kTileRetryMaxAttempts), "manual action grants one bounded budget");
        health.delivered("tile",42,ticket);
        check(health.size() == 0, "only delivery clears recovered tile failure");
    }

    check(tile_retry_needed(true, true, TileRetryTrigger::Manual), "manual retry revives failed tile retaining displayed frame");
    check(!tile_retry_needed(true, false, TileRetryTrigger::Manual), "manual retry skips healthy displayed tile");
    check(!tile_retry_needed(true, true, TileRetryTrigger::Automatic), "ordinary ticks do not reopen displayed-tile retry budget");

    check(tile_retry_needed(true, false, TileRetryTrigger::Manual, 30*kSec, 10*kSec), "manual retry revives stale displayed frame without an SDK error");
    check(!tile_retry_needed(true, false, TileRetryTrigger::Manual, 15*kSec, 10*kSec), "fresh displayed frame is not needlessly retried");

    if (g_failures > 0) {
        std::cerr << "tile-retry: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "tile-retry: all tests passed\n";
    return 0;
}
