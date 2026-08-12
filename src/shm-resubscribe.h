#pragma once

// Release-before-resubscribe for engine SHM read mappings.
//
// This is the one rule that makes re-pointing an existing source_uuid safe, and
// it is easy to mistake for redundant cleanup and delete. It is not cleanup. It
// is a precondition of the subscribe that follows it.
//
// Why, in full:
//
// Re-subscribing the same source_uuid at a different participant makes the
// engine run unsubscribe_locked() first (EngineVideo::subscribe,
// engine/src/engine-video.cpp), which destroys that uuid's SourceTarget. The
// generation counter used to be a member of SourceTarget, so the replacement
// target restarted at generation 0 and its first ensure_shm() created
// generation 1 — the *legacy unsuffixed* region name (see shm_region_name() in
// engine-ipc.h). The generation therefore ran BACKWARDS (the "gen 2 -> 1" and
// "gen 3 -> 1" lines in the 2026-08-10 on-air log) and the engine tried to
// create a region under a name we were still mapping.
//
// That reset is fixed at the source: the counter now lives in a process-wide
// table keyed by region base name (src/shm-generation.h) and survives the
// target, so a rebuilt target moves to _g2, _g3, … instead of back to the
// legacy name. This release is still required, and is not made redundant by
// it:
//
//   * a CoreVideo plugin talks to whatever ZoomObsEngine.exe is installed
//     beside it, including builds that predate the monotonic counter;
//   * on POSIX the engine grows a region in place under one name, so the read
//     side must still be told to let go;
//   * holding a mapping we will never read again is a leak on any platform.
//
// A Windows named section cannot be recreated at a larger size while any
// process still maps it: CreateFileMappingA hands back the existing, smaller
// section and the larger MapViewOfFile fails. The engine is then left writing
// into — or failing to create — a region that does not match the size the
// plugin believes it has, and the read side renders whatever bytes are there.
// On air that showed as participants flashing bright garbage on every speaker
// change, not as a clean black gap.
//
// Dropping the mapping first leaves the name free for the engine to recreate at
// the new size. It costs nothing: the decoded frame the render path shows is a
// separate copy, and shm_read_i420_frame() reopens the region on the very next
// frame event, so even if this release loses the race with the engine's first
// create attempt the following frame self-heals.
//
// Order is not negotiable: release, THEN subscribe. Releasing after the
// subscribe has already reached the engine is the same bug with extra steps.
//
// This rule was first worked out in the Tiles path (Phase A); the tiles code
// now calls this helper through tile_feed_release_mapping() in
// src/zoom-supersource.cpp, which wraps it in the feed lock. See also
// shm_region_name() in src/engine-ipc.h.

#include "engine-ipc.h"

#include <cstdint>

// Drops a read mapping and forgets the generation it was opened against.
//
// Resetting the generation keeps the invariant "mapped_gen == 0 means nothing
// is mapped" true, which the reopen logging keys off; the reopen itself is
// forced by the null pointer (shm_mapping_stale() in engine-ipc.h), so this
// stays correct no matter which generation the engine's replacement target
// comes back with — higher, lower, or the same.
//
// Returns true when a mapping was actually dropped, so callers can log the
// release once per real occurrence instead of on every no-op call. That log
// matters: once the mapping is released, the "gen N -> M" reopen line in
// zoom-source.cpp stops firing (it is guarded on a non-null pointer), and that
// line is how the 2026-08-10 incident was diagnosed. Releasing silently would
// remove the evidence trail for the next recurrence.
//
// Caller must serialise this against the frame callback that reads `region`.
inline bool shm_release_for_resubscribe(ShmRegion &region, uint32_t &mapped_gen)
{
    const bool had_mapping = region.ptr != nullptr;
    shm_region_destroy(region);
    mapped_gen = 0;
    return had_mapping;
}
