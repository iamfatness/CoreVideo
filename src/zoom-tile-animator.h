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

        // ── There is no settle window ────────────────────────────────────────
        //
        // Every set change commits immediately: on every frame, every tile in
        // `desired` targets that frame's rect. This is the whole reason the
        // defect class that dominated this file's history is now
        // unrepresentable rather than merely tested for.
        //
        // That class was always the same shape — two tiles on DIFFERENT grid
        // generations at the same time — and a settle window is what created
        // it, by holding some tiles on targets from an older solve while the
        // rest moved on. Five variants were found and individually fixed
        // (a withheld joiner, a frozen joiner over a live face, a yielding
        // joiner that blanked, a joiner released against a stale set, and
        // finally a tile that was absent when the wall committed and so kept
        // a target from an older generation entirely). Each fix moved the
        // damage. With targets refreshed unconditionally there is no second
        // generation for anything to be on: any two tiles at rest are at
        // their slots in the same solve, which are disjoint by construction.
        //
        // The window existed for one reason — to stop a roster blip producing
        // a full exit animation followed by a full entry — and the render
        // path does not draw exits at all (see tiles_source_render(): held
        // and exiting tiles have no entry in `feeds` and no defined pixel
        // source). It was protecting against something that cannot happen.
        //
        // KNOWN CONSEQUENCE, accepted deliberately: a roster blip now reflows
        // the wall out and back rather than absorbing it. Because the springs
        // carry velocity through a retarget, the wall turns around from
        // wherever it had got to instead of stopping and restarting, so a
        // short blip reads as a wobble rather than a pop. A blipped
        // participant is erased and re-created, and so fades back in over
        // duration_seconds rather than snapping back — the entry fade covers
        // the discontinuity that the old per-tile retention used to cover.
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
        // A tile absent from `desired` resolves on the frame it goes absent —
        // there is nothing left to wait for, so the two cases separate at
        // once on `departed` alone.
        for (auto it = m_tiles.begin(); it != m_tiles.end();) {
            const bool in_desired =
                std::find(incoming.begin(), incoming.end(), it->first) != incoming.end();
            if (in_desired) {
                it->second.exiting = false;      // (3) back in the layout
                it->second.alpha = 1.0;
                ++it;
                continue;
            }

            if (!it->second.exiting) {
                const bool left_roster =
                    std::find(departed.begin(), departed.end(), it->first) != departed.end();
                if (!left_roster) {
                    // ERASED, not merely withheld from the output. A tile kept
                    // here would be invisible — nothing emits a reassigned
                    // participant — right up until that participant later
                    // leaves the meeting for real, at which point `departed`
                    // names it and it begins a full-opacity exit from a frame
                    // recorded before the repoint: the wrong face on air,
                    // which is exactly what src/zoom-tile-slot.h exists to
                    // prevent. One test pins this shape.
                    it = m_tiles.erase(it);      // (2) reassignment: instant
                    continue;
                }
                // Genuinely gone from the roster: begin the exit (1).
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

            // Nothing else can be here. The lifecycle loop above resolves
            // every tile absent from `desired` on the frame it goes absent:
            // a reassignment is erased and a departure starts exiting, so a
            // tracked tile is either in `desired` (emitted by the loop below)
            // or exiting (emitted above). There is no third, held state, and
            // that is what makes it impossible for a tile to be drawn from a
            // grid generation the rest of the wall has left.
        }

        for (const auto &d : desired) {
            auto it = m_tiles.find(d.participant_id);
            if (it == m_tiles.end()) {
                // First sight: start at the target rather than flying in
                // from the origin, and ENTER — alpha 0 to 1 over the same
                // duration, in place. See Motion::entering. Nothing is ever
                // withheld: a tile in `desired` is drawn on the frame it
                // appears, at its slot in the layout being solved right now.
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

            // Unconditional, every frame. This one line is what makes the
            // grid-generation defect class unrepresentable: no tile can ever
            // hold a target from an older solve, so no two tiles can be on
            // different grids at once. Do not reintroduce a condition here
            // without re-reading the header comment above — five separate
            // defects came from exactly that.
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
    //   - An EXITING tile is not in the layout at all, so a fresh solve
    //     describes a wall it is not part of.
    //   - An ENTERING tile is at its final slot and perfectly still, so it
    //     reports at_rest, but it is part-way through its fade. The
    //     whole-grid branch draws through the snapped blit, which has no
    //     alpha, so reporting settled() here would silently drop every entry
    //     fade — the tile would pop in at full opacity. This is the only
    //     reason the geometry-focused checks below are not sufficient on
    //     their own.
    bool settled() const
    {
        for (const auto &entry : m_tiles) {
            const Motion &m = entry.second;
            if (m.exiting || m.entering) return false;
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
