// src/zoom-tile-animator.h
#pragma once

#include "tile-motion.h"
#include "zoom-tile-grid.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

struct AnimationSettings {
    bool   enabled          = false;
    double duration_seconds = 0.35;
};

struct DesiredTile {
    uint32_t participant_id = 0;
    TileRect rect;
};

struct AnimatedTile {
    uint32_t participant_id = 0;
    TileRect rect;
    double   alpha   = 1.0;
    bool     at_rest = true;
};

class TileAnimator {
public:
    // Ordering contract: the returned vector lists held and exiting tiles
    // first, live tiles from `desired` last. A single set change can hand a
    // departing participant's rect to a joiner (a departure and a join at
    // once; the layout solved for the new set reuses the vacated slot), so
    // a consumer MUST draw this vector in the order returned — that is what
    // makes the live tile paint over a held or fading ghost at the same
    // rect rather than being hidden under it. Do not reorder the two loops
    // below without preserving this.
    std::vector<AnimatedTile> advance(uint64_t now_ns,
                                      const std::vector<DesiredTile> &desired,
                                      const AnimationSettings &settings,
                                      const std::vector<uint32_t> &departed)
    {
        // Disabled is a bypass, not a fast setting: every piece of animation
        // state is discarded — not merely left alone, which would let a
        // stale in-flight position or a half-expired settle window resume the
        // moment the toggle came back — and the desired layout is emitted
        // verbatim, so the renderer takes exactly the path it took before
        // this feature existed. Clearing it here is also what makes
        // settled() unconditionally true straight after a disabled call,
        // which is what lets the render path treat "disabled" as simply a
        // case of "settled" rather than as a second branch to keep in step.
        if (!settings.enabled) {
            m_tiles.clear();
            m_last_ns = 0;
            m_has_last = false;
            m_committed_ids.clear();
            m_pending_ids.clear();
            m_pending_since_ns = 0;
            m_has_committed = false;
            m_has_pending = false;
            m_withheld_since.clear();
            std::vector<AnimatedTile> out;
            out.reserve(desired.size());
            for (const auto &d : desired)
                out.push_back(AnimatedTile{d.participant_id, d.rect, 1.0, true});
            return out;
        }

        // An explicit "have we been called before" flag, not a sentinel value
        // of m_last_ns. Zero is a legitimate timestamp — the tests advance from
        // 0 — and overloading it means m_last_ns stays 0 after a first call at
        // 0, so dt is forced to zero on two consecutive calls and the wall
        // silently loses a frame of motion.
        const double dt = (!m_has_last || now_ns <= m_last_ns)
            ? 0.0
            : static_cast<double>(now_ns - m_last_ns) / 1e9;
        m_last_ns = now_ns;
        m_has_last = true;

        // Decide which participant set the wall is laid out for. A change is
        // only adopted once it has held for kSettleNs; a reversal inside that
        // window is forgotten and nothing moves.
        const std::vector<uint32_t> incoming = ids_of(desired);
        if (!m_has_committed) {
            // An explicit flag, not m_committed_ids.empty(): a wall that has
            // legitimately settled on zero participants looks identical to a
            // freshly constructed animator if all we check is emptiness, and
            // a return from a genuine vacate must not skip the settle gate
            // the way a true first frame is allowed to.
            m_committed_ids = incoming;
            m_has_committed = true;
        } else if (incoming != m_committed_ids) {
            // m_has_pending, not "m_pending_ids is non-empty": an empty roster
            // is a legitimate proposal — everyone dropping out at once is the
            // flicker case this gate exists for — and it is indistinguishable
            // from the default-empty vector. Without the flag, an empty
            // proposal falls through to the elapsed-time branch and is measured
            // against a stale m_pending_since_ns, so it is adopted with no hold
            // at all.
            if (!m_has_pending || incoming != m_pending_ids) {
                m_pending_ids = incoming;
                m_pending_since_ns = now_ns;
                m_has_pending = true;
            } else if (now_ns - m_pending_since_ns >= kSettleNs) {
                m_committed_ids = incoming;
                m_pending_ids.clear();
                m_has_pending = false;
            }
        } else {
            m_pending_ids.clear();
            m_has_pending = false;
        }
        // Same reason as above: an empty proposal is still a pending change, so
        // this must read the flag, not the vector's emptiness.
        const bool change_pending = m_has_pending;

        // Exit lifecycle. These four rules bound the exception to the rule in
        // zoom-tile-slot.h — that a stored frame stops being shown the instant a
        // slot is repointed, written after the wrong face reached air:
        //   1. only a genuine roster departure may start an exit
        //   2. a reassignment cuts instantly, no hold and no fade
        //   3. any repoint cancels a running exit immediately
        //   4. an exit can never outlive its duration
        //
        // Presence in the *layout* — this frame's `desired` — cancels a
        // hold or a running exit immediately (3). `departed` is
        // roster-absence state, not a one-shot event, so a participant can
        // be in both `desired` and `departed` at once: repointed back into
        // a slot while still absent from the meeting roster. A correct
        // caller cannot drop them from `departed` just because a slot shows
        // them again, so committed-set membership by itself cannot be
        // trusted to mean "still exiting" — presence in `desired` must
        // cancel a running exit regardless of what the settle window has or
        // hasn't committed to yet. (Invariant 2 is unaffected: a reassigned
        // participant is by definition never back in `desired`.)
        //
        // Absence from `desired` alone must NOT erase a tile. A one-frame
        // blip and the pending phase of a genuine departure or
        // reassignment are indistinguishable at the moment they start —
        // all three are retained identically, motion untouched, until the
        // wall's committed set actually drops the participant or this
        // tile's own hold clock (below) runs out. Gating erasure on
        // `desired` presence alone, rather than `m_committed_ids`, was a
        // regression caught in review: it erased and rebuilt the Motion
        // from scratch on every single dropped frame, snapping a mid-flight
        // tile to its target the instant `desired` next included it again —
        // the exact pop the settle window exists to prevent, even though it
        // never puts a wrong face on air.
        //
        // The hold — absent from `desired`, genuinely departed, but not yet
        // exiting because the whole-set settle window hasn't committed the
        // removal — has its own clock, independent of that settle timer.
        // The timer resets whenever the proposed set changes from the last
        // one seen, so a single unrelated participant flapping in and out
        // of `desired` resets it on every call and would otherwise hold a
        // departed participant on screen indefinitely — the same fault
        // class `zoom-tile-slot.h` exists to prevent, just delayed rather
        // than avoided. `held_since_ns` is stamped the first time a tile is
        // observed absent from `desired`; once `kSettleNs` has elapsed by
        // that clock, the tile stops being retained regardless of whether
        // the settle window has committed: a genuine departure begins its
        // exit, and a reassignment stalled by unrelated churn is cut. Both
        // bound total time spent absent-but-unresolved at kSettleNs,
        // independent of what the rest of the roster does.
        for (auto it = m_tiles.begin(); it != m_tiles.end();) {
            const bool in_desired =
                std::find(incoming.begin(), incoming.end(), it->first) != incoming.end();
            if (in_desired) {
                it->second.exiting = false;      // (3) back in the layout
                it->second.alpha = 1.0;
                it->second.held = false;         // a future departure gets its own fresh clock
                ++it;
                continue;
            }

            // Absent from `desired`. Stamp when that was first observed, so
            // it cannot be pushed out by unrelated roster churn later.
            if (!it->second.held && !it->second.exiting) {
                it->second.held = true;
                it->second.held_since_ns = now_ns;
            }

            if (!it->second.exiting) {
                const bool committed =
                    std::find(m_committed_ids.begin(), m_committed_ids.end(),
                              it->first) != m_committed_ids.end();
                const bool hold_expired =
                    (now_ns - it->second.held_since_ns) >= kSettleNs;
                if (committed && !hold_expired) {
                    // Indistinguishable, at this point, from a one-frame
                    // blip that is about to return: retain untouched.
                    // Whether this turns out to be a blip, a pending
                    // departure, or a pending reassignment is resolved
                    // below, once the wall's committed set actually drops
                    // the participant or this tile's own hold clock runs
                    // out — never merely from being absent for one frame.
                    ++it;
                    continue;
                }
                const bool left_roster =
                    std::find(departed.begin(), departed.end(), it->first) != departed.end();
                if (!left_roster) {
                    // ERASED, not merely withheld from the output. A tile
                    // kept here would be invisible — nothing emits a
                    // reassigned participant — right up until that
                    // participant later leaves the meeting for real, at which
                    // point `departed` names it and it begins a full-opacity
                    // exit from a frame recorded before the repoint: the
                    // wrong face on air, which is exactly what
                    // src/zoom-tile-slot.h exists to prevent. The
                    // corresponding test is the one that pins this shape;
                    // every other test in the suite passes with the tile
                    // retained instead.
                    it = m_tiles.erase(it);      // (2) reassignment: instant, now that it is genuinely resolved
                    continue;
                }
                // Either the whole-set settle window committed the
                // removal, or this tile's own hold clock ran out
                // regardless: begin the exit either way (1).
                it->second.exiting = true;
                it->second.exit_started_ns = now_ns;
            }

            const double elapsed =
                static_cast<double>(now_ns - it->second.exit_started_ns) / 1e9;
            if (elapsed >= settings.duration_seconds) {
                it = m_tiles.erase(it);          // (4) time-boxed
                continue;
            }
            it->second.alpha = 1.0 - elapsed / settings.duration_seconds;
            ++it;
        }

        // The withhold clocks (see the withholding branch below) live only
        // for as long as a tile is proposed and still yielding. The branch
        // itself clears an entry the moment its tile is drawn; this sweep
        // covers the two cases that branch never sees again — an id that has
        // left the layout, and one that has stopped being a guest because it
        // was committed. A proposal that comes back later is a fresh
        // withholding and gets a fresh clock, exactly as `held` is reset for
        // a tile that returns to `desired` on the departure side.
        for (auto it = m_withheld_since.begin(); it != m_withheld_since.end();) {
            const bool in_layout =
                std::find(incoming.begin(), incoming.end(), it->first) != incoming.end();
            const auto tile = m_tiles.find(it->first);
            const bool no_longer_a_guest =
                tile != m_tiles.end() && !tile->second.provisional;
            if (!in_layout || no_longer_a_guest)
                it = m_withheld_since.erase(it);
            else
                ++it;
        }

        std::vector<AnimatedTile> out;
        // Upper bound: every desired tile, plus every tracked tile the loop
        // below might append (exiting, or held mid-departure).
        out.reserve(desired.size() + m_tiles.size());

        // Held and exiting tiles are emitted before live tiles from
        // `desired` — see the ordering contract on advance(). A set change
        // can hand a departing participant's rect to a joiner in the same
        // frame (a departure and a join in one change; the layout solved
        // for the new set reuses the vacated slot), so the departing tile
        // must be earlier in `out` than the live one for a draw-in-order
        // consumer to paint the live tile on top rather than under a
        // frozen or fading ghost at the same rect.
        for (const auto &entry : m_tiles) {
            if (entry.second.exiting) {
                // Fading. Deliberately not gated on presence in `desired`:
                // that invariant — a tile the loop below places back in
                // `desired` always has `exiting == false` by construction,
                // because the branch above already forced it false — is
                // exactly what must hold for this to never double-emit a
                // repointed participant. Gating this check on `desired`
                // membership would paper over a broken reset instead of
                // relying on it, and silently hide the exact bug
                // invariant 3 exists to catch.
                TileRect r;
                r.x = entry.second.x.position; r.y = entry.second.y.position;
                r.width = entry.second.w.position; r.height = entry.second.h.position;
                out.push_back(AnimatedTile{entry.first, r, entry.second.alpha, false});
                continue;
            }

            // Not exiting. Anything present in `desired` this frame was
            // already promoted out of the hold above (or was never held);
            // it is emitted by the loop below instead.
            const bool in_desired =
                std::find(incoming.begin(), incoming.end(), entry.first) != incoming.end();
            if (in_desired) continue;

            // Retained here (see the lifecycle loop above) but not yet
            // exiting: could be a one-frame blip, a pending reassignment,
            // or a pending departure — all three are retained identically
            // for motion-continuity purposes, and only `departed` tells
            // them apart for display. A blip or a pending reassignment
            // must stay invisible for the whole window — showing a
            // reassignment on screen, even briefly, is itself an
            // invariant-2 violation, just delayed rather than avoided; only
            // a genuine, still-pending departure is shown here, frozen,
            // until its exit begins.
            const bool left_roster =
                std::find(departed.begin(), departed.end(), entry.first) != departed.end();
            if (!left_roster) continue;

            TileRect r;
            r.x = entry.second.x.position; r.y = entry.second.y.position;
            r.width = entry.second.w.position; r.height = entry.second.h.position;
            out.push_back(AnimatedTile{entry.first, r, 1.0, true});
        }

        // Every rect that will be drawn as a LIVE participant this frame. A
        // never-before-seen tile is checked against these, and only these,
        // before it is allowed on screen (see the withholding branch below).
        //
        // Held and exiting tiles are deliberately NOT in here. The ordering
        // contract at the top of this function exists precisely so a live
        // tile may paint over a held or fading ghost at the same rect — that
        // is the intended look when a departure hands its slot to a joiner —
        // and the render path does not draw them at all today. Including them
        // would also defeat the whole point of this test: an operator
        // repointing a slot leaves the OUTGOING participant retained here,
        // invisible, at exactly the rect the incoming one wants, so every
        // repoint would look like an overlap and be delayed.
        //
        // Guests already on the wall ARE in here, and carry their id so the
        // tile being tested can skip itself. An earlier version excluded
        // every provisional tile, on the reasoning that "two guests are slots
        // in the same grid and are disjoint by construction". That premise is
        // false: guests released at different moments are seeded against
        // different PROPOSALS, so different grids, so nothing makes them
        // disjoint. The two people who turn their cameras on first, within
        // one settle window of each other, are enough — the first is released
        // into the 1-up layout at (14,8 1890x1064) and the second into the
        // 2-up layout at (964,274 948x532), entirely inside it: 500080 px^2,
        // 99% of the second tile, frozen for the whole window. Excluding only
        // self is what makes the earlier guest keep the space it already
        // holds and the later one wait, instead of both being blind to each
        // other.
        //
        // Read before the springs below advance, so a tile is measured where
        // it was at the end of the previous frame rather than one step into
        // this one. The difference cannot matter for the cases this decides:
        // a never-seen tile only exists while a set change is pending, and a
        // pending change is exactly when committed tiles are NOT retargeted.
        std::vector<std::pair<uint32_t, TileRect>> live_rects;
        live_rects.reserve(desired.size());
        for (const auto &d : desired) {
            auto it = m_tiles.find(d.participant_id);
            if (it != m_tiles.end()) {
                TileRect r;
                r.x = it->second.x.position; r.y = it->second.y.position;
                r.width = it->second.w.position; r.height = it->second.h.position;
                live_rects.emplace_back(d.participant_id, r);
                continue;
            }
            // No motion state. A committed one is created at its target
            // further down and drawn there this frame; an uncommitted one is
            // a newcomer with nothing on screen yet, so it holds no space for
            // anyone else to yield to.
            const bool committed =
                std::find(m_committed_ids.begin(), m_committed_ids.end(),
                          d.participant_id) != m_committed_ids.end();
            if (committed) live_rects.emplace_back(d.participant_id, d.rect);
        }

        for (const auto &d : desired) {
            const bool committed =
                std::find(m_committed_ids.begin(), m_committed_ids.end(),
                          d.participant_id) != m_committed_ids.end();
            if (!committed) {
                // Present in the layout but not yet adopted by the settle
                // window. A tile that already has motion state is held
                // exactly where it is, without disturbing the rest of the
                // wall — that is the blip case, and it must not move.
                //
                // A tile with NO motion state has never been on the wall.
                // Emitting it unconditionally at its target would put one
                // tile in the layout solved for the NEW set while every
                // committed tile is deliberately still sitting on its OLD
                // target (see the retarget gate below) — two different grids
                // on screen at once. When the grids are solved for different
                // counts the newcomer's slot lands on top of a participant
                // who is still legitimately drawn in the old one, and the
                // renderer draws in feed order with newcomers appended last,
                // so it paints OVER that participant's face. Measured against
                // the real solve_tile_grid() at 1920x1080, every count-
                // changing join from 1->2 through 15->16 overlaps an existing
                // tile (1->2: 502203 px^2, 99% of the newcomer's own area).
                // Departures do not have this problem — nothing new appears.
                //
                // So the tile is withheld, but only for as long as it would
                // actually cover someone, and never indefinitely. TWO
                // independent releases, either of which is enough:
                //
                //   - DISJOINT. The overlap above is a property of the
                //     geometry, not of the settle window, so it is tested
                //     directly rather than assumed. A newcomer whose target
                //     touches nothing being drawn cannot cover a face and
                //     goes on screen immediately. That is what makes an
                //     operator repointing one slot cut instantly, as it did
                //     before this feature existed: the grid is identical on
                //     both sides of a same-count repoint, so the incoming
                //     participant's slot is disjoint from every other slot —
                //     measured true at all 15 counts from 2 to 16 — and
                //     waiting would buy nothing, since nothing moves at
                //     commit either. Withholding there is pure dead time, and
                //     worse than it looks: a withheld feed gets an empty
                //     SnappedTileRect, so the width < 2 guard skips it in the
                //     glow loop as well as the tile loop and the slot loses
                //     its tile, border AND glow, where before this feature it
                //     drew a neutral placeholder immediately.
                //
                //   - ITS OWN CLOCK. Stamped the first frame it is withheld,
                //     and independent of the whole-set settle timer, for the
                //     same reason held_since_ns is on the departure side (see
                //     the lifecycle loop above): that timer re-stamps whenever
                //     the proposed set differs from the last one seen, so a
                //     single unrelated participant flapping in and out resets
                //     it on every call and nothing ever commits. Measured
                //     before this clock existed: a newcomer continuously
                //     present in `desired`, with one unrelated participant
                //     flapping every 100ms, was emitted 0 times in 30
                //     seconds — a participant who joined and never appeared,
                //     which is a far worse failure than a brief overlap.
                //     Without churn the same join took 256ms.
                //
                // The clock is kept beside m_tiles rather than as a flag on a
                // Motion created early: a Motion seeded on first sight would
                // carry the rect from that first frame, and the proposal can
                // change while the tile is withheld, so a clock-released tile
                // would appear at a stale rect. Created on release instead,
                // it is always seeded at the target the layout is proposing
                // right now.
                //
                // Releasing a tile early makes it a GUEST of a layout that
                // has not adopted it, and the test above has to keep holding
                // for as long as it stays one — a single check at release
                // time is not enough. Two camera toggles inside 250ms are all
                // it takes: participant 3 drops out, newcomer 4 is released
                // into 3's vacated slot because that slot is now free, then 3
                // comes back. 3 is committed, so it is redrawn frozen at that
                // same slot — a pending change must not retarget it — and the
                // release decision for 4 was made a frame earlier, when 3 was
                // retained but invisible and so in neither the live set nor
                // the emitted one. Both are live, both are drawn, both at the
                // identical rect: measured 495264 px^2, 100% overlap, held
                // for the whole settle window, with the incoming grid's
                // fourth slot blank meanwhile.
                //
                // So a guest is re-tested every frame, against where it would
                // actually be drawn, and yields the moment the committed
                // layout wants that space back. It stops being a guest — and
                // stops yielding forever after — when it is committed, or
                // when its own clock expires, whichever comes first. That
                // second promotion is what keeps the bound from part 1
                // intact: under churn that never commits, the joiner still
                // appears within kSettleNs and is not strobed off again on
                // the next frame.
                //
                // Letting a released guest track the current proposal instead
                // of freezing it was considered and rejected. It moves the
                // guest into the NEW grid while every committed tile is still
                // in the OLD one — which is the two-grids-at-once defect this
                // whole branch exists to prevent. Measured on the repro
                // above, it converges to 246048 px^2, a SUSTAINED 50%
                // overlap, on top of a ~100% transient: better than 100% for
                // the full window, but squarely back in the 25-100% band that
                // made this a defect in the first place. Yielding measures 0.
                auto tile = m_tiles.find(d.participant_id);
                const bool never_seen = (tile == m_tiles.end());

                // Where it would be drawn if it were emitted this frame: a
                // never-seen tile would be seeded at its target, and anything
                // else is frozen where it already is.
                TileRect would_draw = d.rect;
                if (!never_seen) {
                    would_draw.x      = tile->second.x.position;
                    would_draw.y      = tile->second.y.position;
                    would_draw.width  = tile->second.w.position;
                    would_draw.height = tile->second.h.position;
                }

                bool courtesy_expired = false;
                if (never_seen || tile->second.provisional) {
                    bool covers_someone = false;
                    for (const auto &r : live_rects) {
                        // Self, not a collision. Deleting this skip instead
                        // of narrowing the loop above makes every guest
                        // compare against its own rect and yield to itself.
                        if (r.first == d.participant_id) continue;
                        if (rects_overlap(would_draw, r.second)) {
                            covers_someone = true;
                            break;
                        }
                    }
                    if (!covers_someone) {
                        // Clear as this frame stands. Its clock is cleared
                        // too, exactly as `held` is cleared for a tile that
                        // returns to `desired` on the departure side, so a
                        // later yield is measured from when THAT yield began.
                        m_withheld_since.erase(d.participant_id);
                    } else {
                        // KNOWN COST, recorded rather than fixed — the repo
                        // owner is deciding on it. A guest that is ALREADY on
                        // screen goes dark here: it was drawn while its slot
                        // was free, and yields the moment something claims
                        // that space. Over 3000 randomised runs that produced
                        // 1474 dark episodes, 22% of them after only 1 to 3
                        // visible frames — a face flashes for ~32ms and the
                        // slot drops to background for up to 256ms. Neither
                        // the unconditional withhold nor the first
                        // disjointness release did that. It is bounded, not a
                        // ping-pong: the clock below guarantees promotion, so
                        // it is a flash and a gap, never an oscillation. But
                        // it is the same artefact shape rejected elsewhere in
                        // this file, and the alternative — keeping a guest on
                        // screen once drawn — puts it back under the face
                        // that claimed the space.
                        auto since = m_withheld_since.find(d.participant_id);
                        if (since == m_withheld_since.end())
                            since = m_withheld_since
                                        .emplace(d.participant_id, now_ns).first;
                        if ((now_ns - since->second) < kSettleNs)
                            continue;                    // yielding

                        // KNOWN GAP, same status. Promoting here re-creates
                        // the very overlap the yield above prevents, whenever
                        // this clock and the whole-set settle clock do not
                        // expire together — only the latter is re-stamped by
                        // roster churn, so one unrelated blip drives them
                        // apart. Measured under sustained churn: 284
                        // consecutive frames, 4.5 seconds, at 495264 px^2.
                        // The promotion is not optional — it is what stops a
                        // yielding guest being suppressed forever, which an
                        // earlier round required after a joiner was measured
                        // never appearing in 30 seconds — so the two
                        // requirements genuinely conflict and this resolves
                        // toward liveness. The honest statement of what this
                        // branch guarantees is "a guest yields while it would
                        // cover, until its clock runs out", NOT "two live
                        // tiles never overlap".
                        m_withheld_since.erase(since);
                        courtesy_expired = true;
                    }
                }

                if (never_seen) {
                    Motion m;
                    m.x = {d.rect.x, 0.0};
                    m.y = {d.rect.y, 0.0};
                    m.w = {d.rect.width, 0.0};
                    m.h = {d.rect.height, 0.0};
                    m.target = d.rect;
                    m.provisional = !courtesy_expired;
                    m_tiles.emplace(d.participant_id, m);
                } else if (courtesy_expired) {
                    tile->second.provisional = false;
                }
                out.push_back(AnimatedTile{d.participant_id, would_draw, 1.0, true});
                continue;
            }

            auto it = m_tiles.find(d.participant_id);
            if (it == m_tiles.end()) {
                // First sight of this participant: start at the target rather
                // than flying in from the origin.
                Motion m;
                m.x = {d.rect.x, 0.0};
                m.y = {d.rect.y, 0.0};
                m.w = {d.rect.width, 0.0};
                m.h = {d.rect.height, 0.0};
                m.target = d.rect;
                it = m_tiles.emplace(d.participant_id, m).first;
            }

            Motion &m = it->second;

            // Committed: the wall's layout now includes this tile, so it is
            // no longer a guest on someone else's and never yields again.
            m.provisional = false;

            // Adopt a new target only when the layout in hand was solved for
            // the set the wall has committed to. While a change is pending, the
            // incoming rects belong to a layout that may never happen — a blip
            // that reverts must leave every tile exactly where it was.
            if (!change_pending && committed)
                m.target = d.rect;

            spring_advance(m.x, m.target.x,      settings.duration_seconds, dt);
            spring_advance(m.y, m.target.y,      settings.duration_seconds, dt);
            spring_advance(m.w, m.target.width,  settings.duration_seconds, dt);
            spring_advance(m.h, m.target.height, settings.duration_seconds, dt);

            TileRect r;
            r.x = m.x.position; r.y = m.y.position;
            r.width = m.w.position; r.height = m.h.position;

            const bool at_rest =
                std::fabs(r.x - m.target.x) < kRestEpsilon &&
                std::fabs(r.y - m.target.y) < kRestEpsilon &&
                std::fabs(r.width - m.target.width) < kRestEpsilon &&
                std::fabs(r.height - m.target.height) < kRestEpsilon;

            out.push_back(AnimatedTile{d.participant_id, r, 1.0, at_rest});
        }

        return out;
    }

