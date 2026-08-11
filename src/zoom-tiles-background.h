#pragma once

#include <obs.h>

#include <mutex>
#include <string>

// A background layer for the tiles wall: an optional reference to another OBS
// source, rendered behind the tiles and scaled to the canvas.
//
// Holds a WEAK reference so selecting a source never keeps it alive after the
// operator deletes it; a deleted background silently falls back to the
// background colour rather than erroring.
//
// Thread model. The selection is written from the settings thread
// (tiles_source_update / tiles_source_load) and read from the OBS graphics
// thread every frame (tiles_source_render), so it needs a lock of its own —
// NOT the tiles source's ctx->mutex, which the graphics thread already takes
// and which must never be held across a call into libobs. m_mutex here is a
// leaf: the only thing ever done under it is an atomic weak-ref upgrade
// (obs_weak_source_get_source is a lock-free CAS on the refcount) or a pointer
// compare. Every libobs call that can run source code, fire a signal or walk
// the source tree — add/remove_active_child, inc/dec_showing, video_render,
// release — happens with m_mutex released, so this class can never be part of
// a lock cycle with libobs's own locks.
class TilesBackground {
public:
    // Selects `name` as the background, replacing any previous selection.
    // An empty or null name clears it.
    //
    // Returns false and leaves the previous selection intact when the choice
    // would create a render cycle — selecting the tiles source itself, or a
    // scene that contains it. That case is not merely wrong, it is an infinite
    // recursion that would crash OBS, so it is refused rather than attempted.
    // Also returns false when no source by that name exists (yet): the caller
    // may retry with the same name, which is how a scene collection whose
    // sources load after this one still finds its background.
    bool set_source(obs_source_t *parent, const char *name);

    // Draws the background filling canvas_w x canvas_h. No-op when nothing is
    // selected or the selected source has since been deleted. Graphics thread.
    void render(uint32_t canvas_w, uint32_t canvas_h);

    // Releases the reference and the showing/active-child holds. Safe to call
    // when nothing is selected.
    void clear(obs_source_t *parent);

    // Reports the selected source to libobs's source-tree walkers. Wired to
    // obs_source_info::enum_active_sources — without it obs_source_add_active_child
    // cannot see through a tiles source at all (obs_source_enum_full_tree
    // returns early for a source with no enum_active_sources,
    // libobs/obs-source.c:4663), so a cycle that runs through two tiles sources
    // would be accepted and then crash the render thread.
    void enum_active(obs_source_t *parent, obs_source_enum_proc_t enum_callback,
                     void *param);

    // By value, not by reference: the string is mutable under m_mutex and a
    // reference into it would be read unlocked by the caller.
    std::string name() const;

private:
    mutable std::mutex m_mutex;  // guards m_weak and m_name; see the note above
    obs_weak_source_t *m_weak = nullptr;
    std::string        m_name;
};
