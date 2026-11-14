#pragma once

// When the ENGINE rebuilds its screen-share renderer. Shared by the Windows
// engine (engine/src/engine-share.cpp) and the macOS engine
// (engine/src/main-macos.mm) and pinned by tests/share-output-policy-test.cpp.
//
// Both engines short-circuit a subscribe for the share source they already
// hold a renderer for -- right for the ordinary case, where the plugin
// registering a second share output must not tear down a live share. But the
// plugin's stale recovery reaches the engine as exactly that subscribe, so
// before 2026-11-14 a share that Zoom had stopped delivering (a paused
// stream, `onSubscribedUserDataOff`, a renderer that never produced a frame)
// could not be recovered by anything short of the share ending: the plugin
// retried on a backoff, and every retry was a no-op. This is the share half of
// the "force-resubscribe for Zoom-paused streams" gap.
//
// The rule: a subscribe (or a share Resume) rebuilds the renderer when it has
// delivered nothing for kShareRendererIdleRebuildNs -- measured from its last
// frame, or from its creation if it never delivered one. A share that is
// delivering, even at 1 fps on a static slide, is never touched.
//
// The threshold sits below the plugin's kShareStaleVideoNs
// (src/share-output-policy.h) so the plugin's recovery always lands on a
// rebuildable renderer. If a fully static share turns out to deliver nothing
// at all for long stretches, the cost is bounded: one rebuild per plugin
// recovery attempt, and a fresh subscription re-delivers the current picture.

#include <cstdint>

inline constexpr uint64_t kShareRendererIdleRebuildNs = 8'000'000'000ULL;

inline bool share_renderer_needs_rebuild(bool has_renderer,
                                         uint64_t renderer_created_ns,
                                         uint64_t last_frame_ns,
                                         uint64_t now_ns)
{
    if (!has_renderer)
        return false;
    const uint64_t since =
        last_frame_ns > renderer_created_ns ? last_frame_ns : renderer_created_ns;
    if (since >= now_ns)
        return false;
    return now_ns - since >= kShareRendererIdleRebuildNs;
}
