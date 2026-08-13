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
        // stale in-flight position or a half-finished fade resume the moment
        // the toggle came back — and the desired layout is emitted verbatim
        // at full opacity, so the renderer takes exactly the path it took
        // before this feature existed. Clearing it here is also what makes
        // settled() unconditionally true straight after a disabled call,
        // which is what lets the render path treat "disabled" as simply a
        // case of "settled" rather than as a second branch to keep in step.
        if (!settings.enabled) {
            m_tiles.clear();
            m_last_ns = 0;
            m_has_last = false;
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

        const std::vector<uint32_t> incoming = ids_of(desired);

        // ── The settle window applies to DEPARTURES ONLY ─────────────────────
        //
        // It used to gate every set change: the whole roster had to hold for
        // kSettleNs before the wall adopted it. That is what produced this
        // file's entire defect history, because it kept committed tiles on
        // their OLD targets while a joiner wanted the NEW layout, so two
        // grids were alive at once and something had to give — the joiner was
        // invisible, or it was drawn over a face, or it was drawn and then
        // blanked again. Each round moved the damage rather than removing it.
        //
        // The window only ever existed to stop a roster BLIP producing a full
        // exit animation followed by a full entry. A join has no exit to
        // suppress, so gating it bought nothing and cost the mixed state. So:
        //
        //   1. No departure pending -> any set change commits immediately,
        //      and a join reflows the wall at once.
        //   2. A departure pending -> hold everything. Nothing adopts a new
        //      target, and a joiner is not drawn until it resolves. Safe
        //      precisely because NOTHING is on the new grid: every emitted
        //      tile comes from the one old layout, so no two can overlap.
        //   3. A departure is an absence from `desired` by a participant who
        //      is also in `departed` — genuinely gone from the roster. A
        //      reassignment (absent from the layout, still in the meeting) is
        //      NOT a departure and does not hold anything, which is what
        //      keeps an operator repointing a slot instant.
        //   4. A departure that reverts inside its window cancels: the tile
        //      comes back to `desired`, its hold is cleared, and nothing
        //      moved.
        //
        // The timer is per tile — `held_since_ns`, stamped the first frame
        // that tile is observed absent — and NOT a whole-set timer. That
        // distinction is load-bearing: a whole-set timer re-stamps on every
        // set change, so one unrelated participant flapping in and out stalls
        // it forever, and a departure hold that never ends is the failure
        // this animator exists to prevent, just delayed rather than avoided.
        // A flapper cannot extend anything here, because the condition below
        // is evaluated from the tiles absent on THIS frame: on every frame a
        // flapper is present it contributes nothing, so the wall commits and
        // reflows normally.
        //
        // ── Exit lifecycle ───────────────────────────────────────────────────
        //
        // These four rules bound the exception to the rule in
        // zoom-tile-slot.h — that a stored frame stops being shown the instant
        // a slot is repointed, written after the wrong face reached air:
        //   1. only a genuine roster departure may start an exit
        //   2. a reassignment cuts instantly, no hold and no fade
        //   3. any repoint cancels a running exit immediately
        //   4. an exit can never outlive its duration
        //
        // Presence in the *layout* — this frame's `desired` — cancels a
        // hold or a running exit immediately (3). `departed` is
        // roster-absence state, not a one-shot event, so a participant can
        // be in both `desired` and `departed` at once: repointed back into
        // a slot while still absent from the meeting roster. Presence in
        // `desired` must cancel a running exit regardless. (Invariant 2 is
        // unaffected: a reassigned participant is by definition never back
        // in `desired`.)
        //
        // Absence from `desired` alone must NOT erase a tile. A one-frame
        // blip, a pending departure and a pending reassignment are
        // indistinguishable at the moment they start — all three are
        // retained identically, motion untouched, until this tile's own hold
        // clock runs out. Erasing on absence alone was a regression caught in
        // review: it rebuilt the Motion from scratch on every dropped frame,
        // snapping a mid-flight tile to its target the instant `desired` next
        // included it again.
        bool departure_pending = false;
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
                const bool left_roster =
                    std::find(departed.begin(), departed.end(), it->first) != departed.end();
                const bool hold_expired =
                    (now_ns - it->second.held_since_ns) >= kSettleNs;
                if (!hold_expired) {
                    // Indistinguishable, at this point, from a one-frame blip
                    // that is about to return: retained untouched either way.
                    // Only a tile that has genuinely left the ROSTER holds the
                    // wall still while we wait (rule 3) — a reassignment is
                    // retained for motion continuity but the wall reflows
                    // around it at once.
                    if (left_roster) departure_pending = true;
                    ++it;
                    continue;
                }
                if (!left_roster) {
                    // ERASED, not merely withheld from the output. A tile kept
                    // here would be invisible — nothing emits a reassigned
                    // participant — right up until that participant later
                    // leaves the meeting for real, at which point `departed`
                    // names it and it begins a full-opacity exit from a frame
                    // recorded before the repoint: the wrong face on air,
                    // which is exactly what src/zoom-tile-slot.h exists to
                    // prevent. Every other test in the suite passes with the
                    // tile retained instead; one test pins this shape.
                    it = m_tiles.erase(it);      // (2) reassignment: instant
                    continue;
                }
                // The hold ran out and the participant really is gone: begin
                // the exit (1).
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

        std::vector<AnimatedTile> out;
        // Upper bound: every desired tile, plus every tracked tile the loop
        // below might append (exiting, or held mid-departure).
        out.reserve(desired.size() + m_tiles.size());

        // Held and exiting tiles are emitted before live tiles from
        // `desired` — see the ordering contract on advance().
        for (const auto &entry : m_tiles) {
            if (entry.second.exiting) {
                // Fading. Deliberately not gated on presence in `desired`:
                // that invariant — a tile the loop below places back in
                // `desired` always has `exiting == false` by construction,
                // because the branch above already forced it false — is
                // exactly what must hold for this to never double-emit a
                // repointed participant.
                TileRect r;
                r.x = entry.second.x.position; r.y = entry.second.y.position;
                r.width = entry.second.w.position; r.height = entry.second.h.position;
                out.push_back(AnimatedTile{entry.first, r, entry.second.alpha, false});
                continue;
            }

            const bool in_desired =
                std::find(incoming.begin(), incoming.end(), entry.first) != incoming.end();
            if (in_desired) continue;

            // Retained but not yet exiting: a one-frame blip, a pending
            // reassignment, or a pending departure. Only a genuine, still
            // pending departure is SHOWN here, frozen, until its exit begins.
            // Showing a reassignment on screen even briefly is itself an
            // invariant-2 violation, just delayed rather than avoided.
            const bool left_roster =
                std::find(departed.begin(), departed.end(), entry.first) != departed.end();
            if (!left_roster) continue;

            TileRect r;
            r.x = entry.second.x.position; r.y = entry.second.y.position;
            r.width = entry.second.w.position; r.height = entry.second.h.position;
            out.push_back(AnimatedTile{entry.first, r, 1.0, true});
        }

        for (const auto &d : desired) {
            auto it = m_tiles.find(d.participant_id);
            if (it == m_tiles.end()) {
                // A participant the wall has never carried. While a departure
                // is pending it is not drawn at all (rule 2) — the wall is
                // still showing the layout the departing tile belongs to, and
                // putting a tile from the new grid into it is the mixed state
                // this design exists to remove. It appears when the departure
                // resolves, which is bounded at kSettleNs from that
                // departure, never from its own arrival.
                if (departure_pending) continue;

                // First sight: start at the target rather than flying in
                // from the origin, and ENTER — alpha 0 to 1 over the same
                // duration, in place. See Motion::entering.
                Motion m;
                m.x = {d.rect.x, 0.0};
                m.y = {d.rect.y, 0.0};
                m.w = {d.rect.width, 0.0};
                m.h = {d.rect.height, 0.0};
                m.target = d.rect;
                m.entering = true;
                m.entered_ns = now_ns;
                it = m_tiles.emplace(d.participant_id, m).first;
            }

            Motion &m = it->second;

            // Adopt a new target unless a departure is pending. While one is,
            // the incoming rects describe a layout the wall has not adopted;
            // every tile must stay exactly where it is so a departure that
            // reverts inside the window produces no motion at all (rule 4).
            if (!departure_pending)
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

            out.push_back(AnimatedTile{d.participant_id, r, entry_alpha(m, now_ns, settings),
                                       at_rest});
        }

        return out;
    }

    // Read-only: which participant ids currently have tracked motion state
    // (in flight, held, entering, or exiting). The caller uses this, once per
    // frame in the same pass the layout is solved and advance() is called, to
    // compute `departed` (roster-absence for every id this animator is still
    // carrying) without this class needing to know anything about the roster
    // itself. Empty right after construction, and cleared along with
    // everything else by the disabled bypass in advance() above.
    std::vector<uint32_t> tracked_ids() const
    {
        std::vector<uint32_t> ids;
        ids.reserve(m_tiles.size());
        for (const auto &entry : m_tiles) ids.push_back(entry.first);
        return ids;
    }

    // True when the wall is showing the layout it has committed to and
    // nothing is moving, entering or leaving: this is what lets the caller
    // draw one whole-grid snap_tile_grid_even() instead of the animator's own
    // per-tile rects. It is a STRICTER condition than "every AnimatedTile
    // advance() just returned reports at_rest":
    //
    //   - A held tile is frozen, so it reports at_rest, while the survivors
    //     also report at_rest sitting on their OLD targets — and a fresh
    //     solve already describes the new, smaller grid, because feed-list
    //     resizing is not gated on this animator at all. Drawing the fresh
    //     solve during that window shows the wrong size.
    //   - An ENTERING tile is at its final slot and perfectly still, so it
    //     reports at_rest too, but it is part-way through its fade. The
    //     whole-grid branch draws through the snapped blit, which has no
    //     alpha, so reporting settled() here would silently drop every entry
    //     fade — the tile would pop in at full opacity. This is the only
    //     reason the geometry-focused checks below are not sufficient on
    //     their own.
    bool settled() const
    {
        for (const auto &entry : m_tiles) {
            const Motion &m = entry.second;
            if (m.held || m.exiting || m.entering) return false;
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
        double   alpha           = 1.0;   // the EXIT fade only
        bool     exiting         = false;
        uint64_t exit_started_ns = 0;
        // The hold (absent from `desired`, not yet exiting) has its own
        // clock, stamped the first time the absence is observed.
        bool     held            = false;
        uint64_t held_since_ns   = 0;
        // Entering: alpha 0 -> 1 over duration_seconds, at the tile's final
        // slot, per the design's Lifecycle section ("Entering tiles fade in
        // at their final position rather than flying in, so tiles never cross
        // one another"). This is not decoration. A join reflows the wall
        // immediately, so on the frame a newcomer appears it is at its slot
        // in the NEW grid while every other tile is still at its OLD one and
        // only just starting to move — measured at up to 500080 px^2 of
        // overlap on the first frame, decaying to zero as the reflow runs.
        // The fade is what makes that transient invisible: alpha is ~0
        // exactly when the overlap peaks and reaches 1 only once the
        // incumbents have moved away.
        bool     entering        = false;
        uint64_t entered_ns      = 0;
    };

    // The entering tile's alpha this frame, and the place the entering state
    // is retired. Mirrors spring_advance()'s treatment of a zero duration:
    // with no time to travel in there is no meaningful fade, so it arrives
    // fully opaque at once rather than dividing by zero.
    static double entry_alpha(Motion &m, uint64_t now_ns,
                              const AnimationSettings &settings)
    {
        if (!m.entering) return 1.0;
        if (settings.duration_seconds <= 0.0) {
            m.entering = false;
            return 1.0;
        }
        const double elapsed =
            static_cast<double>(now_ns - m.entered_ns) / 1e9;
        if (elapsed >= settings.duration_seconds) {
            m.entering = false;
            return 1.0;
        }
        return elapsed / settings.duration_seconds;
    }

    static std::vector<uint32_t> ids_of(const std::vector<DesiredTile> &d)
    {
        std::vector<uint32_t> ids;
        ids.reserve(d.size());
        for (const auto &t : d) ids.push_back(t.participant_id);
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    // A departure must hold this long before the wall reacts. The roster is
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
};
