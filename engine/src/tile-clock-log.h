// engine/src/tile-clock-log.h
#pragma once

#include "engine-writer.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

// Emits one debug IPC message per observed frame/buffer when
// COREVIDEO_TILE_CLOCK_PROBE=1 in the engine's environment (inherited from
// OBS). The plugin relays debug messages into the OBS log, where
// tools/tile-clock-analyze.py picks them up.
// Payload format inside the message: TILECLOCK,<kind>,<feed_id>,<media_pts_us>,<arrival_ns>
// Off by default and cheap when off: the env var is read once.
inline void tile_clock_log(uint32_t feed_id, uint64_t media_pts_us,
                           uint64_t arrival_ns, const char *kind)
{
    static const bool enabled = [] {
        const char *v = std::getenv("COREVIDEO_TILE_CLOCK_PROBE");
        return v && v[0] == '1';
    }();
    if (!enabled) return;

    EngineIpc::write(std::string(R"({"cmd":"debug","stage":"tile_clock","msg":"TILECLOCK,)") +
                     kind + "," + std::to_string(feed_id) + "," +
                     std::to_string(media_pts_us) + "," +
                     std::to_string(arrival_ns) + "\"}");
}

// Monotonic arrival time in nanoseconds, engine-process clock. All feeds are
// compared against this same clock, which is all the analyzer needs.
inline uint64_t tile_clock_now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
