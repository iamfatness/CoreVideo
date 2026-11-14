// tests/share-output-policy-test.cpp
// How a screen-share output differs from a camera output: when it is stale,
// when it is recovered, and when it stops showing a share that has ended.
// See src/share-output-policy.h for the defects behind each rule.
#include "share-output-policy.h"
#include "share-renderer-policy.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

static constexpr uint64_t kMs = 1'000'000ULL;
static constexpr uint64_t kSec = 1'000'000'000ULL;
static constexpr uint64_t kCameraStale = 3 * kSec; // zoom-source.cpp's kStaleVideoNs

int main()
{
    // --- Staleness ---------------------------------------------------------
    // A camera is stale after 3 s. A share is content-adaptive (1-5 fps on a
    // static slide was measured live), so 3 s sits on the edge of a healthy
    // static share and graded it "Video stale" and resubscribed it.
    check(video_stale_threshold_ns(false, kCameraStale) == kCameraStale,
          "camera outputs must keep their own stale threshold");
    check(video_stale_threshold_ns(true, kCameraStale) == kShareStaleVideoNs,
          "share outputs must use the share stale threshold");
    check(kShareStaleVideoNs >= 3 * kCameraStale,
          "the share threshold must leave headroom over a ~1 fps static share");

    // --- Recovery ----------------------------------------------------------
    // Nobody sharing is a normal state, not a fault: there is no feed to
    // recover, and "recovering" it retried forever on a backoff.
    check(!share_wants_stale_recovery(false),
          "no recovery while the roster shows nobody sharing");
    check(share_wants_stale_recovery(true),
          "a share the roster says is live must be recoverable");

    // A share is the sharer's screen size; no resubscribe can raise it.
    check(!quality_upgrade_applies(true),
          "share outputs must never enter the quality-upgrade retry loop");
    check(quality_upgrade_applies(false),
          "camera outputs keep the quality-upgrade retry loop");

    // --- Clearing an ended share ------------------------------------------
    const uint64_t now = 1000 * kSec;

    // The defect: nothing ever cleared a share output, so when the presenter
    // stopped sharing the last frame stayed frozen on program indefinitely.
    check(share_should_clear(false, now - 60 * kSec, true, now - 5 * kSec, now),
          "an ended share whose frames stopped must be cleared");

    // Not while the roster says someone is sharing, however old the frame:
    // that is a static slide, and blanking it would be the opposite defect.
    check(!share_should_clear(true, 0, true, now - 60 * kSec, now),
          "a live (static) share must never be cleared");

    // Nothing on screen, nothing to clear -- no redundant output every tick.
    check(!share_should_clear(false, now - 60 * kSec, false, 0, now),
          "an output showing nothing must not be cleared again");

    // Grace: frames still arriving mean the share is live even if the
    // roster has not caught up (roster and frames ride different dispatch
    // lanes, and a sharer missing from the participant list would otherwise
    // flicker on every roster update).
    check(!share_should_clear(false, now - 60 * kSec, true, now - 200 * kMs, now),
          "frames still arriving must hold off the clear");
    check(share_should_clear(false, now - 60 * kSec, true, now - kShareEndGraceNs, now),
          "the clear must fire once the grace has elapsed");
    check(!share_should_clear(false, now - 60 * kSec, true, now - kShareEndGraceNs + 1, now),
          "the clear must not fire before the grace has elapsed");

    // A frame timestamp from the future (clock skew between threads) must
    // read as "just arrived", not wrap to a huge age.
    check(!share_should_clear(false, now - 60 * kSec, true, now + 5 * kMs, now),
          "a frame stamped after now must not underflow into an old frame");


    // Debounce on the roster itself (review finding, 2026-11-14): a static
    // slide can go far longer than kShareEndGraceNs without a frame, so the
    // frame gap alone cannot tell "ended" from "static". A momentary roster
    // push with no sharer (an empty viewable list, an unmatched sharer) must
    // not blank a live share; the "nobody sharing" state has to persist.
    check(!share_should_clear(false, now - 500 * kMs, true, now - 60 * kSec, now),
          "a roster that only just stopped showing a sharer must not clear");
    check(!share_should_clear(false, now - kShareRosterDebounceNs + 1, true,
                              now - 60 * kSec, now),
          "the clear must wait out the roster debounce");
    check(share_should_clear(false, now - kShareRosterDebounceNs, true,
                             now - 60 * kSec, now),
          "the clear must fire once the roster debounce has elapsed");
    check(!share_should_clear(false, 0, true, now - 60 * kSec, now),
          "an unknown roster-false time must not clear");

    // Showing something but no frame ever stamped (placeholder) -> clear.
    check(share_should_clear(false, now - 60 * kSec, true, 0, now),
          "a share output showing a placeholder with no share must clear");


    // --- Engine: rebuilding an idle share renderer -------------------------
    // Before 2026-11-14 a resubscribe on a share that already had a renderer
    // was a no-op in BOTH engines, so the plugin's stale recovery could never
    // restart a share Zoom had stopped delivering. The engine now rebuilds a
    // renderer that has been idle past kShareRendererIdleRebuildNs.
    const uint64_t t = 500 * kSec;
    check(!share_renderer_needs_rebuild(false, t - 60 * kSec, 0, t),
          "no renderer, nothing to rebuild");
    check(!share_renderer_needs_rebuild(true, t - 60 * kSec, t - 100 * kMs, t),
          "a renderer delivering frames must never be rebuilt");
    check(share_renderer_needs_rebuild(true, t - 60 * kSec,
                                       t - kShareRendererIdleRebuildNs, t),
          "a renderer idle past the threshold must be rebuilt");
    check(!share_renderer_needs_rebuild(true, t - 60 * kSec,
                                        t - kShareRendererIdleRebuildNs + 1, t),
          "a renderer must not be rebuilt before the threshold");
    // Never delivered a frame: idle is measured from its creation, so a
    // renderer created a moment ago is left alone to deliver its first frame.
    check(!share_renderer_needs_rebuild(true, t - 1 * kSec, 0, t),
          "a just-created renderer must get the chance to deliver");
    check(share_renderer_needs_rebuild(true, t - 30 * kSec, 0, t),
          "a renderer that never delivered must be rebuilt once idle");
    // Clock skew between the frame thread and the caller reads as fresh.
    check(!share_renderer_needs_rebuild(true, t - 60 * kSec, t + 5 * kMs, t),
          "a frame stamped after now must not underflow into idleness");
    // The plugin's recovery fires at kShareStaleVideoNs; the engine must
    // treat a renderer that idle as rebuildable or the recovery is a no-op.
    check(kShareRendererIdleRebuildNs < kShareStaleVideoNs,
          "the engine idle threshold must be below the plugin stale threshold");

    if (failures == 0) {
        std::cout << "share-output-policy-test: all checks passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << "share-output-policy-test: " << failures << " failure(s)\n";
    return EXIT_FAILURE;
}
