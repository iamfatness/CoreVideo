#pragma once

// Monotonic SHM region generations, keyed by region base name.
//
// WHY THIS EXISTS (read before moving the counter back into a target struct)
//
// A Windows named section cannot be recreated at a larger size while any
// process still maps it: CreateFileMappingA hands back the existing, smaller
// section and the following MapViewOfFile fails with ERROR_ACCESS_DENIED (5).
// The fix for that is shm_region_name() in engine-ipc.h: from generation 2 on,
// the generation is part of the region NAME, so a resize lands on a name that
// nothing can be holding. Generations 0 and 1 map to the legacy unsuffixed
// name for compatibility with engines that never resize and with plugin
// binaries that predate suffixed names.
//
// That mechanism is only as good as the counter feeding it, and the counter
// was a member of the per-source target struct (SourceTarget in
// engine-video.h, ShareTarget in engine-share.h, AudioTarget in
// engine-audio.h). Every one of those structs is destroyed and rebuilt on a
// re-subscribe:
//
//   * video  — EngineVideo::subscribe() calls unsubscribe_locked(), which
//              reaches ParticipantSubscription::remove_source() and erases the
//              target; the replacement is a fresh SourceTarget;
//   * share  — EngineShare::unsubscribe() erases the target;
//   * audio  — EngineAudio::remove() erases the target, and main.cpp calls it
//              on every Unsubscribe and on every video-only Subscribe.
//
// So the counter restarted at 0 and the next create asked for generation 1 —
// the legacy unsuffixed name — every single time. The resize was permanently
// exposed to a race with whatever mapping the plugin still held of that name,
// which is routine (not rare) on the Active Speaker source: it re-subscribes on
// every speaker change. Three production incidents have come out of this.
//
// The counter therefore does not live in anything a re-subscribe can destroy.
// It lives here, above the subscriptions, keyed by the region base name and
// scoped to the engine process. A re-created target picks up where the
// destroyed one left off, so a resize always lands on _g2, _g3, _g4 … and the
// race is unreachable by construction rather than by timing.
//
// KEYED BY BASE NAME, NOT BY UUID. The key has to be the thing that decides
// the OS object name, and video and share deliberately share one:
// EngineVideo and EngineShare both name their regions IPC_SHM_PREFIX + uuid
// (main.cpp routes a uuid to one or the other by the subscribe's "mode", and a
// source can be switched between them), so they must draw from ONE counter or
// they could hand two live regions the same name. Audio names its regions
// IPC_SHM_PREFIX + uuid + "_audio", so it gets its own counter for free —
// no special case, just a different key.
//
// INVARIANT: for a given base name the issued generation only ever increases
// within an engine process. Never decrement it, never clear an entry, never
// "recycle" one for a uuid that went away — two regions with the same name
// alive at once is the entire defect. Entries are consequently never removed;
// the table is bounded by the number of distinct region names a session ever
// sees (source UUIDs are stable per OBS source, so this is dozens, not
// thousands) at roughly a string plus four bytes each.
//
// WRAPAROUND: a uint32_t counter bumped once per (re)subscribe cannot
// realistically wrap in a session — it needs 4.29 billion re-creations of the
// same source. If it ever did, next() SATURATES at max_generation instead of
// wrapping: it keeps returning the ceiling, so that one region reverts to
// today's behaviour (repeated creates under one name, exposed to the resize
// race) rather than silently reusing a low generation whose name another
// mapping might still hold. Degrade, do not collide.

#include "engine-ipc.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// The name and generation the next creation of a region must use. `name` goes
// straight to shm_region_create(); `gen` is what the frame/audio event
// publishes as "shm_gen" so the read side knows to reopen.
struct ShmRegionAllocation {
    std::string name;
    uint32_t    gen = 0;
};

// Generation counters for SHM region base names. Safe to call from any thread:
// the video, share, and audio paths run on separate Zoom SDK callback threads
// and each holds only its own target lock, so this needs its own.
class ShmGenerationTable {
public:
    // max_generation is the saturation ceiling; the default is the real one and
    // tests override it to reach saturation without four billion calls.
    // UINT32_MAX rather than std::numeric_limits: engine-ipc.h pulls in
    // <windows.h>, whose max() macro eats the ::max() call.
    explicit ShmGenerationTable(uint32_t max_generation = UINT32_MAX)
        : m_max_generation(max_generation == 0 ? 1u : max_generation)
    {
    }

    // Issues the next generation for `base_name`. Strictly increasing per name
    // until it saturates at max_generation. The first call for a name returns 1
    // (the legacy unsuffixed region name), so a source that never resizes stays
    // wire-compatible with plugin builds that predate suffixed names.
    uint32_t next(const std::string &base_name)
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        uint32_t &issued = m_issued[base_name];
        if (issued >= m_max_generation) return m_max_generation;
        return ++issued;
    }

    // The most recent generation issued for `base_name`, or 0 if this process
    // has never created a region under it. Diagnostics only — the failure path
    // logs it to say which name a failed create actually attempted.
    uint32_t issued(const std::string &base_name) const
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        const auto it = m_issued.find(base_name);
        return it == m_issued.end() ? 0u : it->second;
    }

    size_t tracked_names() const
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_issued.size();
    }

private:
    mutable std::mutex m_mtx;
    std::unordered_map<std::string, uint32_t> m_issued;
    uint32_t m_max_generation;
};

// The whole decision in one place: bump the generation for this base name and
// derive the region name from it. Every engine ensure_shm() goes through this
// so none of them can reintroduce a per-target counter by accident.
inline ShmRegionAllocation shm_next_region(ShmGenerationTable &table,
                                           const std::string &base_name)
{
    ShmRegionAllocation allocation;
    allocation.gen  = table.next(base_name);
    allocation.name = shm_region_name(base_name, allocation.gen);
    return allocation;
}

// The engine process's table. One per process by design — see "KEYED BY BASE
// NAME" above: the video and share paths must not each own a copy.
inline ShmGenerationTable &shm_generations()
{
    static ShmGenerationTable table;
    return table;
}
