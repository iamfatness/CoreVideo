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
            // Cleared with everything else, so the NEXT enabled frame is an
            // adoption again: the wall the operator sees when they tick the
            // checkbox is the wall that stays on screen.
            m_has_run_enabled_frame = false;
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

        // ADOPTION vs ARRIVAL. These are two different events and the animator
        // has to tell them apart, because only one of them should fade:
        //
        //   - Adoption: this animator's FIRST enabled frame. Whatever the
        //     layout holds is a wall that already exists — a scene load, or an
        //     operator ticking the Animate checkbox mid-show (the disabled
        //     bypass discards all state, so the next enabled frame starts from
        //     nothing). Fading here would blank a live show for the whole
        //     duration over a checkbox.
        //   - Arrival: a participant appearing on any LATER frame. That is a
        //     join, and it fades, because it is drawn at its slot in the new
        //     grid while every incumbent is still at its old one.
        //
        // Latched unconditionally, and deliberately NOT derived from `m_tiles`
        // being empty. That test conflates "the animator has not run yet" with
        // "the wall happens to have nobody on it", so an animator left running
        // on an empty wall — OBS open before the meeting starts — treated the
        // first participant to ever appear as an adoption and popped them on
        // at full opacity. An empty container standing in for a state is the
        // recurring bug on this feature; the state is its own flag.
        const bool adopting_existing_wall = !m_has_run_enabled_frame;
        m_has_run_enabled_frame = true;

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

        std::vector<uint32_t> emitted;
        emitted.reserve(desired.size());

        for (const auto &d : desired) {
            // A duplicate id would drive ONE Motion twice per frame, from two
            // targets, so it would oscillate and never settle — and a tile
            // that never settles never returns to the pixel-exact blit. The
            // settings path de-duplicates (resolve_tile_assignments()), so
            // this is unreachable from there; it is here because a map-keyed
            // API that silently mis-handles a duplicate key is a trap, and
            // because the render path's own first-match lookup would hand
            // both feeds the same rect anyway. First entry wins.
            if (std::find(emitted.begin(), emitted.end(), d.participant_id) !=
                emitted.end())
                continue;
            emitted.push_back(d.participant_id);

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
                m.entering = !adopting_existing_wall;
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
    // Has this animator processed an enabled frame yet in this enabled run?
    // Latched unconditionally in advance(), reset by the disabled bypass. See
    // adopting_existing_wall there for why this is its own flag and not a test
    // on m_tiles being empty.
    bool     m_has_run_enabled_frame = false;
};

// ── Caller-side helpers ──────────────────────────────────────────────────────
//
// These are the two ends of advance()'s contract — what produces its
// `departed` argument, and what consumes its output — kept here, pure and
// testable, rather than inline in the render path where neither could be.

// One entry per `desired` index: the tile advance() emitted for that
// participant, or nullptr if it emitted none.
//
// Index-aligned with `desired` BY CONSTRUCTION, and that is the point. The
// render path used to re-read each feed's participant id from live slot state
// to do this lookup, having already read it once to build `desired` — and
// `plan_feeds_locked()` can repoint a slot between the two reads, because the
// render path copies shared_ptrs and drops the mutex before drawing. The
// lookup then either missed (that slot lost its tile, border and glow for a
// frame) or collided with another feed (a retired feed's grey placeholder
// painted over a live tile). Taking `desired` and nothing else means one read
// serves both, and re-introducing the second read would require changing this
// signature.
inline std::vector<const AnimatedTile *> resolve_animated_for_desired(
    const std::vector<DesiredTile> &desired,
    const std::vector<AnimatedTile> &animated)
{
    std::vector<const AnimatedTile *> out;
    out.reserve(desired.size());
    for (const auto &d : desired) {
        const AnimatedTile *found = nullptr;
        for (const auto &t : animated) {
            if (t.participant_id == d.participant_id) { found = &t; break; }
        }
        out.push_back(found);
    }
    return out;
}

