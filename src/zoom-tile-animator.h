// src/zoom-tile-animator.h
#pragma once

#include "tile-motion.h"
#include "zoom-tile-grid.h"

#include <cmath>
#include <cstdint>
#include <iterator>
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
    std::vector<AnimatedTile> advance(uint64_t now_ns,
                                      const std::vector<DesiredTile> &desired,
                                      const AnimationSettings &settings)
    {
        // Disabled is a bypass, not a fast setting: no state is touched and the
        // desired layout is emitted verbatim, so the renderer takes exactly the
        // path it took before this feature existed.
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

        std::vector<AnimatedTile> out;
        out.reserve(desired.size());
        for (const auto &d : desired) {
            auto it = m_tiles.find(d.participant_id);
            if (it == m_tiles.end()) {
                // First sight of this participant: start at the target rather
                // than flying in from the origin.
                Motion m;
                m.x = {d.rect.x, 0.0};
                m.y = {d.rect.y, 0.0};
                m.w = {d.rect.width, 0.0};
                m.h = {d.rect.height, 0.0};
                it = m_tiles.emplace(d.participant_id, m).first;
            }

            Motion &m = it->second;
            spring_advance(m.x, d.rect.x,      settings.duration_seconds, dt);
            spring_advance(m.y, d.rect.y,      settings.duration_seconds, dt);
            spring_advance(m.w, d.rect.width,  settings.duration_seconds, dt);
            spring_advance(m.h, d.rect.height, settings.duration_seconds, dt);

            TileRect r;
            r.x = m.x.position; r.y = m.y.position;
            r.width = m.w.position; r.height = m.h.position;

            constexpr double kRestEpsilon = 0.05;  // sub-tenth-pixel
            const bool at_rest =
                std::fabs(r.x - d.rect.x) < kRestEpsilon &&
                std::fabs(r.y - d.rect.y) < kRestEpsilon &&
                std::fabs(r.width - d.rect.width) < kRestEpsilon &&
                std::fabs(r.height - d.rect.height) < kRestEpsilon;

            out.push_back(AnimatedTile{d.participant_id, r, 1.0, at_rest});
        }

        // Forget participants no longer desired. Task 4 replaces this with the
        // exit lifecycle.
        for (auto it = m_tiles.begin(); it != m_tiles.end();) {
            bool still_wanted = false;
            for (const auto &d : desired)
                if (d.participant_id == it->first) { still_wanted = true; break; }
            it = still_wanted ? std::next(it) : m_tiles.erase(it);
        }

        return out;
    }

private:
    struct Motion { Spring1D x, y, w, h; };
    std::map<uint32_t, Motion> m_tiles;
    uint64_t m_last_ns  = 0;
    bool     m_has_last = false;
};