    // Read-only: which participant ids currently have tracked motion state
    // (in flight, held, or exiting). The caller uses this, once per frame in
    // the same pass the layout is solved and advance() is called, to compute
    // `departed` (roster-absence for every id this animator is still
    // carrying) without this class needing to know anything about the
    // roster itself. Empty right after construction, and cleared along with
    // everything else by the disabled bypass in advance() above.
    std::vector<uint32_t> tracked_ids() const
    {
        std::vector<uint32_t> ids;
        ids.reserve(m_tiles.size());
        for (const auto &entry : m_tiles) ids.push_back(entry.first);
        return ids;
    }

    // True when the wall is showing the layout it has committed to and
    // nothing is moving: no pending set change, no held or exiting tile, and
    // every spring already at its own target. This is a STRICTER condition
    // than "every AnimatedTile advance() just returned reports at_rest" —
    // that per-tile flag is true in two situations where the wall as a whole
    // still disagrees with a fresh solve_tile_grid()/snap_tile_grid_even()
    // call:
    //
    //   - During a departure's settle hold, the held tile reports at_rest
    //     (it is frozen, not moving) while the survivors also report
    //     at_rest — sitting on their OLD remembered targets, since a pending
    //     change must not retarget them yet (see the "blip never moves the
    //     wall" contract above). A fresh solve, meanwhile, already describes
    //     the NEW, smaller grid, because feed-list resizing is not gated on
    //     this animator's settle window at all. Drawing the fresh solve
    //     during this window shows the wrong size.
    //   - During a join's settle hold, symmetrically: every already-committed
    //     tile holds its OLD target and reports at_rest, while a fresh solve
    //     already describes the LARGER grid the newcomer will join. The
    //     newcomer is either withheld — see the not-committed branch in
    //     advance(), where it yields for as long as it would cover a tile the
    //     committed layout is drawing — or released early into a slot that
    //     layout left free, in which case it sits perfectly still and reports
    //     at_rest like everything else. Either way nothing in the emitted
    //     tiles distinguishes this frame from a settled wall.
    //
    // Only when nothing is pending AND every tracked tile is a plain, fully
    // arrived, non-held, non-exiting entry does the fresh solve match what
    // this animator would draw — that is what makes the caller's whole-grid
    // snap safe to use instead of the animator's own (slower, per-tile)
    // rects. See the caller in src/zoom-supersource.cpp for the render-side
    // half of this contract.
    bool settled() const
    {
        if (m_has_pending) return false;
        for (const auto &entry : m_tiles) {
            const Motion &m = entry.second;
            if (m.held || m.exiting) return false;
            if (std::fabs(m.x.position - m.target.x) >= kRestEpsilon) return false;
            if (std::fabs(m.y.position - m.target.y) >= kRestEpsilon) return false;
            if (std::fabs(m.w.position - m.target.width) >= kRestEpsilon) return false;
            if (std::fabs(m.h.position - m.target.height) >= kRestEpsilon) return false;
        }
        return true;
    }

private:
    struct Motion {
        Spring1D x, y, w, h;
        TileRect target;
        double   alpha           = 1.0;
        bool     exiting         = false;
        uint64_t exit_started_ns = 0;
        // The hold (absent from `desired`, not yet exiting) has its own
        // clock, stamped the first time the absence is observed. See the
        // lifecycle loop in advance() for why it cannot rely on the
        // whole-set settle timer instead.
        bool     held            = false;
        uint64_t held_since_ns   = 0;
        // Created by an early release — the animator put this tile on the
        // wall ahead of the settle window adopting it, because its slot was
        // free (or because its withhold clock ran out). A provisional tile is
        // a GUEST in the committed layout: it yields to any tile that layout
        // is actually drawing. Cleared the moment the tile is committed, and
        // when its withhold clock expires. See the withholding branch in
        // advance().
        bool     provisional     = false;
    };

