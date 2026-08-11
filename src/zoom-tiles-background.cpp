#include "zoom-tiles-background.h"

#include <obs-module.h>

namespace {

// Retires a selection: drops the showing hold, breaks the parent link, then
// releases both references. Every line reaches into libobs, so this must only
// ever be called with TilesBackground::m_mutex released — see the thread-model
// note on the class.
//
// Consumes `weak`.
void detach_background(obs_source_t *parent, obs_weak_source_t *weak)
{
    if (!weak) return;
    obs_source_t *prev = obs_weak_source_get_source(weak);
    if (prev) {
        // Order matters: stop holding it visible, then break the parent link,
        // then release our strong reference.
        obs_source_dec_showing(prev);
        if (parent) obs_source_remove_active_child(parent, prev);
        obs_source_release(prev);
    }
    obs_weak_source_release(weak);
}

}  // namespace

void TilesBackground::clear(obs_source_t *parent)
{
    obs_weak_source_t *weak = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        weak = m_weak;
        m_weak = nullptr;
        m_name.clear();
    }
    detach_background(parent, weak);
}

TilesBackgroundResult TilesBackground::set_source(obs_source_t *parent,
                                                  const char *name)
{
    if (!name || !*name) {
        clear(parent);
        return TilesBackgroundResult::Applied;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Both tests on m_weak are load-bearing, and for different reasons.
        // Non-null: a name that failed to resolve last time leaves no weak
        // reference, and a bare name compare would turn every retry into a
        // no-op. Not expired: the operator's natural recovery after deleting
        // the background source is to recreate it under the same name and
        // pick it again, and without the expiry test that re-pick would match
        // the dead reference's name and short-circuit forever — the wall stuck
        // on the colour with no way out but a rename or a restart.
        if (m_weak && !obs_weak_source_expired(m_weak) && m_name == name)
            return TilesBackgroundResult::Applied;
    }

    obs_source_t *next = obs_get_source_by_name(name);
    if (!next) {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] Tiles background source not found: %s", name);
        return TilesBackgroundResult::NotFound;
    }

    // Already holding exactly this source under a different name — i.e. the
    // operator renamed it. Registering it as an active child a second time
    // would add a second set of activate refs that only one
    // remove_active_child ever balances, leaving it stuck showing forever.
    //
    // Gated on the reference still being live: references_source is a raw
    // pointer compare (libobs/obs-source.c:922-925), and an expired weak
    // reference's pointer is dangling, so a recreated source that the
    // allocator happened to place at the freed address would compare equal and
    // be recorded as selected while nothing was actually held.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!obs_weak_source_expired(m_weak) &&
            obs_weak_source_references_source(m_weak, next)) {
            m_name = name;
            obs_source_release(next);
            return TilesBackgroundResult::Applied;
        }
    }

    // Register the parent/child link FIRST: this is the cycle check, and a
    // cycle here would be an infinite render recursion, not a cosmetic bug.
    // Done before the previous selection is retired so a refused choice leaves
    // a working background working.
    if (parent && !obs_source_add_active_child(parent, next)) {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] Tiles background refused (would render itself): %s",
             name);
        obs_source_release(next);
        return TilesBackgroundResult::Refused;
    }

    // A Media or Browser source that is in no active scene does not play.
    // Hold it showing for as long as we reference it.
    obs_source_inc_showing(next);
    obs_weak_source_t *weak = obs_source_get_weak_source(next);

    obs_weak_source_t *prev_weak = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        prev_weak = m_weak;
        m_weak = weak;
        m_name = name;
    }
    obs_source_release(next);
    // Outside the lock, and after the swap: the graphics thread never sees a
    // half-changed selection, and the old source is let go without the lock
    // held across dec_showing/remove_active_child.
    detach_background(parent, prev_weak);

    blog(LOG_INFO, "[obs-zoom-plugin] Tiles background source: %s", name);
    return TilesBackgroundResult::Applied;
}

void TilesBackground::render(uint32_t canvas_w, uint32_t canvas_h)
{
    if (canvas_w == 0 || canvas_h == 0) return;

    obs_source_t *src = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_weak) return;
        src = obs_weak_source_get_source(m_weak);
    }
    if (!src) return;  // deleted since selection; fall back to the colour

    const uint32_t sw = obs_source_get_width(src);
    const uint32_t sh = obs_source_get_height(src);
    if (sw == 0 || sh == 0) { obs_source_release(src); return; }

    // Stretch to fill the canvas. Fit modes are explicitly out of scope.
    gs_matrix_push();
    gs_matrix_scale3f(static_cast<float>(canvas_w) / static_cast<float>(sw),
                      static_cast<float>(canvas_h) / static_cast<float>(sh), 1.0f);
    obs_source_video_render(src);
    gs_matrix_pop();

    obs_source_release(src);
}

void TilesBackground::enum_active(obs_source_t *parent,
                                  obs_source_enum_proc_t enum_callback,
                                  void *param)
{
    if (!enum_callback) return;

    obs_source_t *src = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_weak) return;
        src = obs_weak_source_get_source(m_weak);
    }
    if (!src) return;

    // The strong reference is held across the callback — libobs's tree walkers
    // recurse into the child from here (enum_source_full_tree_callback), so it
    // has to stay alive for the whole call, and the callback must not run
    // under m_mutex.
    enum_callback(parent, src, param);
    obs_source_release(src);
}

std::string TilesBackground::name() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_name;
}
