#include "zoom-tile-animator.h"

#include <algorithm>
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

// Shared area of two rects, 0 when they do not overlap. Used to assert that
// no two tiles the animator emits as live are ever stacked.
static double overlap_area(const TileRect &a, const TileRect &b)
{
    const double left   = std::fmax(a.x, b.x);
    const double top    = std::fmax(a.y, b.y);
    const double right  = std::fmin(a.x + a.width,  b.x + b.width);
    const double bottom = std::fmin(a.y + a.height, b.y + b.height);
    if (right <= left || bottom <= top) return 0.0;
    return (right - left) * (bottom - top);
}

// -1 if absent. Used to check relative draw order, not just presence.
static int index_of(const std::vector<AnimatedTile> &tiles, uint32_t id)
{
    for (size_t i = 0; i < tiles.size(); ++i)
        if (tiles[i].participant_id == id) return static_cast<int>(i);
    return -1;
}

static constexpr uint64_t kMs = 1000000ULL;

// Worst overlap between any two emitted tiles this frame.
static double worst_overlap(const std::vector<AnimatedTile> &out)
{
    double w = 0.0;
    for (size_t i = 0; i < out.size(); ++i)
        for (size_t j = i + 1; j < out.size(); ++j)
            w = std::fmax(w, overlap_area(out[i].rect, out[j].rect));
    return w;
}

// Worst overlap between two tiles that are BOTH at rest. This is the invariant
// the design can actually hold: an immediate join necessarily draws the
// newcomer at its slot in the new grid while the incumbents are still at their
// old ones and only starting to move, so tiles DO overlap transiently during a
// reflow — that is what the entry fade exists to make invisible. What must
// never happen is two STATIONARY tiles stacked, which is what every defect in
// this file's history actually was.
static double worst_at_rest_overlap(const std::vector<AnimatedTile> &out)
{
    double w = 0.0;
    for (size_t i = 0; i < out.size(); ++i)
        for (size_t j = i + 1; j < out.size(); ++j)
            if (out[i].at_rest && out[j].at_rest)
                w = std::fmax(w, overlap_area(out[i].rect, out[j].rect));
    return w;
}

