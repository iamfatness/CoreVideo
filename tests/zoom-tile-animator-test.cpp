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

// -1 if absent. Used to check relative draw order, not just presence.
static int index_of(const std::vector<AnimatedTile> &tiles, uint32_t id)
{
    for (size_t i = 0; i < tiles.size(); ++i)
        if (tiles[i].participant_id == id) return static_cast<int>(i);
    return -1;
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
    // reappears: while that return is still pending, the participant must not
    // be emitted at all — the vacate erased its motion state, so it is a
    // never-before-seen tile and is withheld until commit (see CRITICAL 1
    // below) — and it must appear at the proposed target, untouched
    // (at_rest), only once the return commits. Either half failing means the
    // wall mistook the return for a fresh start and committed to it
    // immediately.
    {
        TileAnimator a;
        a.advance(0, {{1, rect(0, 0, 100, 100)}}, on, {});
        a.advance(300 * kMs, {}, on, {});                          // empty proposed
        a.advance(560 * kMs, {}, on, {});                          // held 260ms: now genuinely settled on zero
        a.advance(570 * kMs, {{1, rect(0, 0, 100, 100)}}, on, {});  // returns, 10ms later
        const auto pending = a.advance(580 * kMs, {{1, rect(0, 0, 300, 300)}}, on, {});
        check(find(pending, 1) == nullptr,
              "a still-pending return was already on the wall — the return was "
              "treated as a fresh start instead of holding its own settle window");

        // 570ms + 250ms = 820ms: this is the frame the return commits on.
        const auto out = a.advance(830 * kMs, {{1, rect(0, 0, 300, 300)}}, on, {});
        const AnimatedTile *t = find(out, 1);
        check(t != nullptr, "the returning participant was dropped");
        check(t != nullptr && t->at_rest,
              "a committed return was interpolating instead of starting at its target");
        check(t != nullptr && std::fabs(t->rect.width - 300.0) < 0.001,
              "a committed return did not start at the target it was proposed at");
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

        // 2 departs, 3 joins into slotB — 2's old rect. Still pending: 2 is
        // held, and 3 — never seen before — is withheld until the change
        // commits (CRITICAL 1), so during the hold there is no live tile at
        // slotB at all. The ordering contract still governs the moment the
        // change commits, below, which is when the two can first coincide.
        const auto pending = a.advance(300 * kMs, {{1, slotA}, {3, slotB}}, on, {2});
        check(index_of(pending, 2) >= 0,
              "a departing tile was not held through its settle window");
        check(index_of(pending, 3) < 0,
              "a joining tile was emitted into the departing tile's rect before "
              "the change committed");

        // Commits at 560ms (260ms >= 250ms): exit begins, 2 is now fading.
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
        a.advance(0, {{1, rect(0, 0, 100, 100)}, {2, rect(100, 0, 100, 100)}}, on, {});
        check(a.settled(), "a freshly-joined, unmoving wall was not reported settled");

        uint64_t t = 0;
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

    // Final review, CRITICAL 1: a never-before-seen tile must be WITHHELD
    // until its set change commits, not emitted at its target in the
    // meantime.
    //
    // While a change is pending, every committed tile deliberately stays on
    // its OLD target (see the "a blip never moves the wall" contract). A
    // newcomer has no old target, so emitting it at its new one puts one
    // tile in the NEW grid while the whole rest of the wall is still in the
    // OLD one — and the two grids are solved for different counts, so the
    // newcomer's slot lands squarely on top of a participant who is still
    // legitimately on air there. The renderer draws in feed order with
    // newcomers appended last, so the newcomer paints OVER that participant
    // for the whole 250ms window.
    //
    // The rects below are the real 1920x1080 geometry, not convenient
    // synthetic ones: solve_tile_grid() at the shipped defaults (16:9,
    // gutter and margin = canvas_height/135 = 8) for one tile and for two.
    // Measured against those exact numbers, the 1->2 newcomer covers
    // 502203 px^2 of participant 1 — 99% of the newcomer's own area. Every
    // count from 1 to 8 overlaps by 25-100%; departures overlap by 0.
    // The earlier version of this test used {1:(0,0,200,200)} and
    // {2:(200,0,100,100)}, which happen not to overlap at all, and so
    // asserted the defect was correct behaviour.
    //
    // Withholding is what the spec already buys: "the wall reacts one
    // settle window later" is the stated cost of the settle window, and a
    // join is exactly that. The render side needs nothing: a feed with no
    // matching animated entry gets SnappedTileRect{} and is skipped by the
    // width < 2 guard in tiles_source_render().
    {
        const TileRect one_of_one   = rect(14.2222, 8.0, 1891.5556, 1064.0);
        const TileRect two_of_two_a = rect(8.0, 273.375, 948.0, 533.25);
        const TileRect two_of_two_b = rect(964.0, 273.375, 948.0, 533.25);

        TileAnimator a;
        a.advance(0, {{1, one_of_one}}, on, {});
        check(a.settled(), "a single settled participant was not reported settled");

        uint64_t t = 0;
        auto step = [&](const std::vector<DesiredTile> &d) {
            t += 20 * kMs;
            return a.advance(t, d, on, {});
        };

        // Steps land on t = 20, 40, ... 260ms. The join is first proposed on
        // the t=20ms step, so the last of these is 240ms into the 250ms hold.
        for (int i = 0; i < 13; ++i) {
            const auto out =
                step({{1, two_of_two_a}, {2, two_of_two_b}});

            const AnimatedTile *newcomer = find(out, 2);
            check(newcomer == nullptr,
                  "a never-before-seen tile was emitted during the settle "
                  "window, stamping the new grid's slot over a participant "
                  "still drawn in the old one");

            // The incumbent is still exactly where it was, in the OLD grid —
            // which is the whole reason the newcomer cannot be drawn yet.
            const AnimatedTile *incumbent = find(out, 1);
            check(incumbent != nullptr, "the incumbent was lost during a pending join");
            check(incumbent != nullptr &&
                      std::fabs(incumbent->rect.x - one_of_one.x) < 0.001 &&
                      std::fabs(incumbent->rect.width - one_of_one.width) < 0.001,
                  "the incumbent moved before the join had settled");

            check(!a.settled(),
                  "settled() was true while a join was still pending, where a "
                  "fresh solve for the new count would already disagree with "
                  "the wall");
        }

        // t is 260ms here; the change was proposed at 20ms, so the next step
        // (280ms, 260ms later) is the frame it commits on. The newcomer must
        // appear on exactly that frame — withheld, not dropped — and at its
        // own target, since it has nowhere to travel from.
        const auto committed = step({{1, two_of_two_a}, {2, two_of_two_b}});
        const AnimatedTile *arrived = find(committed, 2);
        check(arrived != nullptr,
              "the newcomer did not appear on the frame its join committed");
        check(arrived != nullptr &&
                  std::fabs(arrived->rect.x - two_of_two_b.x) < 0.001 &&
                  std::fabs(arrived->rect.y - two_of_two_b.y) < 0.001,
              "the newcomer did not appear at its own target once the join committed");

        // The incumbent reflows into the two-up grid from where it was. Run
        // well past the 350ms settle time and the wall is settled again.
        for (int i = 0; i < 40; ++i)
            step({{1, two_of_two_a}, {2, two_of_two_b}});
        check(a.settled(),
              "settled() was still false long after the join committed and the "
              "incumbent's spring had arrived");
    }

    // settled() reflects the disabled bypass unconditionally: advance()
    // clears m_tiles and m_has_pending on every disabled call (see its own
    // header comment), so settled() must be true right after, regardless of
    // how much in-flight state existed the moment before disabling.
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
