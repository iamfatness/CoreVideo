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
        // Disabled is a bypass, not a fast setting: no state is touched and the
        // desired layout is emitted verbatim, so the renderer takes exactly the
        // path it took before this feature existed.
        if (!settings.enabled) {
            m_tiles.clear();
            m_last_ns = 0;
            m_has_last = false;
            m_committed_ids.clear();
            m_pending_ids.clear();
            m_pending_since_ns = 0;
            m_has_committed = false;
            m_has_pending = false;
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
        // that clock, the exit begins regardless of whether the settle
        // window has committed, bounding total on-air time for a departed
        // participant at kSettleNs + duration_seconds by construction,
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

            const bool left_roster =
                std::find(departed.begin(), departed.end(), it->first) != departed.end();
            if (!left_roster) {
                it = m_tiles.erase(it);          // (2) reassignment: instant, hold or not
                continue;
            }

            if (!it->second.exiting) {
                const bool committed =
                    std::find(m_committed_ids.begin(), m_committed_ids.end(),
                              it->first) != m_committed_ids.end();
                const bool hold_expired =
                    (now_ns - it->second.held_since_ns) >= kSettleNs;
                if (committed && !hold_expired) {
                    ++it;   // still within bounds: hold, unfaded (emitted below)
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

            // Reachable only for a tile still inside its own hold window
            // (see the lifecycle loop above): by construction anything not
            // genuinely departed was already erased there, and anything
            // whose hold expired was already promoted to exiting there. No
            // need to re-check `departed` here — this loop cannot reach a
            // reassigned participant.
            TileRect r;
            r.x = entry.second.x.position; r.y = entry.second.y.position;
            r.width = entry.second.w.position; r.height = entry.second.h.position;
            out.push_back(AnimatedTile{entry.first, r, 1.0, true});
        }

        for (const auto &d : desired) {
            const bool committed =
                std::find(m_committed_ids.begin(), m_committed_ids.end(),
                          d.participant_id) != m_committed_ids.end();
            if (!committed) {
                // Present but not yet adopted: hold it wherever it already is,
                // or emit it at its target if it is new, without disturbing the
                // rest of the wall.
                auto held = m_tiles.find(d.participant_id);
                if (held == m_tiles.end()) {
                    out.push_back(AnimatedTile{d.participant_id, d.rect, 1.0, true});
                } else {
                    TileRect r;
                    r.x = held->second.x.position; r.y = held->second.y.position;
                    r.width = held->second.w.position;
                    r.height = held->second.h.position;
                    out.push_back(AnimatedTile{d.participant_id, r, 1.0, true});
                }
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

            constexpr double kRestEpsilon = 0.05;  // sub-tenth-pixel
            const bool at_rest =
                std::fabs(r.x - m.target.x) < kRestEpsilon &&
                std::fabs(r.y - m.target.y) < kRestEpsilon &&
                std::fabs(r.width - m.target.width) < kRestEpsilon &&
                std::fabs(r.height - m.target.height) < kRestEpsilon;

            out.push_back(AnimatedTile{d.participant_id, r, 1.0, at_rest});
        }

        return out;
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
    };

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

    std::map<uint32_t, Motion> m_tiles;
    uint64_t m_last_ns  = 0;
    bool     m_has_last = false;

    std::vector<uint32_t> m_committed_ids;   // the set the wall is laid out for
    std::vector<uint32_t> m_pending_ids;     // a candidate set, not yet settled
    uint64_t m_pending_since_ns = 0;
    bool     m_has_committed = false;

    // Distinguishes "an empty roster is proposed" from "nothing is proposed".
    bool m_has_pending = false;
};