// Does this frame's layout disagree with what the animator is tracking, in
// EITHER direction? The roster is only worth consulting on such a frame: on
// every other one the answer cannot change anything, and the query is a
// locked deep copy of the participant list on the graphics thread.
//
// Both directions matter. Absences are what `departed` is computed from;
// arrivals are what lets a participant's roster membership be recorded while
// they are still in the meeting, which is what classify_departures() needs to
// tell a departure from a repoint later.
inline bool layout_disagrees_with_tracking(const std::vector<uint32_t> &tracked,
                                           const std::vector<uint32_t> &layout_ids)
{
    for (const uint32_t id : tracked)
        if (std::find(layout_ids.begin(), layout_ids.end(), id) == layout_ids.end())
            return true;
    for (const uint32_t id : layout_ids)
        if (std::find(tracked.begin(), tracked.end(), id) == tracked.end())
            return true;
    return false;
}

// Which tracked participants have genuinely LEFT THE MEETING — the only thing
// that may begin an exit (invariant 1).
//
// Requires positive evidence of a departure rather than absence of evidence of
// presence. "Absent from the layout and absent from the roster" is not enough:
// in Manual fill mode the roster is deliberately not consulted when choosing
// tiles (src/zoom-tile-fill.h — "an operator who cast a tile keeps it even
// while that participant's camera is off"), so an operator can cast someone
// who never joined the meeting at all. Repointing that slot away satisfies
// both conditions, and the animator would start an exit for a participant who
// never departed — invariants 1 and 2 broken by one operator action.
//
// What this does NOT close, and must not be read as closing: a roster snapshot
// that is momentarily EMPTY. Every participant on the wall is in
// `ever_in_roster` by definition, so if `roster_ids` reads empty while they are
// off the layout, every one of them classifies as departed. Forced in a probe,
// that window produced 15,832 exits for participants still in the meeting.
//
// It is unreachable only because of an invariant OUTSIDE this file:
// ZoomEngineClient::m_roster is cleared exactly twice — on "left", which is a
// genuine departure of everyone, and at the top of a "participants" rebuild
// that repopulates it before releasing m_mtx, so it is never observably empty
// mid-rebuild (src/zoom-engine-client.cpp:978 and :1094). A disconnect leaves
// it STALE rather than empty, and stale fails safe: still in the roster means
// not departed, which cuts instantly and puts nothing on air.
//
// If that ever changes — if m_roster is cleared on a disconnect, a
// meeting-end, or an engine restart path — this classifier will call the whole
// wall departed and every tile will begin an exit. Either keep that invariant
// or make this function require a non-empty snapshot.
//
// `ever_in_roster` is the evidence: ids observed in some earlier roster
// snapshot. An id that has never been seen in one cannot have departed from
// it. The failure direction is deliberate — an unrecorded genuine departure is
// treated as a repoint, which cuts instantly and puts nothing on air, whereas
// the reverse animates a face that never left.
inline std::vector<uint32_t> classify_departures(
    const std::vector<uint32_t> &tracked,
    const std::vector<uint32_t> &layout_ids,
    const std::vector<uint32_t> &roster_ids,
    const std::vector<uint32_t> &ever_in_roster)
{
    std::vector<uint32_t> departed;
    for (const uint32_t id : tracked) {
        if (std::find(layout_ids.begin(), layout_ids.end(), id) != layout_ids.end())
            continue;                                   // still on the wall
        if (std::find(roster_ids.begin(), roster_ids.end(), id) != roster_ids.end())
            continue;                                   // still in the meeting
        if (std::find(ever_in_roster.begin(), ever_in_roster.end(), id) ==
            ever_in_roster.end())
            continue;                                   // never was in the meeting
        departed.push_back(id);
    }
    return departed;
}

// The animation duration, bounded to the range the slider offers. The only
// setting on this source that was not clamped: obs_data_get_int returns an
// int64 and scene files are hand-editable, so a negative value wrapped to
// 4294967295 ms and every tile sat at alpha ~0 forever — the wall rendering as
// background only, permanently on the composite path because settled() could
// never become true. Clamped rather than dropped, on the same reading as the
// neighbouring settings: a number outside the range is a request for the
// nearest end of it.
inline uint32_t clamp_animate_duration_ms(int64_t raw)
{
    constexpr int64_t kMinMs = 100;
    constexpr int64_t kMaxMs = 1000;
    if (raw < kMinMs) return static_cast<uint32_t>(kMinMs);
    if (raw > kMaxMs) return static_cast<uint32_t>(kMaxMs);
    return static_cast<uint32_t>(raw);
}
