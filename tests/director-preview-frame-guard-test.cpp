// tests/director-preview-frame-guard-test.cpp
// Which frames the hidden director-preview subscription is allowed to put on
// air.
//
// The incident this guards (2026-08-11, live show): the `CoreVideo Active
// Speaker` source keeps a second, hidden subscription that warms up the NEXT
// speaker's video so the cut lands on a real frame instead of a gap.
// ZoomSource::on_director_preview_frame() published every frame that slot
// delivered straight to the program output. Two kinds of frame routinely arrive
// that do not belong to the participant we are waiting on: frames still in
// flight for the previous target after maybe_update_director_subscription()
// re-points the slot, and frames the engine had already queued when the cut
// unsubscribed it. On air that read as a participant flashing up with no
// speaker change behind them — and when such a late frame carried a different
// id than the one just cut to, the commit logic cut straight back to it,
// producing sub-second A->B->A cuts.
//
// The guard lives in ZoomSource, which needs libobs to instantiate, so the
// decision is factored out here as a pure rule and tested on its own — the same
// treatment speaker-settings-merge.h gets, and for the same reason: the only
// symptom of a regression is the wrong face on a live broadcast.

#include "director-preview-frame-guard.h"

#include <iostream>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

int main()
{
    // --- The frame we are waiting on is the cut, and must publish ---
    check(should_publish_director_preview_frame(16790528, 16790528),
          "the awaited participant's frame was dropped — the cut would land on "
          "a gap instead of a real frame");

    // --- A frame for anyone else is a stale in-flight frame from the slot's
    // previous target: publishing it puts the wrong face on air ---
    check(!should_publish_director_preview_frame(16790528, 16778240),
          "a frame for a participant other than the awaited one was published — "
          "this is the wrong-face flash, and the id mismatch is what drove the "
          "A->B->A cuts");

    // --- Nothing awaited (the cut already unsubscribed the slot, which stores
    // 0): every frame still queued in the engine must be dropped ---
    check(!should_publish_director_preview_frame(0, 16790528),
          "a frame arriving after the cut unsubscribed the preview slot was "
          "published");

    // --- A frame with no participant id is not a match for an idle slot: two
    // zeroes must not read as 'the awaited participant arrived' ---
    check(!should_publish_director_preview_frame(0, 0),
          "an unidentified frame was treated as matching an idle preview slot");

    // --- ...nor may an unidentified frame ride out on a live slot ---
    check(!should_publish_director_preview_frame(16790528, 0),
          "an unidentified frame was published while a real participant was "
          "awaited");

    if (failures == 0)
        std::cout << "director-preview-frame-guard tests passed\n";
    return failures == 0 ? 0 : 1;
}
