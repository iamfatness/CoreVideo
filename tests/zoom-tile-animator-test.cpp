#include "zoom-tile-animator.h"

#include <cmath>
#include <iostream>

static int failures = 0;
static void check(bool ok, const char *message)
{
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; ++failures; }
}

static TileRect rect(double x, double y, double w, double h)
{
    TileRect r; r.x = x; r.y = y; r.width = w; r.height = h; return r;
}

static const AnimatedTile *find(const std::vector<AnimatedTile> &tiles, uint32_t id)
{
    for (const auto &t : tiles)
        if (t.participant_id == id) return &t;
    return nullptr;
}

static constexpr uint64_t kMs = 1000000ULL;

int main()
{
    AnimationSettings on;  on.enabled = true;  on.duration_seconds = 0.35;
    AnimationSettings off; off.enabled = false;

    // Disabled: emits the desired rects verbatim, at rest, always.
    {
        TileAnimator a;
        const std::vector<DesiredTile> desired{{1, rect(0, 0, 100, 100)},
                                               {2, rect(100, 0, 100, 100)}};
        const auto out = a.advance(0, desired, off, {});
        check(out.size() == 2, "disabled animator dropped tiles");
        check(find(out, 1) && find(out, 1)->rect.x == 0.0 &&
              find(out, 2) && find(out, 2)->rect.x == 100.0,
              "disabled animator did not emit the desired rects verbatim");
        check(find(out, 1)->at_rest && find(out, 1)->alpha == 1.0,
              "disabled animator reported motion");
    }

    // First frame with animation on: tiles appear at their target, not from 0,0.
    {
        TileAnimator a;
        const std::vector<DesiredTile> desired{{1, rect(50, 60, 100, 100)}};
        const auto out = a.advance(0, desired, on, {});
        check(out.size() == 1 && find(out, 1)->rect.x == 50.0,
              "a newly seen tile did not start at its target position");
    }

    // A tile that moves is reported not-at-rest and travels toward the target.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}}, on, {});
        const auto out = a.advance(16 * kMs, {{1, rect(400, 0, 100, 100)}}, on, {});
        const AnimatedTile *t = find(out, 1);
        check(t != nullptr, "tile disappeared when retargeted");
        check(!t->at_rest, "a moving tile reported itself at rest");
        check(t->rect.x > 0.0 && t->rect.x < 400.0,
              "tile did not travel toward its new target");
    }

    // Identity, not index: the surviving tile keeps its own position when the
    // tile before it disappears from the desired layout. Participant 1
    // leaving is itself a set change, so — as of the settle window — it is
    // gated too: at 16ms nothing has moved yet for participant 7 either
    // (the whole layout might revert), but once the departure has genuinely
    // settled, participant 7 reflows from its own prior position, not from
    // participant 1's slot.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {7, rect(100, 0, 100, 100)}}, on, {});
        const auto out = a.advance(16 * kMs, {{7, rect(0, 0, 200, 200)}}, on, {});
        const AnimatedTile *t = find(out, 7);
        check(t != nullptr, "participant 7 was lost when participant 1 left");
        check(std::fabs(t->rect.x - 100.0) < 0.001,
              "participant 7 moved before participant 1's departure had settled");
        check(t->at_rest,
              "participant 7 reported motion before the departure had settled");

        const auto settled = a.advance(300 * kMs, {{7, rect(0, 0, 200, 200)}}, on, {});
        const AnimatedTile *t2 = find(settled, 7);
        check(t2 != nullptr, "participant 7 was lost once the departure settled");
        check(t2->rect.x > 0.0 && t2->rect.x < 100.0,
              "participant 7 did not continue animating from its own prior position toward its new target");
        check(!t2->at_rest, "participant 7 reported at rest despite being retargeted");
    }

    // A blip never moves the wall: a participant who vanishes and returns
    // inside the settle window produces no motion at all. Sampled mid-window
    // as well as at the endpoints, so "zero motion" is observed directly
    // rather than inferred from the before/after rects merely matching.
    {
        TileAnimator a;
        const std::vector<DesiredTile> two{{1, rect(0, 0, 100, 100)},
                                           {2, rect(100, 0, 100, 100)}};
        const std::vector<DesiredTile> one{{1, rect(0, 0, 200, 200)}};
        a.advance(0, two, on, {});
        a.advance(100 * kMs, one, on, {});          // 2 disappears
        const auto mid = a.advance(125 * kMs, one, on, {});    // still gone, mid-window
        const AnimatedTile *tm = find(mid, 1);
        check(tm != nullptr, "participant 1 lost mid-window during a blip");
        check(std::fabs(tm->rect.width - 100.0) < 0.001,
              "participant 1 drifted mid-window while the blip was still pending");
        const auto back = a.advance(150 * kMs, two, on, {});  // and returns
        const AnimatedTile *t = find(back, 1);
        check(t != nullptr, "participant 1 lost during a blip");
        check(std::fabs(t->rect.width - 100.0) < 0.001,
              "the wall reflowed for a blip that never settled");
        check(find(back, 2) != nullptr, "the returning participant was dropped");
    }

    // A change that holds past the settle window is acted on.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        a.advance(100 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {});
        const auto out = a.advance(400 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {});
        const AnimatedTile *t = find(out, 1);
        check(t != nullptr && t->rect.width > 100.0,
              "a settled departure did not start the reflow");
    }

    // A wall that vacates entirely and returns inside the settle window must
    // not have moved either: the whole roster leaving at once is a blip like
    // any other, and motion state for every participant survives it exactly
    // as a single participant's does. Sampled mid-window too (querying the
    // full roster again before the "official" return) so zero motion is
    // observed directly at an interior point of the window, not just at the
    // one endpoint chosen for the final check.
    {
        TileAnimator a;
        const std::vector<DesiredTile> two{{1, rect(0, 0, 100, 100)},
                                           {2, rect(100, 0, 100, 100)}};
        a.advance(0, two, on, {});
        a.advance(100 * kMs, {}, on, {});              // everyone leaves
        const auto mid = a.advance(125 * kMs, two, on, {});    // queried mid-window
        const AnimatedTile *m1 = find(mid, 1);
        const AnimatedTile *m2 = find(mid, 2);
        check(m1 != nullptr && m2 != nullptr,
              "a participant was lost mid-window during a full-vacate blip");
        check(std::fabs(m1->rect.width - 100.0) < 0.001 &&
              std::fabs(m2->rect.width - 100.0) < 0.001,
              "the wall had already reflowed mid-window for a full-vacate blip");
        const auto back = a.advance(150 * kMs, two, on, {});  // and returns
        const AnimatedTile *t1 = find(back, 1);
        const AnimatedTile *t2 = find(back, 2);
        check(t1 != nullptr && t2 != nullptr,
              "a participant was lost during a full-vacate blip");
        check(std::fabs(t1->rect.width - 100.0) < 0.001 &&
              std::fabs(t2->rect.width - 100.0) < 0.001,
              "the wall reflowed for a full-vacate blip that never settled");
    }

    // A legitimately empty wall is not a first frame: once the roster has
    // genuinely settled on zero participants — proposed at 300ms, then held
    // and reconfirmed at 560ms, 260ms later, past the 250ms hold — a return
    // must hold for its own 250ms before the wall commits to it, the same as
    // any other set change would. (A single call proposing empty is not
    // enough to "genuinely settle" on it; that would just be the proposal,
    // still pending until it is held and reconfirmed.) Proven by proposing a
    // layout change for the returning participant only 10ms after it
    // reappears: while that return is still pending, the change must sit
    // exactly at the proposed target, untouched (at_rest) — not partially
    // interpolated the way it would be if the wall had mistaken the return
    // for a fresh start and already committed to it.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}}, on, {});
        a.advance(300 * kMs, {}, on, {});                          // empty proposed
        a.advance(560 * kMs, {}, on, {});                          // held 260ms: now genuinely settled on zero
        a.advance(570 * kMs, {{1, rect(0, 0, 100, 100)}}, on, {});  // returns, 10ms later
        const auto out = a.advance(580 * kMs, {{1, rect(0, 0, 300, 300)}}, on, {});
        const AnimatedTile *t = find(out, 1);
        check(t != nullptr, "the returning participant was dropped");
        check(t->at_rest,
              "a still-pending return was already animating a layout change");
        check(std::fabs(t->rect.width - 300.0) < 0.001,
              "a still-pending return partially reflowed instead of holding at the proposed target");
    }

    // The "sharp" case found in review: an empty roster proposed after the
    // animator has already been running past the settle window must still be
    // held for its own 250ms, not adopted the instant it is proposed just
    // because elapsed real time already exceeds kSettleNs. m_pending_ids
    // defaults to (and is reset back to) an empty vector, so without
    // m_has_pending an empty proposal is indistinguishable from "nothing is
    // pending" — it falls through to the elapsed-time branch and is measured
    // against a stale m_pending_since_ns, adopting instantly any time the
    // animator has already been running for 250ms, which in a real session
    // is virtually always.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}}, on, {});
        a.advance(260 * kMs, {}, on, {});                           // empty proposed well past 250ms of uptime
        a.advance(270 * kMs, {{1, rect(0, 0, 100, 100)}}, on, {});  // returns, 10ms later
        const auto out = a.advance(280 * kMs, {{1, rect(0, 0, 300, 300)}}, on, {}); // rect changes, 10ms later
        const AnimatedTile *t = find(out, 1);
        check(t != nullptr, "the returning participant was dropped");
        check(!t->at_rest,
              "the sharp empty-roster proposal was adopted instantly, teleporting the returning tile");
        check(t->rect.width > 100.0 && t->rect.width < 300.0,
              "the sharp empty-roster proposal skipped the settle hold instead of staying in flight");
    }

    // Invariant 1 + 4: a genuine departure holds its last frame — unfaded,
    // never absent — for the whole 250ms settle window, begins fading only
    // once that window commits the smaller roster (not the instant the
    // departure is first observed), and is fully gone once its own duration
    // has elapsed. `departed` is a state, not a one-shot event: a real caller
    // recomputes it every frame and keeps passing it for as long as the
    // participant remains absent, so the test does the same rather than
    // passing it once and letting it lapse.
    //
    // The 300/549ms checks guard against the "hold has a hole" defect found
    // in review: `desired` drops the tile immediately, but `wanted` (still
    // keyed off the pre-commit `m_committed_ids`) does not go false until
    // commit, so without an explicit held-emission for this window the tile
    // is in neither emission set — cut, then popped back at 100% opacity
    // when the fade starts, which is worse than the single-frame cut this
    // whole feature exists to avoid.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});

        const auto proposed = a.advance(300 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});
        const AnimatedTile *held = find(proposed, 2);
        const bool held_at_full_alpha = held != nullptr && held->alpha == 1.0;
        check(held_at_full_alpha,
              "invariant 1: a departing tile vanished during the settle window instead of holding its last frame");

        const auto still_pending = a.advance(549 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});
        const AnimatedTile *still_held = find(still_pending, 2);
        const bool still_held_at_full_alpha = still_held != nullptr && still_held->alpha == 1.0;
        check(still_held_at_full_alpha,
              "invariant 1: a departing tile vanished 249ms into the settle window, one ms short of commit");

        const auto committed =
            a.advance(560 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});  // held 260ms >= 250ms: commits here
        const AnimatedTile *just_started = find(committed, 2);
        check(just_started != nullptr,
              "invariant 1: a departure did not begin exiting the instant its roster change committed");
        const bool started_at_full_alpha = just_started != nullptr && just_started->alpha == 1.0;
        check(started_at_full_alpha,
              "invariant 1: an exit did not start at full alpha at the moment it began "
              "(it must begin at commit, not at the earlier moment the departure was first observed)");

        const auto mid =
            a.advance(660 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});  // 100ms into a 350ms fade
        const AnimatedTile *leaving = find(mid, 2);
        check(leaving != nullptr, "invariant 1: a departing tile vanished instead of fading");
        const bool mid_fade = leaving != nullptr && leaving->alpha < 1.0 && leaving->alpha > 0.0;
        check(mid_fade, "invariant 1: a departing tile was not mid-fade");

        const auto after =
            a.advance(960 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});  // 400ms > 350ms duration
        check(find(after, 2) == nullptr,
              "invariant 4: an exit outlived its configured duration");
    }

    // Invariant 2: a reassignment cuts instantly — no fade, and no hold
    // either, at any of the timestamps at which invariant 1's test shows a
    // genuine departure present (mid-settle-hold, at commit, mid-fade, or
    // after). `departed` stays empty throughout: participant 2 disappeared
    // from the layout but never left the roster. The 300ms check is inside
    // the settle window — exactly where a careless "hold the departing tile"
    // patch (the Invariant-1 fix above) would start emitting a reassigned
    // participant too, if it held on committed-set membership instead of
    // `departed`.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        const auto pending = a.advance(300 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {});
        check(find(pending, 2) == nullptr,
              "invariant 2: a reassigned slot was held during the settle window");
        const auto committed = a.advance(560 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {});
        check(find(committed, 2) == nullptr,
              "invariant 2: a reassigned slot was still present once its set change committed");
        const auto mid = a.advance(660 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {});
        check(find(mid, 2) == nullptr,
              "invariant 2: a reassigned slot faded instead of cutting");
        const auto after = a.advance(960 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {});
        check(find(after, 2) == nullptr,
              "invariant 2: a reassigned slot lingered past a duration it should never have had");
    }

    // Invariant 3: a repoint cancels a running exit immediately — even when
    // the participant is repointed back into the *layout* while still
    // absent from the meeting *roster*. `departed` is roster-absence state
    // (see the header comment on `wanted`), so a correct caller keeps
    // reporting participant 2 as departed even after a slot shows them
    // again; this test does too, rather than testing only the easier case
    // where the caller conveniently clears `departed` on repoint.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        a.advance(300 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});
        a.advance(560 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});  // commits, exit begins
        const auto mid = a.advance(660 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});  // mid-fade
        const AnimatedTile *fading = find(mid, 2);
        const bool setup_ok = fading != nullptr && fading->alpha < 1.0 && fading->alpha > 0.0;
        check(setup_ok, "test setup: participant 2 should be mid-exit before the repoint");

        auto count_id2 = [](const std::vector<AnimatedTile> &tiles) {
            int n = 0;
            for (const auto &t : tiles)
                if (t.participant_id == 2) ++n;
            return n;
        };

        // Repoint: 2 is back in `desired`. `departed` still lists it — a
        // slot showing it again does not mean it rejoined the meeting.
        const auto out = a.advance(
            670 * kMs, {{1, rect(0, 0, 200, 200)}, {2, rect(100, 0, 150, 150)}}, on, {2});
        check(count_id2(out) == 1,
              "invariant 3: a repointed participant was emitted more than once at the repoint frame");
        const AnimatedTile *repointed = find(out, 2);
        const bool cancelled_at_repoint = repointed != nullptr && repointed->alpha == 1.0;
        check(cancelled_at_repoint,
              "invariant 3: a running exit survived the slot being repointed");

        // Advance past the repointed set's own settle commit (250ms after
        // 670ms). This is where a stale `exiting` flag — left true because
        // the `wanted` branch never reset it — would surface as a permanent
        // duplicate: itself a standing invariant-4 violation, an exit that
        // never ends.
        const auto after_commit = a.advance(
            920 * kMs, {{1, rect(0, 0, 200, 200)}, {2, rect(100, 0, 150, 150)}}, on, {2});
        check(count_id2(after_commit) == 1,
              "invariant 3: a stale exit produced a permanent duplicate once the repoint's own settle window committed");
        const AnimatedTile *settled = find(after_commit, 2);
        const bool no_lingering_fade = settled != nullptr && settled->alpha == 1.0;
        check(no_lingering_fade,
              "invariant 3: the repointed participant was still fading after its own settle window committed");
    }

    if (failures == 0) std::cout << "zoom-tile-animator tests passed\n";
    return failures == 0 ? 0 : 1;
}