    // Do the two rects share more than a sliver? Used to decide whether a
    // never-before-seen tile would cover a participant already on screen.
    //
    // The half-pixel tolerance is not floating-point hygiene, it is the
    // difference between a test and a hair trigger. Tiles that are adjacent
    // with the gutter set to zero share an edge exactly, and a spring that
    // has arrived sits a hair either side of its target, so an exact test
    // would call two touching tiles an overlap and delay a join for a
    // sub-pixel sliver that cannot hide any part of a face.
    static bool rects_overlap(const TileRect &a, const TileRect &b)
    {
        constexpr double kSliver = 0.5;
        const double left   = std::fmax(a.x, b.x);
        const double top    = std::fmax(a.y, b.y);
        const double right  = std::fmin(a.x + a.width,  b.x + b.width);
        const double bottom = std::fmin(a.y + a.height, b.y + b.height);
        return (right - left) > kSliver && (bottom - top) > kSliver;
    }

    static std::vector<uint32_t> ids_of(const std::vector<DesiredTile> &d)
    {
        std::vector<uint32_t> ids;
        ids.reserve(d.size());
        for (const auto &t : d) ids.push_back(t.participant_id);
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    // A roster change must hold this long before the wall reacts. The roster is
    // known to flicker — SpeakerDirector carries a 60s absence grace for the
    // same reason — and without this a 120ms dropout would produce a full exit
    // animation followed by a full entry, which is more visible on air than the
    // single-frame pop it replaces. Fixed, not a setting: zero would reintroduce
    // exactly the behaviour this exists to prevent.
    static constexpr uint64_t kSettleNs = 250000000ULL;

    // Shared by advance()'s own at_rest field and settled() above, so the
    // two can never disagree about what "arrived" means for the same spring
    // state. Sub-tenth-pixel: tight enough that a caller treating "at rest"
    // as pixel-exact never observes a rounding difference against the
    // spring's true target.
    static constexpr double kRestEpsilon = 0.05;

    std::map<uint32_t, Motion> m_tiles;
    uint64_t m_last_ns  = 0;
    bool     m_has_last = false;

    std::vector<uint32_t> m_committed_ids;   // the set the wall is laid out for
    std::vector<uint32_t> m_pending_ids;     // a candidate set, not yet settled
    uint64_t m_pending_since_ns = 0;
    bool     m_has_committed = false;

    // Distinguishes "an empty roster is proposed" from "nothing is proposed".
    bool m_has_pending = false;

    // When each never-before-seen tile was first withheld. Its own clock, not
    // the whole-set settle timer, which unrelated churn can re-stamp forever.
    // Swept in advance(); see the sweep for what owns each entry's lifetime.
    std::map<uint32_t, uint64_t> m_withheld_since;
};