// Advance until the wall is fully settled — every spring arrived and every
// entry fade finished — so a test about a later transition does not start
// mid-fade. Returns the timestamp reached, in ms.
static uint64_t settle_in(TileAnimator &a, const std::vector<DesiredTile> &d,
                          const AnimationSettings &on, uint64_t from_ms)
{
    uint64_t ms = from_ms;
    for (int i = 0; i < 60 && !a.settled(); ++i) {
        ms += 16;
        a.advance(ms * kMs, d, on, {});
    }
    return ms;
}

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

    // First frame with animation on: tiles appear at their target, not from
    // 0,0 — and they ENTER, fading from transparent to opaque in place rather
    // than popping on at full brightness. The fade is what makes an immediate
    // join safe: the newcomer is at its slot in the new grid while every other
    // tile is still at its old one, so it is drawn over them on that first
    // frame, and alpha ~0 is what makes that invisible.
    {
        TileAnimator a;
        const std::vector<DesiredTile> desired{{1, rect(50, 60, 100, 100)}};
        const auto out = a.advance(0, desired, on, {});
        check(out.size() == 1 && find(out, 1)->rect.x == 50.0,
              "a newly seen tile did not start at its target position");
        check(find(out, 1)->alpha < 0.001,
              "a newly seen tile appeared at full opacity instead of fading in");

        // Reaches full opacity by duration_seconds, and stays there.
        const auto mid = a.advance(175 * kMs, desired, on, {});
        check(find(mid, 1)->alpha > 0.4 && find(mid, 1)->alpha < 0.6,
              "the entry fade is not linear in time across its duration");
        const auto done = a.advance(350 * kMs, desired, on, {});
        check(find(done, 1)->alpha == 1.0,
              "the entry fade did not reach full opacity by duration_seconds");
        const auto after = a.advance(400 * kMs, desired, on, {});
        check(find(after, 1)->alpha == 1.0,
              "a tile that finished entering did not stay opaque");
        check(a.settled(),
              "the wall was not settled once every entry fade had finished");
    }

    // A zero duration has no fade to run, exactly as it has no travel to do:
    // the tile arrives opaque on its first frame rather than dividing by zero.
    {
        TileAnimator a;
        const AnimationSettings instant{true, 0.0};
        const auto out = a.advance(0, {{1, rect(50, 60, 100, 100)}}, instant, {});
        check(find(out, 1) != nullptr && find(out, 1)->alpha == 1.0,
              "a zero duration still produced a fade");
        check(a.settled(), "a zero-duration wall was not immediately settled");
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

    // Identity, not index: the surviving tile reflows from ITS OWN prior
    // position when the tile before it disappears, not from the departing
    // tile's slot. Participant 1 here is REASSIGNED, not departed — absent
    // from the layout but still in the meeting — so the wall commits at once
    // and participant 7 starts moving on the same frame.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {7, rect(100, 0, 100, 100)}}, on, {});
        const uint64_t t0 = settle_in(
            a, {{1, rect(0, 0, 100, 100)}, {7, rect(100, 0, 100, 100)}}, on, 0);

        const auto out =
            a.advance((t0 + 16) * kMs, {{7, rect(0, 0, 200, 200)}}, on, {});
        const AnimatedTile *t = find(out, 7);
        check(t != nullptr, "participant 7 was lost when participant 1 was reassigned away");
        check(!t->at_rest,
              "participant 7 did not start moving on the frame a reassignment "
              "freed the slot — a reassignment is not a departure and must not hold");
        check(t->rect.x < 100.0 && t->rect.x > 0.0,
              "participant 7 did not travel from its own prior position toward "
              "its new target");
    }

    // A roster blip never moves the wall: a participant who vanishes from the
    // ROSTER and returns inside the settle window produces no motion at all.
    // `departed` names them while they are gone, which is what makes this a
    // departure rather than a reassignment. Sampled mid-window as well as at
    // the endpoints, so "zero motion" is observed directly rather than
    // inferred from the before/after rects merely matching.
    {
        TileAnimator a;
        const std::vector<DesiredTile> two{{1, rect(0, 0, 100, 100)},
                                           {2, rect(100, 0, 100, 100)}};
        const std::vector<DesiredTile> one{{1, rect(0, 0, 200, 200)}};
        a.advance(0, two, on, {});
        settle_in(a, two, on, 0);

        a.advance(600 * kMs, one, on, {2});          // 2 drops off the roster
        const auto mid = a.advance(625 * kMs, one, on, {2});   // still gone, mid-window
        const AnimatedTile *tm = find(mid, 1);
        check(tm != nullptr, "participant 1 lost mid-window during a blip");
        check(std::fabs(tm->rect.width - 100.0) < 0.001,
              "participant 1 drifted mid-window while the departure was still pending");
        const auto back = a.advance(650 * kMs, two, on, {});   // and returns
        const AnimatedTile *t = find(back, 1);
        check(t != nullptr, "participant 1 lost during a blip");
        check(std::fabs(t->rect.width - 100.0) < 0.001,
              "the wall reflowed for a roster blip that never settled");
        check(find(back, 2) != nullptr, "the returning participant was dropped");
        check(worst_overlap(back) == 0.0,
              "tiles overlapped during a departure hold, where nothing is on "
              "the new grid at all");
    }

    // A departure that holds past the settle window is acted on.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        settle_in(a, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, 0);
        a.advance(600 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});
        const auto out = a.advance(900 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});
        const AnimatedTile *t = find(out, 1);
        check(t != nullptr && t->rect.width > 100.0,
              "a settled departure did not start the reflow");
    }

    // A wall that vacates entirely and returns inside the settle window must
    // not have moved either: the whole roster leaving at once is a departure
    // blip like any other, and motion state for every participant survives it
    // exactly as a single participant's does. Sampled mid-window too (querying
    // the full roster again before the "official" return) so zero motion is
    // observed directly at an interior point of the window.
    {
        TileAnimator a;
        const std::vector<DesiredTile> two{{1, rect(0, 0, 100, 100)},
                                           {2, rect(100, 0, 100, 100)}};
        a.advance(0, two, on, {});
        settle_in(a, two, on, 0);

        a.advance(600 * kMs, {}, on, {1, 2});                   // everyone drops off the roster
        const auto mid = a.advance(625 * kMs, two, on, {});     // queried mid-window
        const AnimatedTile *m1 = find(mid, 1);
        const AnimatedTile *m2 = find(mid, 2);
        check(m1 != nullptr && m2 != nullptr,
              "a participant was lost mid-window during a full-vacate blip");
        check(std::fabs(m1->rect.width - 100.0) < 0.001 &&
              std::fabs(m2->rect.width - 100.0) < 0.001,
              "the wall had already reflowed mid-window for a full-vacate blip");
        const auto back = a.advance(650 * kMs, two, on, {});    // and returns
        const AnimatedTile *t1 = find(back, 1);
        const AnimatedTile *t2 = find(back, 2);
        check(t1 != nullptr && t2 != nullptr,
              "a participant was lost during a full-vacate blip");
        check(std::fabs(t1->rect.width - 100.0) < 0.001 &&
              std::fabs(t2->rect.width - 100.0) < 0.001,
              "the wall reflowed for a full-vacate blip that never settled");
    }

    // A wall that genuinely empties, then a participant returns. The vacate is
    // a departure, so it holds for kSettleNs and then resolves; the return is
    // a JOIN, so it commits at once and the participant is drawn on the frame
    // it reappears, entering from transparent. Nothing waits on anything.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}}, on, {});
        settle_in(a, {{1, rect(0, 0, 100, 100)}}, on, 0);

        a.advance(600 * kMs, {}, on, {1});      // 1 leaves the meeting
        a.advance(900 * kMs, {}, on, {1});      // hold expired at 850: the exit begins
        a.advance(1300 * kMs, {}, on, {1});     // past the 350ms fade: gone
        check(a.tracked_ids().empty(),
              "the vacated wall still carried motion state long after the exit "
              "should have finished");

        const auto returned = a.advance(1400 * kMs, {{1, rect(0, 0, 300, 300)}}, on, {});
        const AnimatedTile *back = find(returned, 1);
        check(back != nullptr,
              "a participant returning to an empty wall was not drawn on the "
              "frame it reappeared — a join has no departure to wait for");
        check(back != nullptr && std::fabs(back->rect.width - 300.0) < 0.001,
              "the returning participant did not appear at its own target");
        check(back != nullptr && back->alpha < 0.001,
              "the returning participant popped in at full opacity instead of "
              "fading in");
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
    // in review: `desired` drops the tile immediately, so without an explicit
    // held-emission for the window between that and the exit starting, the
    // tile is in neither emission set — cut, then popped back at 100% opacity
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

    // Fix round 2, NEW Important 1 (tightened in round 3, Minor 1): the
    // hold must have its own ceiling, independent of the whole-set settle
    // timer. The settle timer resets whenever the incoming set changes
    // from what was last proposed, so a single unrelated participant
    // flapping in and out of `desired` keeps resetting it forever — before
    // this fix, a genuinely departed participant held via that timer would
    // then never be released. Mirrors the review's own flapping probe:
    // participant 1 stays, participant 9 flaps in and out every 200ms,
    // participant 2 left the roster at t=300ms and must be gone well
    // before the run ends 60 seconds later.
    //
    // Round 2's version of this test only asserted "not indefinite" (a
    // 1500ms allowance against a 600ms continuous-time ceiling), which a
    // ceiling up to 4x too large — kSettleNs*4 instead of kSettleNs — still
    // passed: a departed participant held at full opacity for a full
    // second is a real regression, not noise. Tightened to two separate,
    // narrow bounds: the last frame seen at *full opacity* must be within
    // kSettleNs of departure (proving the hold itself is bounded, not just
    // the eventual disappearance), and the last frame seen *at all* must be
    // within kSettleNs + duration. Both add one grid interval (200ms, this
    // test's own sampling period) of slack for detection latency on a
    // discrete-call schedule, not to hide a looser ceiling.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});

        constexpr uint64_t kDepartedAtMs = 300;
        constexpr uint64_t kGridMs        = 200;  // this test's sampling interval
        constexpr uint64_t kSettleMs      = 250;  // kSettleNs, in ms
        constexpr uint64_t kDurationMs    = 350;  // on.duration_seconds, in ms
        uint64_t last_full_opacity_ms = 0;
        uint64_t last_seen_ms = 0;

        uint64_t t = kDepartedAtMs * kMs;
        for (int i = 0; i < 300; ++i) {  // 300 * 200ms = 60s of roster churn
            std::vector<DesiredTile> d{{1, rect(0, 0, 100, 100)}};
            if (i % 2) d.push_back({9, rect(100, 0, 100, 100)});  // 9 flaps in and out
            const auto out = a.advance(t, d, on, {2});
            const AnimatedTile *tile2 = find(out, 2);
            if (tile2 != nullptr) {
                last_seen_ms = t / kMs;
                if (tile2->alpha == 1.0) last_full_opacity_ms = t / kMs;
            }
            t += kGridMs * kMs;
        }

        // Both bounds below are on the LAST time participant 2 was seen, and
        // both counters start at 0 — so a build that never emitted the tile at
        // all would satisfy them vacuously, and this is the test that bounds a
        // departed participant's time on air. Pin that the tile was in fact
        // emitted first, so the two ceilings are measuring something.
        check(last_seen_ms > 0,
              "test setup: participant 2 was never emitted at all, so the hold "
              "ceilings below would pass vacuously");

        check(last_full_opacity_ms <= kDepartedAtMs + kSettleMs + kGridMs,
              "invariant 1: a departed tile was held at full opacity well past kSettleNs "
              "under unrelated roster churn — the hold ceiling is too loose");
        check(last_seen_ms <= kDepartedAtMs + kSettleMs + kDurationMs + kGridMs,
              "invariant 1/4: a departed tile was emitted well past kSettleNs + duration "
              "under unrelated roster churn — the hold-plus-fade ceiling is too loose");
    }

    // Final review, IMPORTANT 2: a reassignment whose hold clock expires must
    // be ERASED, not merely kept out of the output. The distinction is
    // invisible until the same participant later genuinely leaves the
    // meeting, and it is the difference between nothing happening and a
    // seconds-old frozen frame going on air at full opacity.
    //
    // The sequence is one an operator produces without trying: repoint a
    // slot away from participant 2 at t=100ms (a reassignment — 2 is still in
    // the meeting, so `departed` is empty), with unrelated roster churn so
    // the whole-set settle window never commits. 2's own hold clock expires
    // 250ms later and the tile is erased. Two seconds after that, 2 genuinely
    // leaves the meeting: a correct caller computes `departed` from
    // tracked_ids(), so a build that erased 2 never reports it and nothing
    // happens — which is the only correct outcome, because the frame that
    // tile is still holding is from before the repoint and shows the wrong
    // face by construction (the rule src/zoom-tile-slot.h exists to enforce).
    //
    // A build that instead retains the tile — `if (committed) { ++it;
    // continue; }` in place of the reassignment erase — passes every other
    // test in this file: the tile is retained but never emitted, so nothing
    // observes it, until `departed` finally names it and it begins a full
    // exit at alpha 1.0 from a rect two seconds stale. That mutant was
    // written and confirmed to survive the whole suite; this test is what
    // kills it.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});

        constexpr uint64_t kReassignedAtMs = 100;
        constexpr uint64_t kLeftMeetingAtMs = 2100;
        constexpr uint64_t kGridMs = 100;

        bool emitted_after_reassign = false;
        bool emitted_after_departure = false;
        bool wall_alive = true;

        for (uint64_t ms = kReassignedAtMs; ms <= 4000; ms += kGridMs) {
            // Participant 9 flaps in and out on every sample, so the proposed
            // set differs from the last one seen on every call and the
            // whole-set settle timer is re-stamped forever. Nothing ever
            // commits, which is the condition that makes the erase and the
            // retain indistinguishable to every other test.
            std::vector<DesiredTile> d{{1, rect(0, 0, 100, 100)}};
            if ((ms / kGridMs) % 2) d.push_back({9, rect(100, 0, 100, 100)});

            // Roster-absence state, recomputed every frame by a real caller:
            // empty while 2 is merely repointed away, naming 2 once 2 has
            // actually left the meeting.
            const std::vector<uint32_t> departed =
                ms >= kLeftMeetingAtMs ? std::vector<uint32_t>{2}
                                       : std::vector<uint32_t>{};

            const auto out = a.advance(ms * kMs, d, on, departed);
            if (find(out, 1) == nullptr) wall_alive = false;
            if (find(out, 2) != nullptr) {
                emitted_after_reassign = true;
                if (ms >= kLeftMeetingAtMs) emitted_after_departure = true;
            }
        }

        check(wall_alive,
              "test setup: participant 1 stopped being emitted, so the run below "
              "proves nothing about participant 2");
        check(!emitted_after_reassign,
              "invariant 2: a reassigned participant was put on screen at some "
              "point after the repoint");
        check(!emitted_after_departure,
              "a participant reassigned away 2 seconds earlier was resurrected "
              "when they later left the meeting — a stale frame from before the "
              "repoint went on air at full opacity and faded");
    }

    // Fix round 2, NEW Important 2: held and exiting tiles must be emitted
    // before live tiles, so a live tile painted after them draws on top
    // rather than being hidden under a departed participant's frozen or
    // fading frame. Reproduces the review's scenario: a departure and a
    // join in the same set change hand the joiner the departing
    // participant's old rect. Checked in both the held phase (before the
    // roster change commits) and the exiting phase (after it commits and
    // the fade begins) — the ordering contract must hold in both states.
    {
        TileAnimator a;
        const TileRect slotA = rect(0, 0, 100, 100);
        const TileRect slotB = rect(100, 0, 100, 100);
        a.advance(0, {{1, slotA}, {2, slotB}}, on, {});

        // 2 departs, 3 joins into slotB — 2's old rect. While the departure
        // holds, 3 is not drawn at all: a departure pending means the wall is
        // still showing the layout 2 belongs to, and nothing from the new grid
        // is put into it. That is what makes a hold overlap-free by
        // construction rather than by a geometric test.
        const auto pending = a.advance(300 * kMs, {{1, slotA}, {3, slotB}}, on, {2});
        check(index_of(pending, 2) >= 0,
              "a departing tile was not held through its settle window");
        check(index_of(pending, 3) < 0,
              "a joiner was drawn into the departing tile's rect while the "
              "departure was still holding");
        check(worst_overlap(pending) == 0.0,
              "tiles overlapped during a departure hold");

        // The hold expires at 550ms: the exit begins and 3 joins. Now the two
        // can coincide, and the ordering contract is what resolves it.
        const auto exiting_phase = a.advance(560 * kMs, {{1, slotA}, {3, slotB}}, on, {2});
        const bool exiting_behind_live =
            index_of(exiting_phase, 2) >= 0 && index_of(exiting_phase, 3) >= 0 &&
            index_of(exiting_phase, 2) < index_of(exiting_phase, 3);
        check(exiting_behind_live,
              "ordering contract: an exiting tile was emitted after the live tile "
              "occupying its rect, instead of before it");
    }

    // Fix round 3, Important: a one-frame blip during an in-flight reflow
    // must not teleport the tile to its target. Round 2 narrowed the
    // lifecycle loop's retention test to `in_desired` alone, so a tile
    // merely absent from `desired` for a single frame — never in
    // `departed` — fell into the reassignment-erase branch and was
    // recreated from scratch (at rest, at the target) the moment it
    // returned: a visible pop in exactly the motion-continuity feature
    // this file exists to deliver, even though no wrong face was ever put
    // on air. Proven by an A/B: a control run that never blips, versus an
    // otherwise-identical run where the same tile is dropped from
    // `desired` for exactly one call.
    {
        TileAnimator control;
        control.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        control.advance(16 * kMs, {{1, rect(400, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        control.advance(32 * kMs, {{1, rect(400, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        const auto control_out =
            control.advance(48 * kMs, {{1, rect(400, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        const AnimatedTile *control_tile = find(control_out, 1);
        check(control_tile != nullptr && !control_tile->at_rest,
              "test setup: the control run should still be in flight at t=48ms");

        TileAnimator blipped;
        blipped.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        blipped.advance(16 * kMs, {{1, rect(400, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        // 1 drops out of `desired` for exactly one call. Never in
        // `departed` — this is not a departure, and it is not a
        // reassignment either: it is the single-frame flicker the settle
        // window exists to absorb.
        blipped.advance(32 * kMs, {{2, rect(100, 0, 100, 100)}}, on, {});
        const auto blipped_out =
            blipped.advance(48 * kMs, {{1, rect(400, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        const AnimatedTile *blipped_tile = find(blipped_out, 1);

        check(blipped_tile != nullptr, "a blipped tile was lost on return");
        const bool still_in_flight = blipped_tile != nullptr && !blipped_tile->at_rest;
        check(still_in_flight,
              "a one-frame blip teleported a mid-flight tile to its target (at_rest) "
              "instead of leaving it in flight");
        const bool did_not_snap_to_target =
            blipped_tile != nullptr && std::fabs(blipped_tile->rect.x - 400.0) > 1.0;
        check(did_not_snap_to_target,
              "a one-frame blip snapped a mid-flight tile to its exact target rect");
    }

    // Fix round 3, Minor 2: a second departure must start its own fresh
    // hold clock, not reuse a stale timestamp from an earlier
    // departure-then-return cycle (mutant H: dropping `held = false` from
    // the in-desired branch). First departure, then a repoint that brings
    // the participant back and lets the roster re-commit to including
    // them, then a second, later departure — checked shortly after it
    // begins, well inside a fresh 250ms hold window. A stale clock (still
    // ticking from the first departure) would already have expired by
    // then and be mid-fade instead of held.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        a.advance(300 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});   // departs
        a.advance(560 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});   // commits, exit begins
        a.advance(600 * kMs, {{1, rect(0, 0, 200, 200)}, {2, rect(100, 0, 100, 100)}}, on, {});  // repointed back
        a.advance(900 * kMs, {{1, rect(0, 0, 200, 200)}, {2, rect(100, 0, 100, 100)}}, on, {});  // held steady 300ms: roster re-commits to {1,2}
        a.advance(2000 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});  // departs again
        const auto shortly_after =
            a.advance(2100 * kMs, {{1, rect(0, 0, 200, 200)}}, on, {2});  // 100ms later: well inside a fresh hold window
        const AnimatedTile *t2 = find(shortly_after, 2);
        check(t2 != nullptr, "a second departure was not shown at all 100ms in");
        const bool fresh_clock = t2 != nullptr && t2->alpha == 1.0;
        check(fresh_clock,
              "invariant 1: a second departure reused a stale hold clock from an earlier "
              "departure-then-return cycle instead of starting its own");
    }

    // TileAnimator::settled(): false while a set change is pending or
    // anything is held, exiting, or still springing; true once everything
    // has committed and fully arrived. Exercises the exact bug settled()
    // was added to fix (see task-6-report.md): a render path that gated its
    // fast path on "every AnimatedTile this frame reports at_rest" instead
    // of settled() mis-sized every live tile during a departure's settle
    // hold, because the held tile and the blip-protected survivors are ALL
    // at_rest individually while the wall as a whole still disagrees with a
    // fresh solve. Stepped in small (20ms) increments throughout, the way a
    // real render loop calls advance() every frame — a single huge-dt call
    // spanning the whole hold would let the spring below nearly finish in
    // one step and defeat the "still not settled right after commit" check.
    {
        TileAnimator a;
        const std::vector<DesiredTile> start{{1, rect(0, 0, 100, 100)},
                                             {2, rect(100, 0, 100, 100)}};
        a.advance(0, start, on, {});
        check(!a.settled(),
              "a wall whose tiles are still fading in was reported settled — the "
              "whole-grid snap has no alpha, so that would drop every entry fade");
        uint64_t t = settle_in(a, start, on, 0) * kMs;
        check(a.settled(), "a joined, arrived, fully-faded-in wall was not reported settled");

        auto step = [&](const std::vector<DesiredTile> &d, const std::vector<uint32_t> &dep) {
            t += 20 * kMs;
            return a.advance(t, d, on, dep);
        };

        // Participant 2 departs; participant 1's target grows to fill the
        // freed space. Still pending for the first 250ms.
        bool saw_all_at_rest_during_hold = false;
        for (int i = 0; i < 12; ++i) {  // 12 * 20ms = 240ms: inside the 250ms hold
            const auto out = step({{1, rect(0, 0, 200, 200)}}, {2});
            if (std::all_of(out.begin(), out.end(),
                             [](const AnimatedTile &x) { return x.at_rest; }))
                saw_all_at_rest_during_hold = true;
            check(!a.settled(),
                  "settled() was true during a departure's settle hold, where a "
                  "fresh solve would already disagree with the wall's held layout");
        }
        check(saw_all_at_rest_during_hold,
              "test setup: every AnimatedTile should report at_rest at some point "
              "during the hold — exactly what settled() must NOT rely on");

        // Cross the 250ms hold: commits, participant 1 starts springing to
        // its new target, participant 2 starts exiting.
        for (int i = 0; i < 3; ++i) step({{1, rect(0, 0, 200, 200)}}, {2});
        check(!a.settled(),
              "settled() was true immediately after commit, with an exit in "
              "flight and a spring not yet at its target");

        // Run well past the 350ms fade duration and let the spring fully
        // arrive.
        for (int i = 0; i < 40; ++i) step({{1, rect(0, 0, 200, 200)}}, {2});
        check(a.settled(),
              "settled() was still false long after the exit finished and the "
              "spring arrived");
    }

    // Fix round 4, part 1: a PURE JOIN reflows the wall immediately. No
    // departure is pending, so nothing is held: the newcomer is drawn on the
    // frame it appears and every other tile starts moving on that same frame.
    //
    // The rects are the real 1920x1080 geometry — solve_tile_grid() at the
    // shipped defaults (16:9, gutter and margin = canvas_height/135 = 8),
    // snapped — for one tile and for two, so the overlap this measures is the
    // one a real wall produces.
    {
        const TileRect one_of_one   = rect(14.0, 8.0, 1890.0, 1064.0);
        const TileRect two_of_two_a = rect(8.0, 274.0, 948.0, 532.0);
        const TileRect two_of_two_b = rect(964.0, 274.0, 948.0, 532.0);

        TileAnimator a;
        a.advance(0, {{1, one_of_one}}, on, {});
        const uint64_t t0 = settle_in(a, {{1, one_of_one}}, on, 0);
        check(a.settled(), "a single arrived participant was not reported settled");

        const auto joined = a.advance((t0 + 16) * kMs,
                                      {{1, two_of_two_a}, {2, two_of_two_b}}, on, {});

        const AnimatedTile *newcomer = find(joined, 2);
        check(newcomer != nullptr,
              "the newcomer was not drawn on the frame it appeared — a join has "
              "no departure to wait for");
        check(newcomer != nullptr &&
                  std::fabs(newcomer->rect.x - two_of_two_b.x) < 0.001,
              "the newcomer did not appear at its own target");
        check(newcomer != nullptr && newcomer->alpha < 0.001,
              "the newcomer appeared at full opacity — the entry fade is what "
              "makes the reflow transient invisible");

        const AnimatedTile *incumbent = find(joined, 1);
        check(incumbent != nullptr && !incumbent->at_rest,
              "the incumbent did not start moving on the frame the newcomer "
              "appeared");
        check(incumbent != nullptr && incumbent->rect.width < one_of_one.width &&
                  incumbent->rect.width > two_of_two_a.width,
              "the incumbent did not begin shrinking toward the two-up grid");

        // This is the transient the entry fade exists for: the newcomer is at
        // its slot in the NEW grid while the incumbent is still very nearly
        // filling the canvas, so it is drawn entirely inside it. Both are
        // "live"; what makes it invisible is that the newcomer's alpha is ~0
        // exactly here. Pinned so a regression in the fade cannot pass unseen.
        const double peak = worst_overlap(joined);
        check(peak > 400000.0,
              "test setup: the join should produce a large first-frame overlap, "
              "otherwise this proves nothing about the fade");
        check(newcomer != nullptr && newcomer->alpha < 0.001,
              "the wall stacked two live tiles at a large overlap with the "
              "newcomer already visible");

        // It decays as the incumbent reflows, and the two never sit stacked
        // while both are at rest.
        double worst_at_rest = 0.0;
        uint64_t ms = t0 + 16;
        for (int i = 0; i < 60; ++i) {
            ms += 16;
            const auto out =
                a.advance(ms * kMs, {{1, two_of_two_a}, {2, two_of_two_b}}, on, {});
            worst_at_rest = std::fmax(worst_at_rest, worst_at_rest_overlap(out));
        }
        check(worst_at_rest == 0.0,
              "two tiles that were both at rest overlapped after a join");
        check(a.settled(),
              "the wall was not settled once the join had reflowed and faded in");
    }

    // The real 3-up and 4-up grids at 1920x1080, snapped. Used by the repro
    // tests below, so what they measure is what a real wall produces.
    const TileRect three_a = rect(18.0, 8.0, 938.0, 528.0);
    const TileRect three_b = rect(964.0, 8.0, 938.0, 528.0);
    const TileRect three_c = rect(490.0, 544.0, 938.0, 528.0);
    const TileRect four_a  = rect(18.0, 8.0, 938.0, 528.0);
    const TileRect four_b  = rect(964.0, 8.0, 938.0, 528.0);
    const TileRect four_c  = rect(18.0, 544.0, 938.0, 528.0);
    const TileRect four_d  = rect(964.0, 544.0, 938.0, 528.0);

    // Fix round 4, rule 3: a REPOINT is instant. The outgoing participant is
    // absent from the layout but still in the meeting, so it is not a
    // departure and holds nothing — the incoming participant is drawn on the
    // frame the operator repoints the slot, exactly as it was before this
    // feature existed.
    //
    // This is the amendment that matters most in practice. Settling every set
    // change, rather than only departures, blanked the repointed slot for
    // kSettleNs: the withheld feed got an empty SnappedTileRect and the
    // width < 2 guard skipped it in the glow loop as well as the tile loop, so
    // the slot lost its tile, its border AND its glow for a quarter second,
    // where before it drew a neutral placeholder immediately.
    {
        TileAnimator a;
        const std::vector<DesiredTile> before{{1, three_a}, {2, three_b}, {3, three_c}};
        a.advance(0, before, on, {});
        const uint64_t t0 = settle_in(a, before, on, 0);
        check(a.settled(), "test setup: the three-up wall should be settled");

        // Slot three is repointed from participant 3 to participant 4.
        // `departed` is empty — 3 has not left the meeting.
        const auto out = a.advance((t0 + 16) * kMs,
                                   {{1, three_a}, {2, three_b}, {4, three_c}}, on, {});

        const AnimatedTile *incoming = find(out, 4);
        check(incoming != nullptr,
              "a repoint left the slot empty: the incoming participant was held "
              "as though a reassignment were a departure");
        check(incoming != nullptr && std::fabs(incoming->rect.x - three_c.x) < 0.001 &&
                  std::fabs(incoming->rect.width - three_c.width) < 0.001,
              "the repointed-in participant did not appear at the slot's own rect");
        check(incoming != nullptr && incoming->at_rest,
              "the repointed-in participant was interpolating instead of cutting in");
        check(find(out, 3) == nullptr,
              "invariant 2: the repointed-away participant was still on screen");
        check(worst_overlap(out) == 0.0,
              "a repoint produced overlapping tiles, where the grid is identical "
              "on both sides of the change");

        // The neighbours do not move: same count, same grid.
        check(find(out, 1) != nullptr && find(out, 1)->at_rest &&
                  find(out, 2) != nullptr && find(out, 2)->at_rest,
              "a repoint moved the tiles that did not change");
    }

    // Fix round 4: a departure hold is BOUNDED under sustained unrelated
    // churn. This is the scenario that broke the first attempt at bounding —
    // a whole-set settle timer re-stamped on every set change, so one
    // participant flapping in and out stalled a genuine departure forever.
    // The timer is per tile and stamped once, at that tile's own
    // disappearance, so nothing else can extend it; and the hold condition is
    // evaluated from the tiles absent on THIS frame, so on every frame the
    // flapper is present it contributes nothing at all.
    {
        TileAnimator a;
        const std::vector<DesiredTile> start{{1, three_a}, {2, three_b}, {3, three_c}};
        a.advance(0, start, on, {});
        const uint64_t t0 = settle_in(a, start, on, 0);

        constexpr uint64_t kSettleMs = 250;
        constexpr uint64_t kFrameMs  = 16;
        const uint64_t departed_at = t0 + kFrameMs;

        uint64_t first_reflow_ms = 0;
        uint64_t last_seen_2_ms  = 0;
        double   worst_at_rest   = 0.0;
        double   worst_in_hold   = 0.0;

        for (uint64_t ms = departed_at; ms <= departed_at + 3000; ms += kFrameMs) {
            // Participant 2 has left the meeting. Participant 9 flaps in and
            // out on every frame — present in the layout, never in `departed`,
            // so it is churn and not a departure.
            std::vector<DesiredTile> d{{1, three_a}, {3, three_b}};
            if ((ms / kFrameMs) % 2) d.push_back({9, three_c});

            const auto out = a.advance(ms * kMs, d, on, {2});
            const AnimatedTile *t3 = find(out, 3);
            if (first_reflow_ms == 0 && t3 != nullptr && !t3->at_rest)
                first_reflow_ms = ms;
            if (find(out, 2) != nullptr) last_seen_2_ms = ms;
            worst_at_rest = std::fmax(worst_at_rest, worst_at_rest_overlap(out));
            if (first_reflow_ms == 0)
                worst_in_hold = std::fmax(worst_in_hold, worst_overlap(out));
        }

        check(first_reflow_ms != 0,
              "the wall never reflowed after a genuine departure — unrelated "
              "churn stalled the hold indefinitely");
        check(first_reflow_ms != 0 &&
                  first_reflow_ms <= departed_at + kSettleMs + 2 * kFrameMs,
              "the departure hold ran well past kSettleNs under unrelated churn "
              "— its timer is being re-stamped by something other than the "
              "departure itself");
        check(last_seen_2_ms != 0 &&
                  last_seen_2_ms <= departed_at + kSettleMs + 350 + 2 * kFrameMs,
              "the departed participant was still on air past kSettleNs plus its "
              "fade duration");
        check(worst_in_hold == 0.0,
              "tiles overlapped during the departure hold, where every emitted "
              "tile comes from the one old layout");
        check(worst_at_rest == 0.0,
              "two tiles that were both at rest overlapped during a departure "
              "and the reflow that followed");
    }

    // Fix round 4: the two-camera-toggle repro that produced 100% frozen
    // overlap under the settle-everything design. Participant 3's camera goes
    // off and 4's comes on, then 3's comes back one frame later. Neither is a
    // departure — nobody left the meeting — so both changes commit at once and
    // the wall simply reflows. The overlap that remains is a REFLOW transient:
    // 3 and 4 momentarily share a slot while both are moving out of it, and
    // 4 is still nearly transparent, which is what the entry fade is for.
    {
        TileAnimator a;
        const std::vector<DesiredTile> start{{1, three_a}, {2, three_b}, {3, three_c}};
        a.advance(0, start, on, {});
        const uint64_t t0 = settle_in(a, start, on, 0);

        // 3's camera off, 4's on.
        a.advance((t0 + 16) * kMs, {{1, three_a}, {2, three_b}, {4, three_c}}, on, {});
        // 3's camera back on: now four tiles.
        const std::vector<DesiredTile> four_up{
            {1, four_a}, {2, four_b}, {4, four_c}, {3, four_d}};

        double peak = 0.0;
        double alpha_at_peak = 1.0;
        double worst_at_rest = 0.0;
        uint64_t ms = t0 + 16;
        for (int i = 0; i < 60; ++i) {
            ms += 16;
            const auto out = a.advance(ms * kMs, four_up, on, {});
            const double w = worst_overlap(out);
            if (w > peak) {
                peak = w;
                const AnimatedTile *n = find(out, 4);
                alpha_at_peak = n != nullptr ? n->alpha : 1.0;
            }
            worst_at_rest = std::fmax(worst_at_rest, worst_at_rest_overlap(out));
        }
        check(worst_at_rest == 0.0,
              "two tiles that were both at rest overlapped in the two-camera-"
              "toggle sequence");
        check(alpha_at_peak < 0.1,
              "the newcomer was substantially visible at the moment of peak "
              "overlap — the entry fade is what makes this transient invisible");
        check(a.settled(),
              "the wall never settled after the two-camera-toggle sequence");
    }

    // Fix round 4: the empty-wall two-camera repro — the first two people to
    // turn their cameras on, one frame apart. Both are joins, so both commit
    // at once. Participant 2 lands inside participant 1's one-up rect while 1
    // is only starting to shrink, which is the largest transient this design
    // produces; 2's alpha is ~0 exactly there.
    {
        const TileRect one_of_one   = rect(14.0, 8.0, 1890.0, 1064.0);
        const TileRect two_of_two_a = rect(8.0, 274.0, 948.0, 532.0);
        const TileRect two_of_two_b = rect(964.0, 274.0, 948.0, 532.0);

        TileAnimator a;
        a.advance(0, {}, on, {});
        a.advance(1280 * kMs, {{1, one_of_one}}, on, {});   // first camera on

        double peak = 0.0;
        double alpha_at_peak = 1.0;
        double worst_at_rest = 0.0;
        for (uint64_t ms = 1296; ms <= 1296 + 960; ms += 16) {
            const auto out =
                a.advance(ms * kMs, {{1, two_of_two_a}, {2, two_of_two_b}}, on, {});
            const double w = worst_overlap(out);
            if (w > peak) {
                peak = w;
                const AnimatedTile *n = find(out, 2);
                alpha_at_peak = n != nullptr ? n->alpha : 1.0;
            }
            worst_at_rest = std::fmax(worst_at_rest, worst_at_rest_overlap(out));
        }
        check(peak > 400000.0,
              "test setup: the second camera should land inside the first's "
              "one-up rect, otherwise this proves nothing about the fade");
        check(alpha_at_peak < 0.1,
              "the second camera was substantially visible at the moment it was "
              "drawn inside the first");
        check(worst_at_rest == 0.0,
              "two tiles that were both at rest overlapped on the empty-wall "
              "two-camera sequence");
        check(a.settled(), "the wall never settled after both cameras came on");
    }

    // settled() reflects the disabled bypass unconditionally: advance() clears
    // m_tiles on every disabled call (see its own header comment), so
    // settled() must be true right after — no held tile, no exit, no entry
    // fade — regardless of how much in-flight state existed the moment before
    // disabling. This is what makes "disabled" simply a case of "settled" for
    // the render path, and so what keeps the toggle-off output byte-identical
    // to the pre-animation one.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        a.advance(20 * kMs, {{1, rect(0, 0, 400, 400)}}, on, {2});  // now mid-flight, unsettled
        check(!a.settled(), "test setup: the animator should be unsettled before disabling");
        a.advance(40 * kMs, {{1, rect(0, 0, 400, 400)}}, off, {2});
        check(a.settled(),
              "settled() was false immediately after the disabled bypass, which "
              "clears every piece of state settled() reads");
    }

    if (failures == 0) std::cout << "zoom-tile-animator tests passed\n";
    return failures == 0 ? 0 : 1;
}
