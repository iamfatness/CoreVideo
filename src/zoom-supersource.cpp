#include "zoom-supersource.h"
#include "engine-ipc.h"
#include "shm-resubscribe.h"
#include "zoom-engine-client.h"
#include "zoom-tile-animator.h"
#include "zoom-tile-border.h"
#include "zoom-tile-crop.h"
#include "zoom-tile-fill.h"
#include "zoom-tile-glow.h"
#include "zoom-tile-grid.h"
#include "zoom-tile-retry.h"
#include "zoom-tile-shape.h"
#include "zoom-tile-slot.h"
#include "zoom-tile-texture.h"
#include "zoom-tiles-audio.h"
#include "zoom-tiles-background.h"
#include "zoom-tiles-effect.h"

#include <graphics/vec4.h>  // obs.h brings in vec2/vec3 but not vec4
#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// The tile shape and the wall's spacing are operator controls; their defaults
// are the constants this file used to hard-code, resolved in zoom-tile-shape.h
// and pinned bit-for-bit by tests/tile-shape-test.cpp. The operator's border is
// drawn *inside* the tile rect by the shader rather than around it, so it
// changes what a tile looks like without changing the grid the tiles were
// solved into — the parity-verified geometry in snap_tile_grid_even() is
// untouched either way.
//
// The tile aspect has three consumers: the grid solve, the cover-crop that
// decides which part of a camera frame fills the resulting rect, and
// solve_slot_crop() for the per-slot crop. Only two of them are call sites in
// this file — the cover-crop reaches it *through* solve_slot_crop(), which
// passes its dst_aspect straight to solve_cover_crop() (zoom-tile-crop.h:46).
// All three must see the SAME value or the layout and the sampling disagree
// and every tile is mis-framed, which is why tiles_source_render() reads the
// atomic once into TileGridParams and the crop then takes it from there rather
// than loading it a second time.

// Custom ratio bounds, matching the property's own range. Enforced against
// scene data as well, since obs_data_get_double will hand over anything.
static constexpr double kMinCustomAspect = 0.1;
static constexpr double kMaxCustomAspect = 10.0;
// Gutter and margin, as a percentage of canvas height. The upper bound is the
// slider's; resolve_spacing_px() bounds the resulting pixels against the canvas
// as well, for hand-edited scene files.
static constexpr double kMaxSpacingPct = 10.0;
// Border property bounds. Enforced against scene data as well as the sliders:
// obs_data_get_int returns int64 and scene files are hand-editable. These only
// bound the *setting*; clamp_border() then bounds it against the actual tile.
static constexpr int64_t kMaxBorderWidth  = 64;
static constexpr int64_t kMaxCornerRadius = 128;
// Outer glow bounds, on the same terms as the border above. The size is in
// canvas pixels rather than a percentage of canvas height — deliberately unlike
// the gutter and margin, and deliberately like the border width and corner
// radius, because a glow is a drawn effect on the tile whose weight the
// operator is choosing directly, not structural spacing. Nothing here clamps
// the glow against the gutter or the margin: overlap is allowed. See
// src/zoom-tile-glow.h.
static constexpr int64_t kMaxGlowSize      = 256;  // canvas pixels
static constexpr int64_t kMaxGlowIntensity = 100;  // percent
// The falloff's shape, as a percentage the operator can judge by eye. It maps
// linearly onto the shader's own parameter k in (1-t)^2 * (1 + k*t), where the
// useful range is k in [0, 2]:
//   0%   -> k = 0, i.e. (1-t)^2 — the curve the glow shipped with, so an
//           existing scene renders unchanged;
//   100% -> k = 2, i.e. (1-t)^2(1+2t) = 1 - smoothstep(t), flat at both ends.
// 2 is where the family stops being monotone rather than a matter of taste:
// d/dt = (1-t)[(k-2) - 3kt] is <= 0 across [0,1) exactly when k <= 2, given
// k >= 0 — the clamp's floor here. (Without that precondition the claim is
// false: k = -3 is also <= 2 but breaks the bound.) Above k = 2 the halo
// brightens just outside the tile and draws a ring. Every value in range
// leaves both the value and the slope at 0 at the outer edge, which is what
// keeps the halo from ending on a visible band.
static constexpr int64_t kMaxGlowSoftness  = 100;  // percent
// How many tile slots the wall has. Declared up here rather than beside the
// other property constants below because `tiles_source` sizes its per-slot crop
// array from it.
static constexpr std::size_t kMaxTileSlots = 9;
// Per-slot crop bound, in percent of the source width. 45 each side means left
// and right together can never exceed 90%, so kMinCropRemainder in
// zoom-tile-crop.h stays a defensive backstop for hand-edited scene files
// rather than something the sliders reach.
static constexpr int64_t kMaxSlotCropPct = 45;
// Neutral fill for the background and for tiles with no frame yet.
static constexpr uint8_t kNeutralY = 0x80;
static constexpr uint8_t kNeutralUV = 0x80;
// Canvas bounds, matching the property ranges. Enforced against scene data too:
// an out-of-range value would otherwise reach resize() as a huge allocation.
static constexpr uint32_t kMinCanvasW = 16, kMaxCanvasW = 7680;
static constexpr uint32_t kMinCanvasH = 16, kMaxCanvasH = 4320;

// Shared between zoom_supersource_register() (below) and the collection-
// loading sweep (request_audio_reconcile / zoom_supersource_set_collection_
// loading) so the two cannot drift apart.
static constexpr const char *kTilesSourceId = "corevideo_tiles_source";

static std::atomic<uint64_t> s_tiles_instance_counter{0};
static std::atomic<uint64_t> s_tile_feed_serial{0};
// True while a scene collection is being torn down or loaded (set on
// OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING *and* _CLEANUP, cleared on
// _CHANGED/_FINISHED_LOADING; see zoom_supersource_set_collection_loading and
// plugin-main.cpp's frontend_event_callback for why CLEANUP is the one that
// makes this reliable). Starts true: nothing should reconcile audio before
// the very first load this session has completed, for the identical reason a
// mid-load one must not.
//
// Two places check it, and both are load-bearing:
//   1. request_audio_reconcile, before queuing anything — because
//      obs_queue_task(OBS_TASK_UI, ...) is NOT reliably deferred and runs
//      inline for a UI-thread caller, which a load always is.
//   2. the queued task body itself, before doing any work — because a task
//      queued from the IPC reader thread (genuinely deferred) while the gate
//      was clear can still be sitting in Qt's queue when a teardown begins,
//      and ClearSceneData pumps that queue three times
//      (frontend/widgets/OBSBasic_SceneCollections.cpp:1550-1559) while its
//      sources are flagged removed but still alive and still enumerable.
//      Creating into a dying group there lands the new source in
//      ClearSceneData's orphan sweep, sets clearingFailed, and closes OBS
//      (LoadData:1147-1150).
static std::atomic<bool> s_collection_loading{true};
// The shared I420 render effect, loaded once at module load and torn down at
// module unload. See src/zoom-tiles-effect.h. Every tile is drawn through it.
static TilesEffect s_tiles_effect;
// Globally unique per decoded frame, so a scratch buffer can tell whether the
// pixels it holds are still the newest ones for its slot without any chance of
// a stale value colliding across feeds.
static std::atomic<uint64_t> s_frame_generation{0};

// One tile slot's feed. The engine publishes each subscription into its own
// shared-memory region, so a slot owns a uuid, a subscription, and a mapping.
//
// The uuid is derived from the slot index and stays fixed for the slot's
// lifetime, matching how the per-participant source reuses its own uuid across
// resubscribes. A stable uuid also keeps the engine's per-source state bounded
// by the number of slots rather than by the number of reassignments.
//
// (This used to be load-bearing for a second reason: the engine's dispatch
// matched "subscribe" as a substring before it tested "unsubscribe", so an
// unsubscribe never reached the branch that drops engine-side state for a uuid,
// and a fresh uuid per reassignment would have stranded one orphaned audio
// target per repoint. That dispatch bug is fixed — see src/engine-command.h —
// and tiles no longer register an audio target at all.)
struct TileFeed {
    const uint64_t id =
        s_tile_feed_serial.fetch_add(1, std::memory_order_relaxed) + 1;
    std::string uuid;  // fixed once constructed
    // Which participant this slot shows, and the staleness rules for frames
    // captured under a previous assignment. Lock-free, so repointing a slot
    // never has to take the mutex guarding its pixels. See zoom-tile-slot.h.
    TileSlotState slot;

    std::mutex mtx;     // guards everything below
    bool alive = true;  // cleared before teardown so in-flight callbacks bail
    ShmRegion shm{};
    // Which engine-side region generation `shm` was opened against. The engine
    // moves to a new region name on every resize, so a reader that ignores this
    // keeps a frozen frame forever (see shm_read_i420_frame in engine-ipc.h).
    uint32_t shm_gen = 0;
    std::vector<uint8_t> frame;  // I420: y_len, then U and V of y_len/4 each
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frame_epoch = 0;
    uint64_t generation = 0;
    bool has_frame = false;  // pixels present that the draw path has not taken

    // Retry budget for the silent-slot sweep (see zoom-tile-retry.h). Guarded
    // by mtx, which the sweep's scan already holds. `retry_epoch` records the
    // assignment these attempts were spent under, so repointing the slot hands
    // it a fresh budget instead of inheriting an exhausted one.
    uint64_t retry_epoch = 0;
    uint64_t last_retry_ns = 0;
    uint32_t retry_attempts = 0;
    bool retry_exhausted_logged = false;

    // Graphics-thread-only state. Never touched by the engine reader thread,
    // so deliberately outside the mutex above: video_render is the only writer
    // and the only reader, and teardown (tile_feed_retire) reaches it only
    // after entering the graphics context, which the graphics thread holds for
    // the whole of video_render. Adding a lock here would put the engine
    // reader thread and the graphics thread on the same mutex for no reason.
    gs_texture_t *tex_y = nullptr;
    gs_texture_t *tex_u = nullptr;
    gs_texture_t *tex_v = nullptr;
    uint32_t      tex_w = 0;   // luma dimensions the textures were created for
    uint32_t      tex_h = 0;
    uint64_t      uploaded_generation = 0;
};
using TileFeedPtr = std::shared_ptr<TileFeed>;

// The draw path's private copy of one slot's newest frame. Owned solely by the
// OBS graphics thread, so the texture upload runs with no lock held.
struct TileScratch {
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t generation = 0;  // 0 means "nothing to show"
    uint64_t feed_id = 0;
    uint64_t epoch = 0;
};

// Keeps roster callbacks from touching a destroyed context, mirroring the
// gate used by the CoreVideo audio sources.
struct TilesCallbackGate {
    std::mutex mtx;
    bool alive = true;
};

struct tiles_source {
    obs_source_t *source = nullptr;

    // Read by OBS on the graphics thread every frame via get_width/get_height,
    // so these are atomics rather than mutex-guarded state: the graphics thread
    // must never be able to block behind engine IPC.
    std::atomic<uint32_t> canvas_width{1920};
    std::atomic<uint32_t> canvas_height{1080};
    // Operator-chosen background fill for the wall's gutters, margins and any
    // uncovered canvas. Same atomic-not-mutex reasoning as canvas_width above.
    std::atomic<uint32_t> bg_color{0xFF808080};
    // The operator's tile border: colour (in the picker's 0xAABBGGRR byte
    // order, converted at draw time), width in canvas pixels, and whether the
    // corners are rounded plus by how much. Same atomic-not-mutex reasoning as
    // bg_color above — the graphics thread reads all four every frame.
    //
    // The radius is kept separate from the shape rather than folded to 0 when
    // Square, so switching to Square and back does not lose the operator's
    // radius; the fold happens at draw time instead.
    std::atomic<uint32_t> border_color{0xFF000000};
    std::atomic<uint32_t> border_width{0};
    std::atomic<bool>     border_rounded{false};
    std::atomic<uint32_t> corner_radius{16};
    // The operator's outer glow: colour (picker byte order, converted at draw
    // time), size in canvas pixels, and intensity as a percentage. Same
    // atomic-not-mutex reasoning as bg_color above.
    //
    // Size 0 is off and is the default, and the draw path skips the entire
    // glow pass at 0 — no technique, no blend state, no draw — so a scene saved
    // before this control existed renders byte-for-byte as it did.
    std::atomic<uint32_t> glow_color{0xFFFFFFFF};
    std::atomic<uint32_t> glow_size{0};
    std::atomic<uint32_t> glow_intensity{100};
    // How the halo falls off between the tile edge and the outer limit, as a
    // percentage. 0 is the curve the glow shipped with, so this control is
    // inert until the operator moves it. See kMaxGlowSoftness.
    std::atomic<uint32_t> glow_softness{0};
    // Layout animation: whether tile moves ease instead of snapping, and the
    // duration in milliseconds. Same atomic-not-mutex reasoning as bg_color
    // above. Off by default — see tiles_source_get_defaults.
    std::atomic<bool>     animate{false};
    std::atomic<uint32_t> animate_ms{350};
    // The wall's geometry: tile shape (width / height, already resolved from
    // the preset and the custom ratio) and the gutter and margin as
    // percentages of canvas height. Same atomic-not-mutex reasoning as
    // bg_color above — the graphics thread reads all three every frame.
    //
    // Stored resolved rather than as (preset, ratio) so the draw path never
    // has to know the preset numbering, and so the fallback for an unusable
    // custom ratio happens once per settings change instead of once per frame.
    std::atomic<double> tile_aspect{kDefaultTileAspect};
    std::atomic<double> gutter_pct{kDefaultSpacingPct};
    std::atomic<double> margin_pct{kDefaultSpacingPct};
    // Optional other OBS source drawn over the background colour and under the
    // tiles. Carries its own leaf lock (see zoom-tiles-background.h) rather
    // than living under `mutex`: the graphics thread reads it every frame, and
    // selecting a background calls into libobs, which must never happen under
    // a lock the graphics thread takes.
    TilesBackground background;

    // Serializes whole plan-and-execute cycles (update/show/hide/destroy) so
    // engine IPC happens outside `mutex` yet still in a well-defined order.
    std::mutex engine_mutex;

    std::mutex mutex;  // guards participants, feeds, fill_params and visible
    std::vector<uint32_t> participants;
    // The settings the resolver needs. Cached because the roster callback runs
    // with no obs_data_t in hand — it only knows the participants changed.
    TileFillParams fill_params;
    std::vector<TileFeedPtr> feeds;  // parallel to participants, slot for slot
    // The operator's per-slot left/right crop, in percent of the source width,
    // indexed by tile slot. Under `mutex` rather than in atomics like the
    // border settings: the draw path needs all eighteen values to belong to one
    // consistent settings pass, and it is already taking this lock every frame
    // to snapshot the feed list, so this costs nothing extra.
    std::array<std::pair<double, double>, kMaxTileSlots> slot_crop{};
    bool visible = false;
    // Distinguishes this source's slot uuids from any other tiles source.
    uint64_t instance_id = 0;
    // Names the group per-participant audio sources are created into; empty
    // means the feature is off. Guarded by `mutex` alongside fill_params so a
    // settings pass lands as one unit, matching how the crop params are
    // already handled.
    std::string audio_group;
    // Set while an audio reconcile is queued on OBS_TASK_UI or currently
    // running there; used to coalesce a burst of triggering events (a
    // talking change alone fires both an active_speaker line and a full
    // roster update) into at most one queued task. See
    // request_audio_reconcile(). Deliberately not guarded by `mutex`: it is
    // read and written from whatever thread calls apply_assignments
    // (including the IPC reader thread) and from the queued task on the UI
    // thread, and a plain compare-exchange is all the coalescing needs.
    std::atomic<bool> audio_reconcile_pending{false};
    // Set when the operator clears audio_group after it had a value; cleared
    // by the reconcile that acts on it. Clearing the field is the operator
    // turning the feature off, usually mid-show and usually because something
    // is wrong — so off has to mean the sources this wall owns actually go
    // quiet. Without this, clearing the field only stopped reconciliation:
    // every participant stayed unmuted, on tracks 1-6, in a group that is in
    // every scene, and stayed owned by a still-live Tiles source so nothing
    // else could adopt or mute them either.
    //
    // A flag rather than testing "audio_group is empty" at reconcile time,
    // because the reconcile has to distinguish "just turned off — mute
    // everything I own, once" from "has been off all along — do nothing,
    // touch nothing".
    //
    // Deliberately not guarded by `mutex`, like audio_reconcile_pending above:
    // it is set on the UI thread under that lock but read by the queued task,
    // and it has to survive a request the collection-load gate suppresses —
    // the post-load sweep picks it up from here.
    std::atomic<bool> audio_off_pending{false};

    // Owned solely by the OBS graphics thread (video_render) — no locking
    // needed. Copy-assigned into rather than constructed per frame: allocation
    // churn on a 60 Hz draw path is the documented root cause of the
    // operator-stutter incident.
    std::vector<TileFeedPtr> render_feeds;
    // Taken in the same critical section as render_feeds above, so a settings
    // change landing mid-frame cannot give one tile the new crop and the next
    // the old one.
    std::array<std::pair<double, double>, kMaxTileSlots> render_slot_crop{};
    // One scratch buffer per slot, holding the pixels last taken from that
    // feed. Grow-only, so a slot that comes back reuses its buffer instead of
    // allocating on the draw path.
    std::vector<TileScratch> render_scratches;
    uint64_t    rendered_frames = 0;
    // Layout animation state, keyed by participant id across frames so a
    // tile's motion survives its slot being repointed to someone else and
    // back. Render-thread only, like the rest of this block — no lock:
    // only tiles_source_render() ever touches it. See
    // src/zoom-tile-animator.h.
    TileAnimator animator;
    // The intermediate render target a tile IN MOTION is composited through
    // before being drawn at a fractional position — see
    // ensure_motion_texrender() and the draw loop in tiles_source_render().
    //
    // Null until the first frame a tile actually moves, so a source whose
    // animation toggle has never been on holds no GPU memory for this at all —
    // nothing on that path reaches the code that creates it.
    //
    // It is NOT freed when the toggle goes back off: once a tile has moved,
    // this stays allocated (one tile-sized render target, well under a
    // megabyte at 1080p) for the source's life. Freeing it on a settings
    // change would mean calling obs_enter_graphics() from the update thread,
    // which blocks on the graphics thread mid-frame — a worse trade than
    // holding the memory. What the toggle guarantees is that the OUTPUT is
    // byte-identical to before this feature existed, and that still holds:
    // the disabled path never touches this texture, never draws through it,
    // and never resizes it.
    //
    // Render-thread only, like the rest of this block; freed in
    // tiles_source_destroy() with the graphics context entered.
    gs_texrender_t *motion_render = nullptr;

    std::shared_ptr<TilesCallbackGate> gate =
        std::make_shared<TilesCallbackGate>();

    // os_gettime_ns() of the last silent-slot sweep, 0 for "never". Atomic and
    // CAS-claimed so the rate limit holds without taking a lock first — the
    // point of the limit is to avoid the lock convoy, so it must be checked
    // before engine_mutex. Same idiom as ZoomSource::m_last_stale_recover_ns.
    std::atomic<uint64_t> last_sweep_ns{0};
};

// ── Feed lifecycle ───────────────────────────────────────────────────────────

static std::string make_tile_feed_uuid(uint64_t instance_id, size_t slot)
{
    return "tile_" + std::to_string(instance_id) + "_" + std::to_string(slot);
}

// Copies the newest frame for `feed` out of its shared-memory region. Runs on
// the engine client's reader thread, which dispatches frames for every source
// in the plugin — so this must not hold feed->mtx any longer than the copy.
static void tile_feed_on_frame(const TileFeedPtr &feed, uint32_t event_width,
                               uint32_t event_height,
                               uint32_t event_participant_id,
                               uint32_t event_shm_gen)
{
    // Drop frames belonging to a participant this slot no longer shows, and
    // capture the epoch to stamp this frame with. begin_frame() does both in
    // the one order that is race-free — see the ordering contract on it; the
    // two halves must not be pulled apart here. The engine stamps every frame
    // event with the subscription's real participant id
    // (engine/src/engine-video.cpp:235).
    uint64_t epoch = 0;
    if (!feed->slot.begin_frame(event_participant_id, epoch)) return;

    std::lock_guard<std::mutex> lock(feed->mtx);
    if (!feed->alive) return;

    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t y_len = 0;
    const ShmFrameRead status =
        shm_read_i420_frame(feed->shm, IPC_SHM_PREFIX + feed->uuid,
                            event_width, event_height, event_shm_gen,
                            feed->shm_gen, feed->frame, w, h, y_len);
    if (status != ShmFrameRead::Ok) return;
    // Odd dimensions have no valid I420 chroma layout to sample.
    if ((w & 1u) || (h & 1u)) return;

    feed->width = w;
    feed->height = h;
    feed->frame_epoch = epoch;
    feed->generation = s_frame_generation.fetch_add(1, std::memory_order_relaxed) + 1;
    feed->has_frame = true;
}

// P720 is a deliberate default: past 2-up a tile is at most half the canvas in
// each axis, so a 720p feed is already oversampled, and the Zoom SDK caps how
// many high-resolution streams may be subscribed at once.
//
// video_only: a tile never plays audio (tile_feed_register supplies no
// on_audio), so without this flag every slot registered an engine-side target
// with isolate_audio=false and audience_audio=false — which is exactly the
// combination that receives *mixed meeting audio* on every callback. Each slot
// cost one SHM write plus one {"cmd":"audio"} IPC line per audio buffer, which
// the plugin then parsed and discarded, on the same reader thread that
// dispatches video frames for every source in the plugin. A nine-tile wall paid
// that nine times over.
static void tile_feed_subscribe(const TileFeedPtr &feed)
{
    ZoomEngineClient::instance().subscribe(feed->uuid, feed->slot.participant_id(),
                                           false, false, VideoResolution::P720,
                                           /*video_only=*/true);
}

// Defined below with the other mapping-lifetime helpers; needed here for the
// new-engine-process callback.
static void tile_feed_release_mapping(const TileFeedPtr &feed);

static void tile_feed_register(const TileFeedPtr &feed)
{
    ZoomEngineClient::instance().register_source(feed->uuid, {
        [feed](uint32_t width, uint32_t height, uint32_t participant_id,
               uint32_t shm_generation) {
            tile_feed_on_frame(feed, width, height, participant_id,
                               shm_generation);
        },
        {},  // tiles are video-only; audio stays on the dedicated audio sources
        // A new engine process restarts the SHM generation counter from
        // nothing, so its first create for this slot's region asks for the
        // legacy unsuffixed name whatever generation the dead engine had
        // reached. A mapping carried across the restart blocks it exactly as a
        // mapping carried across a re-point does. The retry sweep would find
        // the slot silent and release eventually, but only after the operator
        // has watched a dead tile through several backoff intervals.
        [feed]() { tile_feed_release_mapping(feed); }
    });
}

// Releases this slot's read mapping of its shared-memory region. MUST be called
// before any subscribe that re-points an existing uuid, and before the subscribe
// reaches the engine.
//
// This is not redundant cleanup and deleting it reintroduces a production
// incident (2026-08-08). The rule, and the full reasoning for it, live in
// src/shm-resubscribe.h; the tile-specific part is only that `frame` (the
// decoded copy the draw path reads) is separate from `shm` (the window onto the
// engine's buffer), so dropping the mapping discards no pixels — the tile keeps
// showing its last frame until the next frame event reopens the region.
//
// What this wrapper adds over the shared helper is the locking the helper
// requires of its callers: it takes feed->mtx, the innermost lock and the same
// one tile_feed_on_frame() holds while reading `shm`. Callers hold
// ctx->engine_mutex with ctx->mutex released, matching tile_feed_retire().
static void tile_feed_release_mapping(const TileFeedPtr &feed)
{
    if (!feed) return;
    std::lock_guard<std::mutex> lock(feed->mtx);
    // Return value deliberately ignored: it exists so a caller can log a real
    // release once, and the two call sites here already log the reassign /
    // resubscribe they are part of.
    shm_release_for_resubscribe(feed->shm, feed->shm_gen);
}

// Defined with the rest of the draw path below; declared here because teardown
// has to free the feed's plane textures.
static void tile_destroy_textures(TileFeed &feed);

static void tile_feed_retire(const TileFeedPtr &feed)
{
    if (!feed) return;
    ZoomEngineClient::instance().unsubscribe(feed->uuid);
    ZoomEngineClient::instance().unregister_source(feed->uuid);
    blog(LOG_INFO, "[obs-zoom-plugin] Tile slot retired: uuid=%s participant_id=%u",
         feed->uuid.c_str(), feed->slot.participant_id());
    {
        // A callback dispatched before unregister_source() may still be
        // running; it holds feed->mtx, so this blocks until it finishes and
        // then locks it out.
        std::lock_guard<std::mutex> lock(feed->mtx);
        feed->alive = false;
        shm_region_destroy(feed->shm);
    }

    // Retiring runs off the graphics thread, so the plane textures have to be
    // freed with the context entered. Leaking three textures per repointed slot
    // would accumulate GPU memory for the length of a show.
    //
    // OUTSIDE feed->mtx, DELIBERATELY. video_render() runs with the graphics
    // context already held and takes feed->mtx inside it (tile_take_snapshot),
    // so acquiring the two in the opposite order here would be an ABBA
    // deadlock. Entering the context is also what makes this safe without a
    // lock: it blocks until the graphics thread is out of video_render, and the
    // feed is already gone from ctx->feeds, so no later frame can pick it up.
    obs_enter_graphics();
    tile_destroy_textures(*feed);
    obs_leave_graphics();
}

// The engine work implied by a change to the assignment list. Computing it is
// pure bookkeeping; performing it is blocking pipe I/O. They are split so the
// I/O never runs under ctx->mutex, which the draw path takes every frame.
struct FeedPlan {
    std::vector<TileFeedPtr> to_register;     // new slots: register + subscribe
    std::vector<TileFeedPtr> to_resubscribe;  // repointed slots: subscribe
    std::vector<TileFeedPtr> to_retire;       // removed slots: unsubscribe
};

// Brings `ctx->feeds` in line with `ctx->participants`, one feed per tile slot,
// and returns the engine calls that implies. Call with ctx->mutex held; this
// touches only the feeds vector and atomics — never feed->mtx, never the engine.
static FeedPlan plan_feeds_locked(tiles_source *ctx)
{
    FeedPlan plan;
    const size_t wanted = ctx->visible ? ctx->participants.size() : 0;

    while (ctx->feeds.size() > wanted) {
        plan.to_retire.push_back(ctx->feeds.back());
        ctx->feeds.pop_back();
    }
    for (size_t slot = 0; slot < wanted; ++slot) {
        if (slot < ctx->feeds.size()) {
            const TileFeedPtr &feed = ctx->feeds[slot];
            // Retires any frame stored or in flight for the outgoing
            // participant, without touching feed->mtx.
            if (!feed->slot.assign(ctx->participants[slot])) continue;
            plan.to_resubscribe.push_back(feed);
        } else {
            auto feed = std::make_shared<TileFeed>();
            feed->uuid = make_tile_feed_uuid(ctx->instance_id, slot);
            feed->slot.assign(ctx->participants[slot]);
            ctx->feeds.push_back(feed);
            plan.to_register.push_back(feed);
        }
    }
    return plan;
}

// Performs the plan's engine IPC. Must be called with ctx->engine_mutex held
// and ctx->mutex released. Retires run first so freed Zoom subscription slots
// are available to the new subscriptions that follow.
static void execute_feed_plan(const FeedPlan &plan)
{
    for (const auto &feed : plan.to_retire) tile_feed_retire(feed);
    for (const auto &feed : plan.to_register) {
        tile_feed_register(feed);
        tile_feed_subscribe(feed);
        blog(LOG_INFO,
             "[obs-zoom-plugin] Tile slot subscribed: uuid=%s participant_id=%u",
             feed->uuid.c_str(), feed->slot.participant_id());
    }
    for (const auto &feed : plan.to_resubscribe) {
        // Before the subscribe, never after: the engine rebuilds this uuid's
        // SHM region from generation 0 and would be blocked by our live mapping
        // of the old one. See tile_feed_release_mapping().
        tile_feed_release_mapping(feed);
        tile_feed_subscribe(feed);
        blog(LOG_INFO,
             "[obs-zoom-plugin] Tile slot reassigned: uuid=%s participant_id=%u",
             feed->uuid.c_str(), feed->slot.participant_id());
    }
}

// Re-issues the subscription for any slot that has not produced a frame under
// its *current* assignment. A participant assigned before they joined the
// meeting otherwise stays blank until the operator reopens the properties
// dialog. Holds engine_mutex so it cannot resurrect a subscription for a slot a
// concurrent plan is retiring.
//
// The test is per-epoch, not "never delivered anything": a slot that showed a
// previous assignee has a non-zero generation forever, so keying off that would
// permanently exclude every repointed tile — exactly the slots most likely to
// need a retry, since repointing at somebody who has not joined yet is the
// common case.
//
// Rate-limited across the source and attempt-bounded per slot — see
// zoom-tile-retry.h for why both are needed, and for why the sweep is left on
// every roster event rather than filtered to roster-only ones.
static void resubscribe_silent_feeds(tiles_source *ctx)
{
    // Claim the sweep before taking any lock. The whole point of the interval
    // is that a burst of roster events must not each acquire engine_mutex,
    // ctx->mutex and every feed->mtx on the engine reader thread.
    const uint64_t now_ns = os_gettime_ns();
    uint64_t last_sweep = ctx->last_sweep_ns.load(std::memory_order_acquire);
    if (!tile_sweep_due(now_ns, last_sweep)) return;
    if (!ctx->last_sweep_ns.compare_exchange_strong(last_sweep, now_ns,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire))
        return;  // another sweep claimed this interval

    std::lock_guard<std::mutex> engine_lock(ctx->engine_mutex);

    std::vector<TileFeedPtr> retry;
    std::vector<TileFeedPtr> gave_up;  // logged below, with no lock held
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        for (const auto &feed : ctx->feeds) {
            if (!feed) continue;
            std::lock_guard<std::mutex> feed_lock(feed->mtx);
            if (!feed->alive) continue;
            // frame_epoch is the epoch of the last accepted frame and is not
            // cleared when the draw path takes the pixels, so this stays true
            // for a healthy tile and flips back to "silent" on every repoint.
            const uint64_t epoch = feed->slot.epoch();
            if (feed->slot.frame_is_current_at(feed->frame_epoch, epoch)) {
                // Healthy: a later silence gets a full budget again.
                feed->retry_epoch = epoch;
                feed->last_retry_ns = 0;
                feed->retry_attempts = 0;
                feed->retry_exhausted_logged = false;
                continue;
            }
            if (feed->retry_epoch != epoch) {
                // Repointed since the last sweep: fresh assignment, fresh
                // budget. Covers the Auto-mode reflow as well as an operator
                // recasting a tile by hand.
                feed->retry_epoch = epoch;
                feed->last_retry_ns = 0;
                feed->retry_attempts = 0;
                feed->retry_exhausted_logged = false;
            }
            if (!tile_retry_due(now_ns, feed->last_retry_ns,
                                feed->retry_attempts)) {
                if (feed->retry_attempts >= kTileRetryMaxAttempts &&
                    !feed->retry_exhausted_logged) {
                    feed->retry_exhausted_logged = true;
                    gave_up.push_back(feed);
                }
                continue;
            }
            feed->last_retry_ns = now_ns;
            ++feed->retry_attempts;
            retry.push_back(feed);
        }
    }
    // Issued outside ctx->mutex. The engine no-ops a subscribe that is already
    // active at this resolution, so this only revives dead slots.
    //
    // The mapping is released first here too. A retried slot has, by definition,
    // no frame accepted under its current assignment, so there is nothing to
    // lose — but it may still hold a mapping (a frame that opened the region and
    // then failed the header or size check leaves one behind). If the engine
    // finds this uuid's subscription dead it destroys and rebuilds the
    // SourceTarget, restarting at the legacy region name, and a mapping we still
    // hold would block the recreate exactly as on a repoint. See
    // tile_feed_release_mapping().
    for (const auto &feed : retry) {
        tile_feed_release_mapping(feed);
        tile_feed_subscribe(feed);
    }
    // Silence is a real operator-visible state, so say so once per slot rather
    // than letting a tile stay grey with no explanation anywhere.
    for (const auto &feed : gave_up) {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] Tile slot stopped retrying a silent feed: uuid=%s participant_id=%u attempts=%u",
             feed->uuid.c_str(), feed->slot.participant_id(),
             kTileRetryMaxAttempts);
    }
}

// ── Frame handoff ────────────────────────────────────────────────────────────

// Moves the slot's newest frame into the draw path's scratch, if there is one.
// The lock is held only for an O(1) buffer swap — never across the upload,
// which would head-of-line block the engine reader thread that feeds every
// source in the plugin. The swap hands our previous buffer back to the reader
// to refill, so neither side allocates after warm-up.
//
// Returns false when the slot has nothing valid to show, leaving the caller to
// paint the neutral placeholder.
static bool tile_take_snapshot(const TileFeedPtr &feed, TileScratch &scratch)
{
    if (!feed) return false;

    if (scratch.feed_id != feed->id) {  // slot rebuilt: discard stale pixels
        scratch.feed_id = feed->id;
        scratch.generation = 0;
        scratch.epoch = 0;
    }

    {
        std::lock_guard<std::mutex> lock(feed->mtx);
        if (!feed->alive) return false;

        // One epoch load drives both decisions below. Re-reading it for the
        // second would let a repoint landing between them disagree — declining
        // to take the new frame while also declining to drop the old one, so
        // the outgoing participant survives an extra frame.
        const uint64_t epoch = feed->slot.epoch();
        if (feed->has_frame &&
            TileSlotState::frame_is_current_at(feed->frame_epoch, epoch)) {
            scratch.pixels.swap(feed->frame);
            scratch.width = feed->width;
            scratch.height = feed->height;
            scratch.generation = feed->generation;
            feed->has_frame = false;  // the pixels live in the scratch now
        } else if (scratch.epoch != epoch) {
            // Reassigned with nothing new yet: stop showing the old assignee.
            scratch.generation = 0;
        }
        scratch.epoch = epoch;
    }

    if (scratch.generation == 0 || scratch.width < 2 || scratch.height < 2)
        return false;
    const size_t y_len = static_cast<size_t>(scratch.width) * scratch.height;
    return scratch.pixels.size() >= y_len + y_len / 2;
}

// ── GPU draw ─────────────────────────────────────────────────────────────────

// Converts an OBS colour-property value to what gs_effect_set_color() expects.
//
// Byte order, observed rather than assumed: an obs_data colour int (as OBS's
// colour picker and obs-websocket both write it) is 0xAABBGGRR.
// gs_effect_set_color() takes uint32_t argb, i.e. 0xAARRGGBB (graphics.h:442,
// which then calls vec4_from_bgra). Setting "bg_color" to opaque red stored
// 0xFF0000FF; passed straight through, the rendered gutter sampled
// (r=0, g=0, b=255) — blue — confirming the mismatch. Swapping the R and B
// bytes before the call made the same setting render (r=255, g=0, b=0),
// matching the picker. See task-1-report.md for the full trace.
//
// One helper for every colour property this source has, so the border cannot
// drift from the background: they come from the same kind of control and need
// the same swap.
static inline uint32_t picker_color_to_argb(uint32_t picker)
{
    return (picker & 0xFF00FF00u) | ((picker & 0x000000FFu) << 16) |
           ((picker & 0x00FF0000u) >> 16);
}

// Everything the I420 technique needs for one tile that is not a texture.
//
// Bundled into a struct, and taken by tiles_begin_pass() rather than set at the
// call sites, because these are per-tile uniforms: libobs uploads a pass's
// parameters inside gs_technique_begin_pass() and does not re-upload them for
// later draws, so a path that forgot to set them would silently inherit the
// previous tile's border geometry. Routing the video path and the neutral
// placeholder through the same setter makes that impossible rather than
// merely unlikely.
// Plain floats rather than struct vec2/vec4 members: those are C unions of an
// anonymous struct and an array, which brace-initialise awkwardly. The vectors
// are built with libobs's own vec2_set/vec4_set inside tiles_begin_pass().
struct TilePassParams {
    uint32_t border_argb   = 0xFF000000;  // swapped ready for gs_effect_set_color
    float    border_width  = 0.0f;        // canvas pixels, clamped to the tile
    float    corner_radius = 0.0f;        // canvas pixels, clamped to the tile
    float    tile_w = 0.0f;
    float    tile_h = 0.0f;
    // The sub-rectangle of the source texture this draw samples, in normalized
    // texture coords (origin, then size). Defaults to the identity — the whole
    // texture — which is exactly what a full gs_draw_sprite() produces; the
    // video path overrides it with the cover-crop it passes to
    // gs_draw_sprite_subregion(). See the crop_uv comment in
    // data/effects/corevideo-tiles.effect.
    float    crop_u = 0.0f;
    float    crop_v = 0.0f;
    float    crop_cu = 1.0f;
    float    crop_cv = 1.0f;
};

// Everything the Glow technique needs for one tile's halo.
//
// Bundled and set in one place for exactly the reason TilePassParams is:
// libobs uploads a pass's parameters inside gs_technique_begin_pass() and never
// afterwards, so a glow quad that opened a pass having set only some of these
// would draw the previous tile's halo — at the previous tile's position, which
// on a clipped edge tile is visibly wrong rather than subtly wrong.
struct GlowPassParams {
    uint32_t color_argb = 0xFFFFFFFF;  // swapped ready for gs_effect_set_color
    float    quad_w = 0.0f;            // the expanded quad, in canvas pixels
    float    quad_h = 0.0f;
    float    center_x = 0.0f;          // the tile's centre, in quad-local px
    float    center_y = 0.0f;
    float    half_w = 0.0f;            // half the TILE's size, not the quad's
    float    half_h = 0.0f;
    float    corner_radius = 0.0f;     // the tile's own clamped radius
    float    size = 0.0f;              // falloff distance, canvas pixels
    float    intensity = 1.0f;         // 0..1 alpha at the tile's edge
    float    falloff = 0.0f;           // 0..2 curve shape; 0 = (1-t)^2
};

// One shared 1x1 sample per plane holding the same neutral bytes the CPU
// compositor wrote. Drawing the placeholder through the I420 technique makes it
// bit-identical to the old neutral fill by construction, rather than by an RGB
// constant that has to be trusted to round the same way. Shared across every
// Tiles source, like the effect itself.
static gs_texture_t *s_neutral_y = nullptr;
static gs_texture_t *s_neutral_u = nullptr;
static gs_texture_t *s_neutral_v = nullptr;
static bool s_neutral_failed_logged = false;

// Both of these touch gs_* objects, so every caller must already hold the
// graphics context: video_render has it entered for us, module unload does not.
static void destroy_neutral_textures()
{
    gs_texture_destroy(s_neutral_y);  // null-safe (libobs graphics.c:2418)
    gs_texture_destroy(s_neutral_u);
    gs_texture_destroy(s_neutral_v);
    s_neutral_y = s_neutral_u = s_neutral_v = nullptr;
}

static bool ensure_neutral_textures()
{
    if (s_neutral_y && s_neutral_u && s_neutral_v) return true;
    // All three or none: a half-built set would bind a null plane and draw
    // whatever the previous pass left there.
    destroy_neutral_textures();

    // The same named constants the CPU path filled its neutral rects with, so
    // an edit to one path cannot silently drift from the other.
    static const uint8_t y_byte  = kNeutralY;
    static const uint8_t uv_byte = kNeutralUV;
    const uint8_t *y_data  = &y_byte;
    const uint8_t *uv_data = &uv_byte;
    s_neutral_y = gs_texture_create(1, 1, GS_R8, 1, &y_data, 0);
    s_neutral_u = gs_texture_create(1, 1, GS_R8, 1, &uv_data, 0);
    s_neutral_v = gs_texture_create(1, 1, GS_R8, 1, &uv_data, 0);
    if (s_neutral_y && s_neutral_u && s_neutral_v) return true;

    destroy_neutral_textures();
    // Once, not once per vsync: a failing graphics device would otherwise fill
    // the log at the frame rate. An invisible wall with no explanation is the
    // worst symptom, so say it at least once.
    if (!s_neutral_failed_logged) {
        s_neutral_failed_logged = true;
        blog(LOG_ERROR,
             "[obs-zoom-plugin] Tiles: could not create the neutral placeholder "
             "textures; the wall will not draw");
    }
    return false;
}

// Both logged once rather than once per vsync: these failures are persistent
// (a broken graphics device, or a technique with no pass 0), so at the frame
// rate they would bury every other line in the log. Once is still essential —
// a wall that silently will not draw is the worst symptom there is.
static bool s_tile_texture_failed_logged = false;
static bool s_tile_pass_failed_logged = false;
static bool s_bg_pass_failed_logged = false;
static bool s_glow_pass_failed_logged = false;
// The operator asked for a glow that the loaded effect cannot draw — i.e. a
// stale corevideo-tiles.effect beside a new DLL. tiles_effect_load() already
// logged the full detail once at module load; this fires at the moment the
// operator actually notices (they moved the size off zero and saw nothing), so
// it points back at that line rather than repeating it every frame.
static bool s_glow_unavailable_logged = false;
// The intermediate render target for tiles in motion could not be created, at
// all or at a particular size. Once, not once per vsync, for the same reason
// as the flags above — and essential at least once, because the visible
// symptom (motion stepping on the 2px grid again) is subtle enough that
// nobody would think to look for a graphics failure behind it.
static bool s_motion_render_failed_logged = false;

// Frees one feed's plane textures and forgets what they held. The caller must
// already hold the graphics context.
static void tile_destroy_textures(TileFeed &feed)
{
    gs_texture_destroy(feed.tex_y);  // null-safe (libobs graphics.c:2418)
    gs_texture_destroy(feed.tex_u);
    gs_texture_destroy(feed.tex_v);
    feed.tex_y = feed.tex_u = feed.tex_v = nullptr;
    feed.tex_w = 0;
    feed.tex_h = 0;
    feed.uploaded_generation = 0;
}

// Brings one feed's three plane textures in line with the pixels just taken
// into `scratch`. Runs on the graphics thread with the context already entered.
// Returns false when there is nothing usable to draw, leaving the caller to
// paint the neutral placeholder instead of binding a null plane.
static bool tile_upload_frame(const TileFeedPtr &feed, const TileScratch &scratch)
{
    const uint32_t w = scratch.width;
    const uint32_t h = scratch.height;

    // Reallocate only when this participant's stream size actually changed.
    // The engine raises and lowers a participant's resolution during a call, so
    // this is not a one-off — but it is rare, and recreating three textures per
    // frame would be a real cost on a nine-tile wall.
    if (tile_texture_needs_realloc(feed->tex_w, feed->tex_h, w, h)) {
        tile_destroy_textures(*feed);
        feed->tex_y = gs_texture_create(w, h, GS_R8, 1, nullptr, GS_DYNAMIC);
        feed->tex_u = gs_texture_create(w / 2, h / 2, GS_R8, 1, nullptr, GS_DYNAMIC);
        feed->tex_v = gs_texture_create(w / 2, h / 2, GS_R8, 1, nullptr, GS_DYNAMIC);
        if (!feed->tex_y || !feed->tex_u || !feed->tex_v) {
            // All three or none, exactly as with the neutral set: a half-built
            // set binds a null plane and samples whatever the previous pass
            // left there. Resetting tex_w/tex_h to 0 also means the next frame
            // retries rather than treating the failure as a valid allocation.
            tile_destroy_textures(*feed);
            if (!s_tile_texture_failed_logged) {
                s_tile_texture_failed_logged = true;
                blog(LOG_ERROR,
                     "[obs-zoom-plugin] Tiles: could not create the %ux%u plane "
                     "textures for a tile; it will stay grey", w, h);
            }
            return false;
        }
        feed->tex_w = w;
        feed->tex_h = h;
        feed->uploaded_generation = 0;  // new textures hold nothing
    }
    if (!feed->tex_y || !feed->tex_u || !feed->tex_v) return false;

    // Skip the upload entirely when the feed has produced no new frame. An idle
    // wall must not re-upload unchanged pixels every vsync — a nine-tile 720p
    // wall at 60 Hz would be ~150 MB/s of PCIe traffic for no visible change.
    if (tile_texture_needs_upload(feed->uploaded_generation, scratch.generation)) {
        // tile_take_snapshot() has already checked that the buffer holds a full
        // I420 frame of these dimensions, so these offsets are in bounds.
        const uint8_t *y = scratch.pixels.data();
        const uint8_t *u = y + static_cast<size_t>(w) * h;
        const uint8_t *v = u + (static_cast<size_t>(w) * h) / 4;
        gs_texture_set_image(feed->tex_y, y, w, false);
        gs_texture_set_image(feed->tex_u, u, w / 2, false);
        gs_texture_set_image(feed->tex_v, v, w / 2, false);
        feed->uploaded_generation = scratch.generation;
    }
    return true;
}

// Binds one tile's three planes and its border geometry, then opens a pass.
//
// THE BINDING MUST HAPPEN BEFORE THE PASS OPENS, AND EVERY TILE NEEDS ITS OWN
// PASS. gs_technique_begin_pass() uploads the effect's parameters to the shader
// (upload_parameters(), libobs/graphics/effect.c:209) and nothing re-uploads
// them per draw call. Each tile binds *different* textures, so rebinding inside
// an already-open pass silently draws the previously-bound planes — or nothing.
// (The alternative is one pass plus gs_effect_update_params() after every
// rebind; a pass per tile is the same cost and much harder to get wrong.)
//
// The border uniforms are per-tile for exactly the same reason and are set
// here, in the same breath as the textures, so no draw path can open a pass
// having set one and not the other: a tile that skipped them would inherit the
// previous tile's border width, radius and crop rect.
//
// Returns false when the pass could not be opened, in which case the draw must
// be skipped: gs_technique_begin_pass() loads the pass's vertex and pixel
// shaders, so drawing after a failed call runs against whatever shader was
// loaded last — a wall drawn with some other source's effect.
static bool tiles_begin_pass(gs_technique_t *tech, gs_texture_t *y,
                             gs_texture_t *u, gs_texture_t *v,
                             const TilePassParams &p)
{
    gs_effect_set_texture(s_tiles_effect.param_y, y);
    gs_effect_set_texture(s_tiles_effect.param_u, u);
    gs_effect_set_texture(s_tiles_effect.param_v, v);

    gs_effect_set_color(s_tiles_effect.param_border_color, p.border_argb);
    gs_effect_set_float(s_tiles_effect.param_border_width, p.border_width);
    gs_effect_set_float(s_tiles_effect.param_corner_radius, p.corner_radius);
    struct vec2 tile_size;
    vec2_set(&tile_size, p.tile_w, p.tile_h);
    gs_effect_set_vec2(s_tiles_effect.param_tile_size, &tile_size);
    struct vec4 crop_uv;
    vec4_set(&crop_uv, p.crop_u, p.crop_v, p.crop_cu, p.crop_cv);
    gs_effect_set_vec4(s_tiles_effect.param_crop_uv, &crop_uv);

    if (gs_technique_begin_pass(tech, 0)) return true;

    if (!s_tile_pass_failed_logged) {
        s_tile_pass_failed_logged = true;
        blog(LOG_ERROR,
             "[obs-zoom-plugin] Tiles: gs_technique_begin_pass failed on the "
             "I420 technique; the wall will not draw");
    }
    return false;
}

// Binds one tile's halo geometry, then opens a pass on the Glow technique.
//
// SET BEFORE THE PASS OPENS, ONE PASS PER TILE — the same rule, and the same
// reason, as tiles_begin_pass() above: gs_technique_begin_pass() is where
// libobs uploads a pass's parameters (upload_parameters(),
// libobs/graphics/effect.c:209) and nothing re-uploads them per draw call.
//
// Returns false when the pass could not be opened, in which case the draw must
// be skipped: begin_pass loads the pass's shaders, so drawing after a failed
// call runs against whatever shader was loaded last.
static bool glow_begin_pass(gs_technique_t *tech, const GlowPassParams &p)
{
    gs_effect_set_color(s_tiles_effect.param_glow_color, p.color_argb);
    struct vec2 quad_size;
    vec2_set(&quad_size, p.quad_w, p.quad_h);
    gs_effect_set_vec2(s_tiles_effect.param_glow_quad_size, &quad_size);
    struct vec2 center;
    vec2_set(&center, p.center_x, p.center_y);
    gs_effect_set_vec2(s_tiles_effect.param_glow_tile_center, &center);
    struct vec2 half;
    vec2_set(&half, p.half_w, p.half_h);
    gs_effect_set_vec2(s_tiles_effect.param_glow_tile_half, &half);
    gs_effect_set_float(s_tiles_effect.param_glow_corner_radius, p.corner_radius);
    gs_effect_set_float(s_tiles_effect.param_glow_size, p.size);
    gs_effect_set_float(s_tiles_effect.param_glow_intensity, p.intensity);
    gs_effect_set_float(s_tiles_effect.param_glow_falloff, p.falloff);

    if (gs_technique_begin_pass(tech, 0)) return true;

    if (!s_glow_pass_failed_logged) {
        s_glow_pass_failed_logged = true;
        blog(LOG_ERROR,
             "[obs-zoom-plugin] Tiles: gs_technique_begin_pass failed on the "
             "Glow technique; the tile glow will not draw");
    }
    return false;
}

// Paints one rect with the neutral placeholder, through the same I420 technique
// and the same 1x1 0x80 planes the whole path uses.
//
// It takes the same TilePassParams as a video tile, so a slot with no frame
// gets the operator's border at its own rect rather than the previous tile's.
// gs_draw_sprite() maps the whole texture, so the caller's default identity
// crop rect is the correct one here — nothing to override.
static void tiles_draw_neutral(gs_technique_t *tech, uint32_t x, uint32_t y,
                               uint32_t w, uint32_t h,
                               const TilePassParams &p)
{
    if (!tiles_begin_pass(tech, s_neutral_y, s_neutral_u, s_neutral_v, p))
        return;
    gs_matrix_push();
    gs_matrix_translate3f(static_cast<float>(x), static_cast<float>(y), 0.0f);
    gs_draw_sprite(s_neutral_y, 0, w, h);
    gs_matrix_pop();
    gs_technique_end_pass(tech);
}

// Even-rounds one value onto a valid I420 chroma boundary — the same need
// snap_tile_grid_even() (src/zoom-tile-grid.cpp) exists for, applied to one
// tile in isolation rather than a whole grid. Mirrors that file's
// even_floor/even_round split without reusing it (both are file-local
// there): sizes are floored so an independently snapped tile never grows
// past the animator's own rect, positions are rounded to the nearest even
// value so they stay close to the animator's true position rather than
// always shifting toward the origin. See snap_tile_independently() below for
// why this exists instead of just calling snap_tile_grid_even().
static uint32_t even_floor_px(double v)
{
    if (v <= 0.0) return 0;
    return static_cast<uint32_t>(v) & ~1u;
}

static uint32_t even_round_px(double v)
{
    if (v <= 0.0) return 0;
    return static_cast<uint32_t>(std::lround(v / 2.0)) * 2u;
}

// Snaps one tile's rect to even pixel boundaries independently of every
// other tile on the wall. Used only while something is in motion — see the
// mode-split comment in tiles_source_render() for why snap_tile_grid_even()
// cannot be used there. This loses that function's guarantee of identical
// gaps between neighbours (already meaningless mid-reflow, since tiles are
// at different positions and sizes by definition while they move) and
// quantises position and size to 2px steps.
//
// A tile that is actually MOVING no longer draws at the position this
// returns: the sub-pixel path in tiles_source_render() takes only the even
// SIZE from here — which is what the intermediate texture has to be, for the
// same I420 chroma reason — and draws at the animator's true fractional
// position instead. The position this computes still matters for the tiles
// this file draws directly: everything on a wall that is not settled but is
// itself at rest (a newcomer seeded at its target while its neighbours
// reflow), and any tile whose composite could not be made.
static SnappedTileRect snap_tile_independently(const TileRect &r)
{
    SnappedTileRect s;
    s.x      = even_round_px(r.x);
    s.y      = even_round_px(r.y);
    s.width  = even_floor_px(r.width);
    s.height = even_floor_px(r.height);
    return s;
}

// Where a tile in motion actually is, as opposed to the even-boundary rect it
// was snapped to. One entry per feed, index-aligned with the `rects` the draw
// loop reads.
//
// The vector this fills is EMPTY whenever the wall is settled — which includes
// every single frame the animation toggle is off, since the disabled bypass in
// TileAnimator::advance() leaves settled() unconditionally true. That
// emptiness is what keeps this whole feature out of the disabled path: the
// draw loop's only reference to it is `i >= moving.size()`, which is true for
// every tile, so no branch is taken, no intermediate texture is created, and
// the draw calls are the ones this file has always made.
struct MovingTile {
    bool   moving = false;  // false: draw at the snapped rect, exactly as before
    double x = 0.0;         // the animator's true, unsnapped canvas position
    double y = 0.0;
    // The animator's true, unsnapped SIZE. The intermediate texture is still
    // the even-rounded size — that is what the I420 chroma rule requires of
    // the surface the tile is composited into — but the finished tile is
    // scaled onto these dimensions when it is blitted, so the trailing edge
    // lands at exactly x + width rather than at x + even_floor(width).
    // Without this the leading edge moves smoothly while the trailing edge
    // steps 2px, which reads worse than the uniform stepping it replaced:
    // two edges of the same rectangle on different quantisation grids.
    double width = 0.0;
    double height = 0.0;
    double alpha = 1.0;     // the animator's own per-tile alpha
};

// The intermediate render target a tile in motion is composited through.
// Created on the first frame a tile actually moves, reused for the rest of the
// source's life, and destroyed in tiles_source_destroy(). Its texture is
// (re)allocated by gs_texrender_begin() whenever the requested size changes,
// which during a reflow is most frames — a tile's size is part of what is
// being animated. That cost is bounded (one small texture per distinct tile
// size per frame, and only while something is moving) and is why this is not
// reached at all when nothing is.
//
// GS_RGBA, deliberately 8-bit rather than GS_RGBA16F: OBS's own SDR render
// texture is 8-bit (GS_BGRA — obs_init_textures(), libobs/obs.c), so an 8-bit
// intermediate quantises on exactly the same grid the direct path's write
// does, and round-to-8-bit is idempotent — the round trip through this texture
// reproduces the direct path's own bytes. A 16-bit float intermediate would
// round onto a finer grid first and the 8-bit one second, which flips the last
// bit on a small percentage of pixels: a smaller error per pixel, but a real
// difference between two paths that currently have none. See task-7-report.md.
//
// The caller must already hold the graphics context; video_render does.
static gs_texrender_t *ensure_motion_texrender(tiles_source *ctx)
{
    if (!ctx->motion_render) {
        ctx->motion_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        if (!ctx->motion_render && !s_motion_render_failed_logged) {
            s_motion_render_failed_logged = true;
            blog(LOG_ERROR,
                 "[obs-zoom-plugin] Tiles: could not create the intermediate "
                 "render target for layout animation; tiles in motion will "
                 "step on the 2px chroma grid instead of moving smoothly");
        }
    }
    return ctx->motion_render;
}

// Scales what has already been drawn into the bound render target by `alpha`:
// the colour is left exactly as it is, and dst.a becomes alpha * dst.a.
//
// This is how the sub-pixel path honours AnimatedTile::alpha. It is a second
// quad rather than a shader uniform because the I420 technique has no
// global-alpha parameter, and adding one means changing
// data/effects/corevideo-tiles.effect — which a stale effect file beside a new
// DLL would then not have (see zoom-tiles-effect-policy.h). The blend factors
// do the multiply instead: ZERO/ONE for colour leaves the destination
// untouched, ZERO/SRCALPHA for alpha replaces dst.a with src.a * dst.a, and
// the Solid technique supplies src.a from fill_color's alpha byte.
//
// Not reachable today, and deliberately so rather than by oversight: every
// tile this path draws is a LIVE tile, and TileAnimator::advance() hardcodes
// 1.0 for every live tile it emits. The only tiles that fade are exiting ones,
// which this task does not draw at all — their pixel source is an open
// decision with the repo owner. This exists so the alpha the animator computes
// is honoured rather than silently dropped the day those tiles do get drawn,
// and the caller skips it entirely at alpha == 1.0, so the live path pays
// nothing for it.
static void fade_tile_alpha(uint32_t w, uint32_t h, double alpha)
{
    const double clamped = alpha < 0.0 ? 0.0 : (alpha > 1.0 ? 1.0 : alpha);
    const uint32_t a8 = static_cast<uint32_t>(std::lround(clamped * 255.0));
    // gs_effect_set_color() takes 0xAARRGGBB and reads the alpha from the top
    // byte (vec4_from_bgra, graphics/vec4.h); the colour bytes are zero here
    // because the ZERO/ONE colour factors discard them anyway.
    gs_effect_set_color(s_tiles_effect.param_color, a8 << 24);

    gs_enable_blending(true);
    gs_blend_function_separate(GS_BLEND_ZERO, GS_BLEND_ONE,
                               GS_BLEND_ZERO, GS_BLEND_SRCALPHA);
    gs_technique_t *const solid = s_tiles_effect.tech_solid;
    gs_technique_begin(solid);
    if (gs_technique_begin_pass(solid, 0)) {
        gs_draw_sprite(nullptr, 0, w, h);
        gs_technique_end_pass(solid);
    }
    gs_technique_end(solid);
    // Back to how the composite left it. The caller's gs_blend_state_pop()
    // restores the factors as well; this only keeps the enable flag honest for
    // anything drawn between here and there.
    gs_enable_blending(false);
}

// Draws the wall. Runs on the OBS graphics thread with the graphics context
// already entered — do not nest obs_enter_graphics() in here.
static void tiles_source_render(void *data, gs_effect_t *)
{
    auto *ctx = static_cast<tiles_source *>(data);
    if (!s_tiles_effect.valid() || !ensure_neutral_textures()) return;

    const uint32_t canvas_w = ctx->canvas_width.load(std::memory_order_acquire);
    const uint32_t canvas_h = ctx->canvas_height.load(std::memory_order_acquire);
    if (canvas_w < 2 || canvas_h < 2) return;

    // Snapshot the feed list under the lock, then release it — the draw below
    // must never hold ctx->mutex, which update and roster changes also take.
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->render_feeds = ctx->feeds;
        ctx->render_slot_crop = ctx->slot_crop;
    }
    // No early return on an empty feed list: the CPU path still emitted a full
    // neutral canvas with nobody on the wall, so an empty wall must still paint
    // itself grey rather than disappear. solve_tile_grid(0, ...) returns no
    // rects, so the tile loop below simply does nothing.
    const std::vector<TileFeedPtr> &feeds = ctx->render_feeds;
    // Grow-only: a slot that comes back reuses its buffer instead of
    // allocating on a 60 Hz draw path.
    if (ctx->render_scratches.size() < feeds.size())
        ctx->render_scratches.resize(feeds.size());

    // The parameters the grid is solved from. At their defaults these are
    // byte-for-byte the ones the CPU compositor solved from — 16:9 tiles and a
    // canvas_height/135 gutter and margin — including the even-snapping pass;
    // resolve_spacing_px() reproduces that division exactly, which
    // tests/tile-shape-test.cpp pins at every canvas height the source accepts.
    // Dropping the snap, or letting the spacing default drift by a bit, moves
    // every tile by up to a pixel against the parity baseline in
    // docs/design-reference/. Solved from feeds.size(), as the compositor did,
    // not from participants.size(): the two can differ while a plan is in
    // flight.
    //
    // Each atomic is read exactly once here. Reading tile_aspect a second time
    // for the crop below would let a settings change landing mid-frame solve
    // the grid at one shape and sample at another, which is precisely the
    // mis-framing this control has to avoid.
    const double canvas_h_d = static_cast<double>(canvas_h);
    TileGridParams params;
    params.canvas_width  = static_cast<double>(canvas_w);
    params.canvas_height = canvas_h_d;
    params.tile_aspect   = ctx->tile_aspect.load(std::memory_order_acquire);
    params.gutter        = resolve_spacing_px(
        ctx->gutter_pct.load(std::memory_order_acquire), canvas_h_d);
    params.margin        = resolve_spacing_px(
        ctx->margin_pct.load(std::memory_order_acquire), canvas_h_d);
    const std::vector<TileRect> solved = solve_tile_grid(feeds.size(), params);
    std::vector<DesiredTile> desired;
    desired.reserve(feeds.size());
    for (size_t i = 0; i < feeds.size() && i < solved.size(); ++i) {
        // Defensive, not reachable today: plan_feeds_locked() only ever
        // pushes make_shared<TileFeed>(), so feeds[i] is never null. Guarded
        // anyway for consistency with the null checks further down in this
        // function (the live-tile lookup in the motion branch, and the main
        // draw loop), so the file has one answer to "can a feed be null
        // here", not two that could quietly drift apart.
        const uint32_t pid = feeds[i] ? feeds[i]->slot.participant_id() : 0;
        desired.push_back(DesiredTile{pid, solved[i]});
    }

    AnimationSettings anim;
    anim.enabled = ctx->animate.load(std::memory_order_acquire);
    anim.duration_seconds =
        static_cast<double>(ctx->animate_ms.load(std::memory_order_acquire)) / 1000.0;

    // `departed` is a STATE, not an event: which tracked participants are gone
    // from the meeting roster right now, recomputed every frame. It is what
    // separates a genuine departure (fade out) from an operator repointing a
    // slot (instant cut, no fade) — the distinction the exit invariants in
    // src/zoom-tile-slot.h rest on. Hardcoding it empty would make the exit
    // path dead code on the wall.
    // Computed in THIS pass, from the same frame the layout was solved in. If
    // `departed` lags `desired` by even one frame, the exit exception silently
    // never fires and the whole fade becomes dead code.
    //
    // Gated on `anim.enabled` AND `tracked` being non-empty — either half
    // missing and the render path must not take ZoomEngineClient's roster
    // mutex when it has nothing to ask it. That mutex is hot: the engine
    // reader thread takes it for every "frame" and "audio" message across
    // every source in the plugin (see zoom-engine-client.h's own warning on
    // stalling that dispatch), and roster() deep-copies the whole
    // participant list, one std::string allocation per participant, while
    // holding it. Before this feature existed the draw path took exactly
    // one lock, for an O(1) pointer copy; this guard is what keeps that
    // true when the toggle is off, rather than adding a lock + N
    // allocations on the graphics thread unconditionally. Do not remove
    // either half as "redundant" — it is the difference between "disabled
    // costs nothing" and "disabled costs a roster copy every frame".
    //
    // `anim.enabled` is not implied by `tracked.empty()`: tracked_ids()
    // reflects `m_tiles` as of the END of the PREVIOUS frame's advance()
    // call, and only advance()'s own disabled branch clears it. So on the
    // very first frame after the operator flips the toggle off while
    // anything was mid-flight, `tracked` is still non-empty from last
    // frame — `!tracked.empty()` alone would pass and pay for a roster
    // copy that `advance()`'s disabled branch is about to discard unread
    // (it never looks at `departed`). Checking `anim.enabled` directly
    // closes that one-frame gap. On the enabled path `anim.enabled` is
    // always true, so the condition reduces to exactly `!tracked.empty()`,
    // unchanged from before.
    std::vector<uint32_t> departed;
    const std::vector<uint32_t> tracked = ctx->animator.tracked_ids();
    if (anim.enabled && !tracked.empty()) {
        const std::vector<ParticipantInfo> roster =
            ZoomEngineClient::instance().roster();
        for (const uint32_t id : tracked) {
            const bool in_roster = std::any_of(
                roster.begin(), roster.end(),
                [id](const ParticipantInfo &p) { return p.user_id == id; });
            if (!in_roster)
                departed.push_back(id);
        }
    }

    const std::vector<AnimatedTile> animated =
        ctx->animator.advance(os_gettime_ns(), desired, anim, departed);

    // ── Mode split: pixel-exact when settled, approximate only while moving ─
    //
    // snap_tile_grid_even() assumes a UNIFORM grid: one tile size, one gutter,
    // every tile at a whole multiple of (size+gutter) from its row origin (see
    // zoom-tile-grid.cpp — it derives the single tile_w/tile_h it uses for
    // EVERY row from rects[0] alone). That holds for the solver's own output,
    // but not for the animator's in general: a tile that has just joined is
    // seeded at ITS target size while the others are still springing from the
    // old one ("a newly seen tile starts at its target position" — see
    // TileAnimator::advance()), so mid-reflow the tiles the animator returns
    // can legitimately differ in size from each other. Feeding a mixed-size
    // list to a function that derives ONE size from its first element
    // silently mis-sizes every tile but the first for as long as the reflow
    // lasts — confirmed against this exact code with a standalone repro
    // before this split existed; see task-6-report.md.
    //
    // The gate is `ctx->animator.settled()`, deliberately NOT "every
    // AnimatedTile this frame reports at_rest": that per-tile flag is also
    // true in two situations where the WALL as a whole still disagrees with
    // a fresh solve — a departure's settle hold (the held tile is frozen,
    // at_rest, while the survivors sit on their OLD targets, also at_rest,
    // while a fresh solve already reflects the new smaller grid — feed-list
    // resizing is not gated on this animator's own settle window at all),
    // and symmetrically a join's settle hold (a not-yet-committed newcomer
    // is held at its own target while the already-committed tiles hold
    // THEIR old ones — everything at_rest, and `animated.size() ==
    // desired.size()`, so a count check would not catch this case either).
    // Both were caught by re-running the repro below after implementing the
    // naive `all_of(at_rest)` version first; see task-6-report.md. settled()
    // is false in both cases (pending set change, or a held/exiting tile
    // present), which is exactly what routes them to the per-tile branch
    // instead.
    //
    // `rects`, below, ends up index-aligned with `feeds` — one rect per feed,
    // in feed order — exactly the shape this file has always drawn from, so
    // the glow loop, the tile loop and the log line after it are UNCHANGED
    // from before this feature existed. Only how each entry is computed
    // differs, by branch:
    //
    // `moving`, alongside it, records which of those tiles the animator says
    // are actually in motion, and where they truly are before the snap. It
    // stays EMPTY on the settled branch — so it is empty on every frame the
    // toggle is off — and the draw loop treats an absent entry as "not
    // moving", which is the pre-feature behaviour exactly.
    std::vector<SnappedTileRect> rects;
    std::vector<MovingTile> moving;
    if (ctx->animator.settled()) {
        // Disabled, or settled — no pending set change, nothing held or
        // exiting, every spring at its own target (see TileAnimator::
        // settled()): solve and snap FRESH from `solved`/`params` — the
        // exact call this file has always made — rather than routing the
        // animator's rects through the snap. That makes this the SAME call
        // as before this feature existed, so parity holds by construction
        // rather than by the coincidence that the animator's numbers happen
        // to match. It is also deliberately ONE branch for both "disabled"
        // and "settled", not two that have to be kept in agreement: the
        // disabled bypass in advance() clears every piece of state
        // settled() reads (m_tiles, m_has_pending — see its own header
        // comment), so settled() is unconditionally true right after a
        // disabled call, making disabled simply a case of this condition
        // rather than a separate path. `solved` has exactly one entry per
        // feed (or is empty — solve_tile_grid() is all-or-nothing), so this
        // is index-aligned with `feeds` the same way the pre-animation code
        // was, with no held/exiting entries to consider: `solved` never
        // contains one, since a departed participant is already gone from
        // `feeds` by the time it shrinks the count this is solved from.
        rects = snap_tile_grid_even(solved, params);
    } else {
        // Not settled — a set change is pending, or something is held,
        // exiting, or still springing: snap_tile_grid_even()'s single-
        // shared-size assumption does not hold (see the mode-split comment
        // above), so each tile is snapped independently instead, one rect
        // per feed.
        //
        // The rect for feed i is found by searching `animated` for the
        // entry whose participant id matches feed i's — not by position —
        // which is what makes skipping held and exiting tiles deliberate
        // rather than an accident of array shape. Held and exiting tiles
        // (see the ordering contract on TileAnimator::advance()) have no
        // entry in `feeds` and no defined pixel source: a repointed slot's
        // stored frame is exactly what src/zoom-tile-slot.h invalidates on
        // purpose. What they should draw is still an open decision with the
        // repo owner, so this task draws live tiles only; searching from
        // `feeds` outward means a held or exiting entry in `animated` is
        // simply never looked at here, rather than silently dropped through
        // an index that happened not to line up. The reflow animation is
        // fully functional without them; only exit fades are not drawn yet.
        //
        // `live` is non-null whenever feed is non-null: `desired` is built
        // one-for-one from `feeds` above, and advance()'s live-tile loop
        // emits exactly one output entry for every entry of `desired`,
        // pending or committed alike — so every feed has exactly one
        // matching entry in `animated`. The null fallback below is only for
        // a null feed pointer.
        rects.reserve(feeds.size());
        moving.reserve(feeds.size());
        for (const TileFeedPtr &feed : feeds) {
            const uint32_t pid = feed ? feed->slot.participant_id() : 0;
            const AnimatedTile *live = nullptr;
            for (const auto &t : animated) {
                if (t.participant_id == pid) { live = &t; break; }
            }
            rects.push_back(live ? snap_tile_independently(live->rect)
                                  : SnappedTileRect{});

            // The same lookup decides whether this tile takes the sub-pixel
            // path in the draw loop below. The test is this tile's own
            // `at_rest`, deliberately NOT the whole-wall settled() gate above:
            // the two answer different questions. settled() asks "does a fresh
            // solve describe what the animator would draw", which is about
            // which rects are safe to use; at_rest asks "is THIS tile moving",
            // which is about whether resampling it through an intermediate
            // texture buys anything. A newcomer seeded at its target sits
            // perfectly still while its neighbours reflow around it — it takes
            // the snapped blit and stays pixel-crisp, exactly as it would have
            // before this feature existed.
            MovingTile m;
            if (live && !live->at_rest) {
                m.moving = true;
                m.x      = live->rect.x;
                m.y      = live->rect.y;
                m.width  = live->rect.width;
                m.height = live->rect.height;
                m.alpha  = live->alpha;
            }
            moving.push_back(m);
        }
    }

    // Background first, so tiles and their borders draw over it. This replaces
    // what used to be a fixed neutral fill drawn through the I420 technique
    // purely for CPU-path parity; the per-tile placeholder below
    // (tiles_draw_neutral) is untouched and still uses that path, because it
    // remains parity-critical for tiles with no frame. The full-canvas fill is
    // now the operator's "bg_color" setting, drawn through the effect's Solid
    // technique. The default, 0xFF808080, is grey in either byte order, so an
    // existing scene looks unchanged until the operator picks a colour.
    //
    // The picker-to-ARGB byte swap is picker_color_to_argb(); the evidence for
    // it is documented there.
    const uint32_t fill_argb =
        picker_color_to_argb(ctx->bg_color.load(std::memory_order_acquire));
    gs_effect_set_color(s_tiles_effect.param_color, fill_argb);
    gs_technique_t *solid = s_tiles_effect.tech_solid;
    gs_technique_begin(solid);
    if (gs_technique_begin_pass(solid, 0)) {
        gs_draw_sprite(nullptr, 0, canvas_w, canvas_h);
        gs_technique_end_pass(solid);
    } else if (!s_bg_pass_failed_logged) {
        // Same once-only treatment as tiles_begin_pass() above: a background
        // that silently stops drawing looks like "the gutters went
        // transparent", with no clue why, unless this is logged.
        s_bg_pass_failed_logged = true;
        blog(LOG_ERROR,
             "[obs-zoom-plugin] Tiles: gs_technique_begin_pass failed on the "
             "Solid technique; the background will not draw");
    }
    gs_technique_end(solid);

    // The operator's background source, over the colour and under the tiles.
    // Outside the technique above on purpose: it renders through its own
    // source's effect, so it cannot be drawn inside another technique's pass.
    // No-op when nothing is selected, which is why an unset background leaves
    // the colour-only behaviour byte-for-byte unchanged.
    ctx->background.render(canvas_w, canvas_h);

    // The operator's border, read once for the whole wall. The shape is folded
    // into the radius here rather than stored folded, so switching to Square
    // and back keeps the radius the operator chose.
    const uint32_t border_argb =
        picker_color_to_argb(ctx->border_color.load(std::memory_order_acquire));
    const double border_width_setting =
        static_cast<double>(ctx->border_width.load(std::memory_order_acquire));
    const double corner_radius_setting =
        ctx->border_rounded.load(std::memory_order_acquire)
            ? static_cast<double>(ctx->corner_radius.load(std::memory_order_acquire))
            : 0.0;

    // ── The outer glow, over the background and under the tiles ──────────────
    //
    // Its own pass, on a quad expanded beyond each tile rect, because a tile is
    // drawn as a quad exactly its own size — there is no canvas outside it for
    // a halo to bleed into. The geometry, including the canvas clamping, is
    // solve_glow_quad() (src/zoom-tile-glow.h), which is pure and unit-tested.
    //
    // A size of 0 skips the whole thing: no technique, no blend state, no draw.
    // That is the no-regression guarantee for this control — a wall with the
    // glow off costs nothing and renders byte-for-byte as it did before the
    // control existed — so the guard is here at the top rather than inside the
    // loop. An intensity of 0 skips it too; that one is only a cost saving,
    // since an alpha-0 quad composites to the destination unchanged under the
    // blend factors below.
    const double glow_size_px =
        static_cast<double>(ctx->glow_size.load(std::memory_order_acquire));
    const uint32_t glow_pct = ctx->glow_intensity.load(std::memory_order_acquire);
    const bool glow_wanted = glow_size_px > 0.0 && glow_pct > 0;

    // The third gate, and unlike the two above it this one is not a cost
    // saving: a stale corevideo-tiles.effect beside a new DLL resolves the Glow
    // technique or one of its uniforms to nullptr, and the wall must still
    // draw. See zoom-tiles-effect-policy.h. Gating the whole block — rather
    // than checking inside glow_begin_pass() — means no param_glow_* handle is
    // touched at all, which matters for two reasons: gs_effect_set_*() on a
    // null parameter is harmless but logs LOG_ERROR per call
    // (libobs/graphics/effect.c), and a stale Glow technique was never
    // written to consume the values a current build would set, so drawing
    // through it risks a halo of the wrong colour or size that reads as a
    // driver fault rather than a version mismatch.
    if (glow_wanted && !s_tiles_effect.glow_valid() &&
        !s_glow_unavailable_logged) {
        s_glow_unavailable_logged = true;
        blog(LOG_ERROR,
             "[obs-zoom-plugin] Tiles: the outer glow is switched on but the "
             "loaded effect cannot draw it — the plugin and its data directory "
             "are out of step (see the effect-load error at startup). The rest "
             "of the wall is unaffected.");
    }

    if (glow_wanted && s_tiles_effect.glow_valid()) {
        const uint32_t glow_argb =
            picker_color_to_argb(ctx->glow_color.load(std::memory_order_acquire));
        const float glow_intensity = static_cast<float>(glow_pct) / 100.0f;
        // The operator's percentage mapped onto the curve's own parameter k,
        // whose useful range is [0, 2] — see kMaxGlowSoftness and the shader.
        // 0% is k = 0, which is the original (1-t)^2 exactly.
        const float glow_falloff =
            static_cast<float>(
                ctx->glow_softness.load(std::memory_order_acquire)) *
            (2.0f / static_cast<float>(kMaxGlowSoftness));

        // The halo is transparent everywhere by construction, so it needs
        // blending — and inheriting whatever OBS last set is how a bug becomes
        // machine-dependent, exactly as for the tile pass below.
        //
        // The same factors as that pass, for the same reasons:
        // SRCALPHA/INVSRCALPHA composites the halo over the background, and
        // ONE/INVSRCALPHA for the alpha channel is what libobs's own
        // gs_reset_blend_state() uses (graphics.c:1289-1295). With the naive
        // SRCALPHA/INVSRCALPHA for alpha as well, destination alpha
        // under-accumulates at every partially transparent pixel — which here
        // is the entire halo — and the wall shows a translucent rectangle
        // around each tile once it is filtered, nested in another scene, or
        // routed through the colour-space-conversion texrender path.
        //
        // NOT additive (ONE/ONE), the other obvious choice for a glow: two
        // overlapping halos would blow out towards white instead of staying the
        // colour the operator picked, and overlap is explicitly allowed here.
        //
        // Pushed and popped, so the tile pass that follows starts from exactly
        // the state it always did and pushes its own.
        gs_blend_state_push();
        gs_enable_blending(true);
        gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA,
                                   GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

        gs_technique_t *glow = s_tiles_effect.tech_glow;
        gs_technique_begin(glow);
        for (const SnappedTileRect &r : rects) {
            // Skipped on the same rule the tile loop below uses, so a rect too
            // small to be drawn does not get a halo drawn around nothing.
            if (r.width < 2 || r.height < 2) continue;

            const GlowQuad q =
                solve_glow_quad(r, glow_size_px, canvas_w, canvas_h);
            if (!q.visible) continue;  // clamped away to nothing

            // The tile's own clamped radius, not the raw setting: the halo has
            // to follow the shape the tile is actually drawn with, and
            // clamp_border() is what decides that.
            const BorderParams b =
                clamp_border(border_width_setting, corner_radius_setting,
                             static_cast<double>(r.width),
                             static_cast<double>(r.height));

            GlowPassParams gp{};
            gp.color_argb    = glow_argb;
            gp.quad_w        = static_cast<float>(q.width);
            gp.quad_h        = static_cast<float>(q.height);
            gp.center_x      = static_cast<float>(q.center_x);
            gp.center_y      = static_cast<float>(q.center_y);
            gp.half_w        = static_cast<float>(q.half_width);
            gp.half_h        = static_cast<float>(q.half_height);
            gp.corner_radius = static_cast<float>(b.radius);
            gp.size          = static_cast<float>(glow_size_px);
            gp.intensity     = glow_intensity;
            gp.falloff       = glow_falloff;

            // break, not continue: every tile opens the same pass on the same
            // technique, so a failure here fails for all of them, and retrying
            // it once per tile would only repeat the same call nine times a
            // frame. The tiles loop below uses continue because each of its
            // passes binds different textures.
            if (!glow_begin_pass(glow, gp)) break;
            gs_matrix_push();
            gs_matrix_translate3f(static_cast<float>(q.x),
                                  static_cast<float>(q.y), 0.0f);
            // No texture: the halo is generated from the distance field alone,
            // and gs_draw_sprite() with a null texture and an explicit size
            // gives the 0..1 UVs the shader needs (gs_draw_quadf(),
            // libobs/graphics/graphics.c:1039-1078). The background fill above
            // draws the same way.
            gs_draw_sprite(nullptr, 0, q.width, q.height);
            gs_matrix_pop();
            gs_technique_end_pass(glow);
        }
        gs_technique_end(glow);
        gs_blend_state_pop();
    }

    // Rounded corners make a tile transparent at its corners, so the background
    // drawn above has to show through them. That needs alpha blending, and
    // inheriting whatever OBS last set is how a bug becomes machine-dependent:
    // correct on the box it was written on, wrong somewhere else. Pushed and
    // popped so the rest of the frame is left exactly as it was found — the
    // stack saves the enable flag, all four factors and the op
    // (gs_blend_state_pop(), libobs/graphics/graphics.c:1265-1282).
    //
    // gs_blend_function() sets the factors but does not enable blending, hence
    // the explicit gs_enable_blending(true) alongside it.
    gs_blend_state_push();
    gs_enable_blending(true);
    // Separate alpha factors, not gs_blend_function(SRCALPHA, INVSRCALPHA) for
    // all four: with the same pair for colour and alpha, destination alpha
    // under-accumulates at partial-alpha pixels (e.g. 0.84 instead of 1.0),
    // visible as a faint translucent ring around rounded corners when the wall
    // is filtered, nested in another scene, or routed through the
    // colour-space-conversion texrender path. ONE/INVSRCALPHA for alpha is
    // what libobs's own gs_reset_blend_state() uses (graphics.c:1289-1295) —
    // match it here instead of "simplifying" back to gs_blend_function().
    gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA,
                                GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    gs_technique_t *tech = s_tiles_effect.tech_i420;
    gs_technique_begin(tech);

    size_t drawn = 0;

    // One tile's draw — video or neutral placeholder — with its top-left at
    // (ox, oy) in whatever render target is currently bound. This is the body
    // the loop below used to have inline, unchanged except that the origin is
    // a parameter instead of r.x/r.y and its three early exits are `return`
    // instead of `continue`.
    //
    // It is a function because the sub-pixel path for a tile in motion has to
    // run EXACTLY this — the same effect, the same technique, the same
    // parity-verified BT.709 conversion, the same cover-crop and crop_uv, the
    // same clamped border geometry — into an intermediate texture at (0, 0). A
    // second copy specialised for that path is the one thing most likely to
    // make a moving tile a different colour from a resting one, which is the
    // acceptance gate for this whole feature. There is no second copy.
    auto draw_tile = [&](size_t i, const SnappedTileRect &r, uint32_t ox,
                         uint32_t oy) {
        // Bounded against this tile, not against the canvas: a width past half
        // the shorter side would leave no interior, and the shader's distance
        // field assumes a radius no larger than that. See zoom-tile-border.h.
        const BorderParams b =
            clamp_border(border_width_setting, corner_radius_setting,
                         static_cast<double>(r.width),
                         static_cast<double>(r.height));
        TilePassParams pass{};
        pass.border_argb   = border_argb;
        pass.border_width  = static_cast<float>(b.width);
        pass.corner_radius = static_cast<float>(b.radius);
        pass.tile_w = static_cast<float>(r.width);
        pass.tile_h = static_cast<float>(r.height);

        // Exactly the compositor's rule for "this tile has something to show":
        // tile_take_snapshot() already folds in TileSlotState's epoch check, so
        // a frame captured under a previous assignment is refused here for the
        // same reason it was refused there. No new rule is invented.
        const TileFeedPtr &feed = feeds[i];
        if (!feed || !tile_take_snapshot(feed, ctx->render_scratches[i]) ||
            !tile_upload_frame(feed, ctx->render_scratches[i])) {
            tiles_draw_neutral(tech, ox, oy, r.width, r.height, pass);
            return;
        }

        // Narrow the usable source by this slot's crop, then sample the largest
        // centred tile-shaped sub-rectangle of what is left, so the tile fills
        // completely and is never letterboxed — the same solve_cover_crop() the
        // CPU blit used, mapped straight onto gs_draw_sprite_subregion(), which
        // samples exactly such a sub-rectangle.
        //
        // params.tile_aspect, deliberately, rather than a second read of the
        // atomic: this is the value the grid above was actually solved from, so
        // the rect being filled and the rect being sampled cannot disagree.
        // A non-16:9 tile fed by a 16:9 camera therefore crops more off the
        // sides — that is what filling a narrower rect means, not a defect.
        //
        // The order is crop-then-cover and it is pinned by
        // tests/tile-crop-test.cpp; see zoom-tile-crop.h. With both crops at 0
        // solve_slot_crop() reduces to solve_cover_crop() exactly, so an
        // operator who never touches the sliders sees no change at all.
        //
        // Bounds-checked rather than assumed: feeds.size() is capped at
        // kMaxTileSlots by the resolver, but the draw path is the wrong place
        // to depend on that holding.
        const std::pair<double, double> slot_crop =
            i < ctx->render_slot_crop.size() ? ctx->render_slot_crop[i]
                                             : std::pair<double, double>{0.0, 0.0};
        const CropRect crop = solve_slot_crop(static_cast<double>(feed->tex_w),
                                              static_cast<double>(feed->tex_h),
                                              params.tile_aspect,
                                              slot_crop.first, slot_crop.second);
        // The sprite's geometry is sub_cx x sub_cy *in whole texels*
        // (build_subsprite_norm(), libobs/graphics/graphics.c:1024), so the
        // scale must divide by the truncated integers actually passed below,
        // not by the un-truncated doubles: dividing by crop.width would leave
        // the tile up to a pixel short of its rect and expose a sliver of the
        // neutral canvas along two edges.
        const uint32_t crop_x = static_cast<uint32_t>(crop.x);
        const uint32_t crop_y = static_cast<uint32_t>(crop.y);
        const uint32_t crop_w = static_cast<uint32_t>(crop.width);
        const uint32_t crop_h = static_cast<uint32_t>(crop.height);
        if (crop_w == 0 || crop_h == 0) {  // degenerate source: nothing to map
            tiles_draw_neutral(tech, ox, oy, r.width, r.height, pass);
            return;
        }

        // The same sub-rectangle, in normalized texture coords, so the shader
        // can undo the crop and recover a 0..1 coordinate across the tile for
        // its distance field. Derived from the truncated integers actually
        // passed to gs_draw_sprite_subregion() below, and from tex_w/tex_h,
        // because those are precisely what build_subsprite_norm() divides by
        // when it builds the quad's UVs — anything else would put the border
        // slightly out of register with the video.
        //
        // This is why neither the slot crop nor the tile shape changes anything
        // here: both move the sub-rectangle, and these four come from that same
        // moved rectangle by construction. Any future change must keep
        // computing crop_uv from the exact integers handed to
        // gs_draw_sprite_subregion(), or borders will silently misregister on
        // every tile. tests/tile-shape-test.cpp reproduces this arithmetic for
        // a 4:3 tile with a non-zero crop and checks the shader's
        // (uv - crop_uv.xy) / crop_uv.zw still lands on 0..1 across the tile.
        pass.crop_u  = static_cast<float>(crop_x) / static_cast<float>(feed->tex_w);
        pass.crop_v  = static_cast<float>(crop_y) / static_cast<float>(feed->tex_h);
        pass.crop_cu = static_cast<float>(crop_w) / static_cast<float>(feed->tex_w);
        pass.crop_cv = static_cast<float>(crop_h) / static_cast<float>(feed->tex_h);

        if (!tiles_begin_pass(tech, feed->tex_y, feed->tex_u, feed->tex_v, pass))
            return;
        gs_matrix_push();
        gs_matrix_translate3f(static_cast<float>(ox), static_cast<float>(oy),
                              0.0f);
        gs_matrix_scale3f(static_cast<float>(r.width) / static_cast<float>(crop_w),
                          static_cast<float>(r.height) / static_cast<float>(crop_h),
                          1.0f);
        // The chroma planes are half-size but sampled with normalized UVs, so
        // the same sub-region is correct for all three without separate maths.
        gs_draw_sprite_subregion(feed->tex_y, 0, crop_x, crop_y, crop_w, crop_h);
        gs_matrix_pop();
        gs_technique_end_pass(tech);
        ++drawn;
    };

    for (size_t i = 0; i < rects.size() && i < feeds.size(); ++i) {
        const SnappedTileRect &r = rects[i];
        if (r.width < 2 || r.height < 2) continue;  // as the CPU path skips them

        // Not moving. That is EVERY tile when the animation toggle is off
        // (`moving` is empty, so this condition is true for every i), every
        // tile on a settled wall, and any tile that happens to be sitting
        // still while others reflow around it. It takes the blit this file has
        // always done — straight to the canvas, at its snapped even-boundary
        // rect, through the same call sequence as before this feature existed.
        if (i >= moving.size() || !moving[i].moving) {
            draw_tile(i, r, r.x, r.y);
            continue;
        }

        // ── A tile in motion ─────────────────────────────────────────────────
        //
        // Composited into an intermediate texture of the tile's own
        // even-rounded size, then drawn at the animator's true fractional
        // position with linear filtering.
        //
        // The even-boundary rule exists because I420 chroma is subsampled 2x2:
        // a blit edge on an odd pixel has no chroma sample to reconstruct from.
        // Inside the intermediate the tile sits at (0, 0) with even dimensions,
        // so that rule is satisfied in TILE space, and where the finished tile
        // then lands on the canvas is free to be fractional. That is the whole
        // trick — without it, motion is quantised to the 2px steps
        // snap_tile_independently() imposes, which is plainly visible on a slow
        // reflow and is exactly the case this feature exists to make look good.
        //
        // Resolved before anything is composited so that a missing default
        // effect takes the fallback path below rather than rendering a tile
        // into a texture nothing can then draw.
        gs_effect_t *const def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
        gs_eparam_t *const def_image =
            def ? gs_effect_get_param_by_name(def, "image") : nullptr;
        gs_texrender_t *const tr = def_image ? ensure_motion_texrender(ctx)
                                             : nullptr;

        // The wall's technique is closed across this whole branch and reopened
        // at the end. It has to be: gs_effect_loop() refuses to run — and logs
        // a warning — while another effect is active (gs_effect_loop(),
        // libobs/graphics/effect.c:69), and the blit below goes through
        // libobs's default effect, not ours. The composite reopens the wall's
        // technique for the tile's own pass inside the texrender.
        // gs_technique_end() also discards the effect's parameter values, which
        // costs nothing here: tiles_begin_pass() sets every one of them before
        // every tile's pass anyway — that is already this file's rule, for the
        // same upload-at-begin_pass reason.
        gs_technique_end(tech);

        bool composited = false;
        if (tr) {
            gs_texrender_reset(tr);
            if (gs_texrender_begin(tr, r.width, r.height)) {
                composited = true;
                // gs_texrender_begin() sets the VIEWPORT but not the
                // projection — it pushes the old one and leaves it in place
                // (libobs/graphics/texture-render.c), so without this the
                // tile would be drawn through the CANVAS's ortho into a
                // tile-sized viewport and come out shrunk into a corner.
                // Every caller in libobs sets its own; obs-scene.c:935 is the
                // same call with the same -100..100 depth range. The projection
                // is popped by gs_texrender_end().
                //
                // This particular ortho is also what makes the composite
                // pixel-exact against the direct path: 0..width mapped across a
                // width-wide viewport puts each destination pixel centre at the
                // same tile-relative coordinate the canvas ortho gives it when
                // the tile is drawn at an integer origin, so the shader is
                // evaluated at identical UVs and produces identical bytes.
                gs_ortho(0.0f, static_cast<float>(r.width), 0.0f,
                         static_cast<float>(r.height), -100.0f, 100.0f);
                // Blending OFF while compositing. The intermediate has to hold
                // exactly what the shader produced, with STRAIGHT (not
                // premultiplied) alpha, because the blit below composites it
                // with the same SRCALPHA/INVSRCALPHA factors the direct path
                // uses — the ones pushed above this loop, still in force.
                // Leaving blending enabled here would fold alpha into the
                // colour a first time against the cleared target and a second
                // time at the blit, darkening every rounded corner.
                gs_blend_state_push();
                gs_enable_blending(false);
                // The tile covers the whole intermediate, so this only matters
                // when a pass fails to open: cleared, that tile draws nothing
                // (which is what the direct path does on the same failure)
                // rather than blitting whatever the texture last held.
                struct vec4 transparent;
                vec4_zero(&transparent);
                gs_clear(GS_CLEAR_COLOR, &transparent, 0.0f, 0);

                gs_technique_begin(tech);
                draw_tile(i, r, 0, 0);
                gs_technique_end(tech);

                // Honour the animator's own alpha. Skipped at 1.0, which is
                // every tile that reaches here today — see fade_tile_alpha().
                if (moving[i].alpha < 1.0)
                    fade_tile_alpha(r.width, r.height, moving[i].alpha);

                gs_blend_state_pop();
                gs_texrender_end(tr);

                // Never null after a successful begin: gs_texrender_begin()
                // returns false when its target could not be created
                // (libobs/graphics/texture-render.c). Checked anyway, on this
                // file's existing convention of not depending on that from the
                // draw path.
                gs_texture_t *const tile_tex = gs_texrender_get_texture(tr);
                if (tile_tex) {
                    // Sample the way the ambient framebuffer state writes, so
                    // the round trip is an identity rather than a colour
                    // change. With sRGB framebuffer writes off — which is the
                    // state a source renders in (obs-video.c enables it only
                    // around libobs's own blits) — both the intermediate and
                    // the canvas store raw shader output, so a raw read is
                    // exact. With them on, both stores are sRGB-encoded and the
                    // read has to decode, or the value would be encoded twice
                    // and the tile would visibly wash out. One line so this
                    // holds either way instead of on one of them by luck.
                    if (gs_framebuffer_srgb_enabled())
                        gs_effect_set_texture_srgb(def_image, tile_tex);
                    else
                        gs_effect_set_texture(def_image, tile_tex);

                    gs_matrix_push();
                    // The fractional position, at last — the point of all of
                    // the above. The default effect's sampler is Linear/Clamp
                    // (libobs/data/default.effect), so the sub-pixel offset is
                    // resolved by bilinear filtering inside the tile, where
                    // chroma has already been reconstructed.
                    gs_matrix_translate3f(static_cast<float>(moving[i].x),
                                          static_cast<float>(moving[i].y), 0.0f);
                    // ...and the fractional SIZE. The intermediate had to be
                    // even-dimensioned, but the quad it is blitted onto does
                    // not: scaling the sprite from the even size onto the
                    // animator's true size puts the trailing edge at exactly
                    // x + width instead of x + even_floor(width). Without it
                    // the leading edge of a moving tile travels smoothly while
                    // the trailing edge steps 2px — the same artifact class
                    // this task exists to remove, now on one edge only, which
                    // reads worse than the uniform stepping it replaced.
                    //
                    // The factor is within ~0.2% of 1.0 (the even-rounding
                    // discards under 2px of a tile hundreds of pixels wide), so
                    // the resampling it adds is far below the fractional-offset
                    // filtering already happening on the same draw. Anchored at
                    // the tile's top-left, which the translate above has
                    // already put at the animator's exact position, so growth
                    // happens away from a corner that is already correct.
                    //
                    // r.width/r.height are at least 2 here — the loop skips
                    // anything smaller before this branch — so neither divisor
                    // can be zero.
                    gs_matrix_scale3f(
                        static_cast<float>(moving[i].width) /
                            static_cast<float>(r.width),
                        static_cast<float>(moving[i].height) /
                            static_cast<float>(r.height),
                        1.0f);
                    while (gs_effect_loop(def, "Draw"))
                        gs_draw_sprite(tile_tex, 0, r.width, r.height);
                    gs_matrix_pop();
                }
            } else if (!s_motion_render_failed_logged) {
                s_motion_render_failed_logged = true;
                blog(LOG_ERROR,
                     "[obs-zoom-plugin] Tiles: could not create a %ux%u "
                     "intermediate texture for a tile in motion; tiles will "
                     "step on the 2px chroma grid instead of moving smoothly",
                     r.width, r.height);
            }
        }

        gs_technique_begin(tech);
        // Whatever failed — no default effect, no texrender, or a render
        // target that could not be created at this size — the tile still has
        // to appear. Falling back to the snapped draw costs it sub-pixel
        // smoothness for this frame and nothing else, which is why this is a
        // fallback and not a skip.
        if (!composited) draw_tile(i, r, r.x, r.y);
    }

    gs_technique_end(tech);
    gs_blend_state_pop();

    // Rate-limited so the rig can confirm the draw path is live and see both
    // the grid it solved and how many tiles actually have a feed, without
    // flooding the log at the frame rate.
    if (ctx->rendered_frames == 0 || ctx->rendered_frames % 300 == 0) {
        blog(LOG_INFO,
             "[obs-zoom-plugin] Tiles render: canvas=%ux%u aspect=%.4f "
             "gutter=%.2fpx margin=%.2fpx tiles=%zu with_video=%zu count=%llu",
             canvas_w, canvas_h, params.tile_aspect, params.gutter,
             params.margin, rects.size(), drawn,
             static_cast<unsigned long long>(ctx->rendered_frames));
    }
    ++ctx->rendered_frames;
}

// ── OBS source callbacks ─────────────────────────────────────────────────────

static const char *tiles_source_get_name(void *)
{
    return "CoreVideo Tiles";
}

// Property keys. Tile and exclude slots are numbered from 1 to match their
// labels, so scene files stay readable. Declared here rather than beside the
// properties builder because tiles_source_update reads the same keys.
static constexpr const char *PROP_FILL_MODE = "fill_mode";
static constexpr const char *PROP_MAX_TILES = "max_tiles";
static constexpr const char *PROP_BG_SOURCE = "bg_source";
static constexpr const char *PROP_BORDER_WIDTH  = "border_width";
static constexpr const char *PROP_BORDER_COLOR  = "border_color";
static constexpr const char *PROP_BORDER_SHAPE  = "border_shape";
static constexpr const char *PROP_CORNER_RADIUS = "corner_radius";
// Outer glow. Size 0 is off, which is the default, so the other two are inert
// until the operator moves it.
static constexpr const char *PROP_GLOW_SIZE      = "glow_size";
static constexpr const char *PROP_GLOW_COLOR     = "glow_color";
static constexpr const char *PROP_GLOW_INTENSITY = "glow_intensity";
static constexpr const char *PROP_GLOW_SOFTNESS  = "glow_softness";
// Layout animation: whether tile moves/joins/departures ease instead of
// snapping, and how long that eases over. See PROP_ANIMATE default below —
// off is a complete bypass, not a fast setting.
static constexpr const char *PROP_ANIMATE    = "animate_layout";
static constexpr const char *PROP_ANIMATE_MS = "animate_duration_ms";
// Tile shape: a preset, plus the ratio the Custom entry reveals. Two keys
// rather than one so switching to a preset and back keeps the ratio the
// operator typed, exactly as the corner radius survives a switch to Square.
static constexpr const char *PROP_TILE_SHAPE  = "tile_shape";
static constexpr const char *PROP_TILE_RATIO  = "tile_ratio";
// Wall spacing, each a percentage of canvas height.
static constexpr const char *PROP_GUTTER_PCT  = "gutter_pct";
static constexpr const char *PROP_MARGIN_PCT  = "margin_pct";
// The collapsible group the eighteen crop sliders live in. Named so a scene
// file can be read, though the group itself stores no value of its own.
static constexpr const char *PROP_CROP_GROUP = "crop_group";
// Naming a group is both the destination and the on-switch: empty means the
// feature creates nothing. It ships empty, because this feature writes to
// the operator's scene collection and must not start doing that on upgrade for
// someone who never asked for it. Clearing it once it has been set is a
// distinct event — the operator switching the feature off — and owes one final
// reconcile that mutes what this wall owns; see audio_off_pending.
static constexpr const char *PROP_AUDIO_GROUP = "audio_group";
// kMaxTileSlots is declared with the other draw-path constants at the top of
// this file, because `tiles_source` sizes its crop array from it.
static constexpr std::size_t kMaxExcludes   = 3;

// Corner shape, stored as an int in the scene file so the list is stable if
// more shapes are ever added.
enum class TileBorderShape : long long { Square = 0, Rounded = 1 };

static std::string tile_prop_name(std::size_t slot)
{
    return "tile_" + std::to_string(slot);
}

static std::string exclude_prop_name(std::size_t slot)
{
    return "exclude_" + std::to_string(slot);
}

// One key per side per slot, numbered from 1 like the tile choosers so a scene
// file reads the same way. Shared by the defaults, the properties builder and
// tiles_source_update, so the three cannot drift apart.
static std::string crop_left_prop_name(std::size_t slot)
{
    return "crop_left_" + std::to_string(slot);
}

static std::string crop_right_prop_name(std::size_t slot)
{
    return "crop_right_" + std::to_string(slot);
}

// Logs, once per occurrence, that a second Tiles source has nominated a
// participant audio group.
//
// Multi-wall audio is not supported, and this is the honest way to say so
// rather than the expensive way to fix it. Audio for a participant is owned by
// exactly one Tiles source (see the design's "one owner per participant"), and
// two consequences fall out of that the moment a second wall names a group.
// Wall B owns P; P drops off B's wall, so B mutes P; P is still on wall A's
// wall, but A hits the "owned by another live Tiles source" guard and emits
// nothing at all — so P is muted while on screen and no reconcile will ever
// unmute them. And stem tracks are allocated per wall from track 2 up, so
// A's first tile and B's first tile both claim track 2 and that ISO stem
// records two people mixed together. Both are silent failures; a log line at
// least makes them findable.
//
// Read from each Tiles source's settings rather than from its ctx->audio_group
// on purpose: this runs inside an obs_enum_sources callback, which holds
// obs->data.sources_mutex, and taking a ctx->mutex there — a lock the graphics
// thread takes every frame — would add a fresh lock-order edge for a value the
// settings already hold authoritatively.
static void warn_on_multiple_audio_walls()
{
    int walls = 0;
    obs_enum_sources(
        [](void *param, obs_source_t *src) -> bool {
            const char *id = obs_source_get_id(src);
            if (!id || std::strcmp(id, kTilesSourceId) != 0) return true;
            obs_data_t *settings = obs_source_get_settings(src);
            if (!settings) return true;
            const char *group = obs_data_get_string(settings, PROP_AUDIO_GROUP);
            if (group && *group) ++*static_cast<int *>(param);
            obs_data_release(settings);
            return true;
        },
        &walls);

    // Latched: a standing misconfiguration is logged once, not on every roster
    // tick, and the latch re-arms when it clears so setting the same thing up
    // again tomorrow logs again.
    static std::atomic<bool> s_warned_multi_wall{false};
    if (walls < 2) {
        s_warned_multi_wall.store(false, std::memory_order_release);
        return;
    }
    if (s_warned_multi_wall.exchange(true, std::memory_order_acq_rel)) return;

    blog(LOG_WARNING,
         "[obs-zoom-plugin] Tiles audio: %d Tiles sources have a participant "
         "audio group set. Only one is supported. A participant's audio "
         "belongs to a single wall, so someone who leaves that wall while "
         "still on another one is muted and will not be unmuted again, and "
         "both walls assign ISO stems from track 2 upwards, so two people can "
         "end up recorded on the same stem. Set the audio group on one Tiles "
         "source and clear it on the others.",
         walls);
}

// Requests a per-participant audio reconcile; performs none of the work
// itself — the actual scan-plan-apply happens in the obs_queue_task body
// below, at some point that is not necessarily "later".
//
// obs_queue_task(OBS_TASK_UI, ..., wait=false) is NOT reliably deferred.
// libobs's OBS_TASK_UI handler is QMetaObject::invokeMethod(App(), "Exec",
// Qt::AutoConnection, ...) (frontend/OBSApp.cpp:1149-1155), and
// Qt::AutoConnection resolves to a direct, synchronous call whenever the
// calling thread already IS the receiver's thread — i.e. whenever this is
// called from the UI thread, the lambda below runs INLINE, before
// obs_queue_task returns, not queued at all. It is only genuinely deferred
// when called from a non-UI thread (the roster callback on the IPC reader
// thread, notably), where AutoConnection resolves to QueuedConnection.
//
// A scene-collection load runs on the UI thread. So the naive version of
// this function — queue unconditionally, trust the queue to defer — reconciles
// audio INLINE, mid-load, for exactly the caller that matters most: the
// settings pass tiles_source_create makes for a restored Tiles source with a
// saved, non-empty audio_group. That scans a collection libobs is still in
// the middle of creating, finds no clash for a participant whose saved audio
// source has not been created yet in file order, and creates a duplicate —
// which libobs then also creates from the save data moments later. Doubled
// voice. This is why s_collection_loading exists: while it is true, this
// function does nothing at all, deferring the intent (not the request) to
// zoom_supersource_set_collection_loading, which sweeps every live Tiles
// source once loading is confirmed finished — see that function.
//
// The gate is checked twice, here and again at the top of the task body,
// because these are two different windows. This check covers a request made
// while a load is in progress. The one in the body covers a task that was
// queued from the IPC reader thread while the gate was still clear and is
// still sitting in Qt's queue when a teardown begins — ClearSceneData pumps
// that queue (frontend/widgets/OBSBasic_SceneCollections.cpp:1550-1559) and
// would otherwise run it against half-destroyed sources.
//
// Coalesced via audio_reconcile_pending, for the inline-execution case too:
// a talking change alone fires both an active_speaker line and a full
// roster update (engine/src/main.cpp's onUserActiveAudioChange), several
// times a second in ordinary conversation, each of which calls
// apply_assignments. Without coalescing that is a full obs_enum_sources
// scan-plan-apply, behind a process-wide lock, per event. The flag collapses
// any burst into at most one task in flight; it is cleared at the very
// start of the task (not at the end), so a change that arrives while the
// task is already running (or, inline, while a nested call is impossible
// because this thread IS the one running it) queues a fresh one instead of
// being silently absorbed.
//
// Lifetime: obs_source_get_ref keeps the underlying source alive until the
// task releases it — and with it, ctx, which the source owns for its entire
// lifetime (tiles_source_destroy only runs, and only frees ctx, once every
// reference including this one is gone). So ctx cannot be freed out from
// under a queued or running task, inline or deferred. get_ref returns
// nullptr if the source is already being destroyed; treated here as
// "nothing to reconcile, and nothing will ever run to clear the flag" —
// safe, because ctx is being freed by that same teardown regardless, and
// nothing else ever reads audio_reconcile_pending after that point.
static void request_audio_reconcile(tiles_source *ctx)
{
    // See s_collection_loading's own comment: skipped entirely, not merely
    // deferred, and nothing is lost — zoom_supersource_set_collection_
    // loading(false) requests a reconcile on every live Tiles source once
    // loading is confirmed finished, independent of what happened here
    // while it was true. audio_reconcile_pending is deliberately left
    // untouched on this path: setting it here with no task queued to clear
    // it would wedge every later call into believing one is already in
    // flight when none is.
    if (s_collection_loading.load(std::memory_order_acquire)) return;

    // Cheap bail before touching the pending flag or queuing anything: the
    // common case is the feature being off, and there is no reason to pay
    // for a ref, an allocation and a queued task just to have the task find
    // nothing to do.
    //
    // The one no-group case that does have work to do is the transition into
    // it — the operator clearing the field, which owes exactly one reconcile
    // that mutes everything this wall owns. If a reconcile is already pending
    // when that happens, the compare-exchange below fails and this returns
    // without queuing a second one; that is correct and not a lost signal,
    // because the task body re-reads both audio_group and audio_off_pending
    // when it runs, so the reconcile already in flight performs the off sweep.
    bool has_group = false;
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        has_group = !ctx->audio_group.empty();
    }
    if (!has_group && !ctx->audio_off_pending.load(std::memory_order_acquire))
        return;

    bool expected = false;
    if (!ctx->audio_reconcile_pending.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
        return;  // already queued/running/inline-executing — rides along

    // ref and ctx->source are the same object, so the queued task below
    // releases it via ctx->source directly rather than carrying a second
    // copy of the pointer through as a separate payload; ref itself exists
    // only for the null check.
    obs_source_t *ref = obs_source_get_ref(ctx->source);
    if (!ref) {
        ctx->audio_reconcile_pending.store(false, std::memory_order_release);
        return;
    }

    obs_queue_task(OBS_TASK_UI, [](void *param) {
        auto *ctx = static_cast<tiles_source *>(param);

        // Cleared first, not last: a change arriving while the reconcile
        // below is running (whether that running is inline, on this same
        // call stack, or a genuinely later queued task) must queue a fresh
        // one rather than be coalesced into this one and then lost.
        //
        // Cleared before the gate re-check below, too, and deliberately: if
        // this task bails on a closed gate while leaving the flag set,
        // nothing would ever clear it — the gate-clear sweep's own
        // compare_exchange would fail forever and this ctx would never
        // reconcile again. Clearing first means the sweep re-requests
        // normally, which is exactly what it exists to do.
        ctx->audio_reconcile_pending.store(false, std::memory_order_release);

        // The gate can close between this task being queued and it running.
        // A task queued from the IPC reader thread is genuinely deferred, so
        // it sits in Qt's queue; if a collection teardown starts in the
        // meantime, ClearSceneData's pumps (frontend/widgets/
        // OBSBasic_SceneCollections.cpp:1550-1559, and LoadData:1161) will
        // run it mid-teardown, against sources that are flagged removed but
        // that neither obs_enum_sources (libobs/obs.c:1837) nor
        // obs_get_source_by_name (:2038) filters out. A Create there can
        // succeed into a dying group, land in ClearSceneData's orphan sweep,
        // set clearingFailed and close OBS. Nothing is lost by bailing:
        // zoom_supersource_set_collection_loading(false) sweeps every live
        // Tiles source once the load is confirmed finished.
        if (s_collection_loading.load(std::memory_order_acquire)) {
            obs_source_release(ctx->source);
            return;
        }

        std::string audio_group;
        std::vector<uint32_t> assignments;
        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            audio_group = ctx->audio_group;
            assignments = ctx->participants;
        }

        // Read after the load gate above, not before it: a task that bails on
        // a closed gate must leave the flag set, so the post-load sweep still
        // performs the off reconcile it owes the operator.
        const bool off_sweep =
            ctx->audio_off_pending.load(std::memory_order_acquire);

        // No group and no pending off means the feature has been off all
        // along: nothing to reconcile, and nothing of the operator's to touch.
        if (audio_group.empty() && !off_sweep) {
            obs_source_release(ctx->source);
            return;
        }

        const char *uuid = obs_source_get_uuid(ctx->source);
        if (uuid && *uuid) {
            const std::vector<ParticipantInfo> roster =
                ZoomEngineClient::instance().roster();
            TilesAudioPlanParams params;
            params.self_uuid = uuid;
            params.enabled   = true;

            // Cleared before the reconcile rather than after, for the same
            // reason audio_reconcile_pending is cleared at task entry: a
            // clear-the-field that somehow lands while this is running must
            // leave a request behind rather than be absorbed. Cleared on the
            // group-named path too — a group named again after being cleared
            // supersedes the off sweep entirely, and a stale flag left set
            // there would mute the whole wall on some later tick.
            ctx->audio_off_pending.store(false, std::memory_order_release);

            // Cheap enough beside a full scan-plan-apply, and this is the only
            // place that sees every wall at once. Called on the off path too,
            // so the latch re-arms as soon as the second wall stands down.
            warn_on_multiple_audio_walls();

            // audio_group is empty on the off path, which tiles_audio_reconcile
            // reads as "mute everything this wall owns, create nothing".
            tiles_audio_reconcile(assignments, roster, params, audio_group);
        }

        obs_source_release(ctx->source);
    }, ctx, false);
}

// Recomputes the wall from the cached settings plus the live roster, and
// performs whatever engine work the change implies. Safe to call from the
// settings path and from the roster callback.
//
// Audio reconciliation is always requested here, independently of whether
// the tile assignments changed below: naming a group on an otherwise-stable
// wall, or switching from one group to another, changes no participant
// assignment at all but still has to reconcile audio, so it is not behind
// the "did the wall change" early exit the feed plan uses.
static void apply_assignments(tiles_source *ctx)
{
    {
        // engine_mutex opens *before* roster() and closes after
        // execute_feed_plan, so the feed path's read-compute-apply is atomic
        // per ctx. It has to be: two callers race here for the same ctx (the
        // settings path on the UI thread, and the roster callback on the IPC
        // reader thread), and if the roster snapshot were taken outside the
        // guard, the caller holding the *staler* roster could win the lock
        // second and write its stale tile assignments over the fresher ones,
        // where they would stick until the next roster tick moved a tile.
        // The audio request below is deliberately outside — see there.
        std::lock_guard<std::mutex> engine_lock(ctx->engine_mutex);

        // Fetched before ctx->mutex: roster() takes the engine client's lock,
        // and taking them in the other order anywhere would invite a
        // deadlock. (engine_mutex → engine client lock is the only order this
        // codebase ever uses; the client releases its own lock before
        // dispatching the roster callbacks that re-enter here.)
        const std::vector<ParticipantInfo> roster =
            ZoomEngineClient::instance().roster();

        FeedPlan plan;
        bool     feed_changed = false;
        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            std::vector<uint32_t> next =
                resolve_tile_assignments(ctx->participants, roster, ctx->fill_params);
            feed_changed = !(ctx->participants == next);
            if (feed_changed) {
                ctx->participants.swap(next);
                plan = plan_feeds_locked(ctx);
            }
        }
        if (feed_changed) execute_feed_plan(plan);
    }

    // Outside engine_mutex on purpose. Once the gate is open, this call runs
    // the whole scan-plan-apply inline whenever the caller is the UI thread,
    // and holding engine_mutex across that would block a concurrent
    // reader-thread apply_assignments for this same ctx for the full
    // duration — the reader-thread stall this feature must not introduce.
    //
    // Safe to drop the guard here because the audio half does not depend on
    // the roster snapshot taken above: the queued task re-reads assignments
    // and the roster itself, and every reconcile body runs on the UI thread
    // (obs_queue_task(OBS_TASK_UI, ...)) with g_reconcile_mutex held across
    // scan-and-apply, which is what actually serialises reconciles — not the
    // audio_reconcile_pending compare-exchange, which only coalesces and is
    // cleared at task entry rather than at task exit.
    request_audio_reconcile(ctx);
}

static void tiles_source_update(void *data, obs_data_t *settings)
{
    auto *ctx = static_cast<tiles_source *>(data);

    TileFillParams params;
    params.mode = obs_data_get_int(settings, PROP_FILL_MODE) ==
                          static_cast<long long>(TileFillMode::Manual)
                      ? TileFillMode::Manual
                      : TileFillMode::Auto;

    // Clamp: scene files are hand-editable and obs_data_get_int returns int64.
    const int64_t raw_max = obs_data_get_int(settings, PROP_MAX_TILES);
    params.max_tiles = static_cast<std::size_t>(
        std::min<int64_t>(std::max<int64_t>(raw_max, 1),
                          static_cast<int64_t>(kMaxTileSlots)));

    // Max Tiles is an Auto-only control, and the properties dialog hides it in
    // Manual mode — so a value the operator lowered while in Auto must not go
    // on quietly capping a Manual wall from behind a hidden control. In Manual,
    // casting a tile is an explicit operator decision and the software does not
    // override it; the only bound is the nine physical tile slots. Same
    // principle that already keeps a cast participant whose camera is off.
    //
    // Deliberately applied here rather than in resolve_tile_assignments: the
    // resolver stays a pure function with one uniform rule, and this
    // mode-specific policy lives beside the UI that owns the control. That is
    // why the assignment below looks redundant with the clamp above.
    if (params.mode == TileFillMode::Manual) params.max_tiles = kMaxTileSlots;

    // A chooser value outside the 32-bit Zoom id range cannot name a real
    // participant, so it is dropped rather than wrapped by the cast.
    const auto read_id = [settings](const std::string &key) -> uint32_t {
        const int64_t raw = obs_data_get_int(settings, key.c_str());
        if (raw <= 0 || raw > 0xFFFFFFFFll) return 0;
        return static_cast<uint32_t>(raw);
    };

    for (std::size_t i = 1; i <= kMaxExcludes; ++i)
        params.excluded.push_back(read_id(exclude_prop_name(i)));
    for (std::size_t i = 1; i <= kMaxTileSlots; ++i)
        params.manual.push_back(read_id(tile_prop_name(i)));

    // Per-slot crop, read here and stored under ctx->mutex below with the fill
    // params, so one settings pass lands on the draw path as a unit.
    //
    // Clamped for the same reason the canvas and border settings above are: the
    // sliders bound these to 0..45, but obs_data_get_int returns int64 and a
    // scene file is hand-editable. solve_slot_crop() clamps again against the
    // frame — that is the backstop, this is the bound on the setting itself.
    const auto read_crop_pct = [settings](const std::string &key) -> double {
        const int64_t raw = obs_data_get_int(settings, key.c_str());
        return static_cast<double>(
            std::min<int64_t>(std::max<int64_t>(raw, 0), kMaxSlotCropPct));
    };
    std::array<std::pair<double, double>, kMaxTileSlots> crops{};
    for (std::size_t i = 1; i <= kMaxTileSlots; ++i) {
        crops[i - 1] = {read_crop_pct(crop_left_prop_name(i)),
                        read_crop_pct(crop_right_prop_name(i))};
    }

    // Clamp before the parity mask. The properties UI already bounds these, but
    // scene files are hand-editable and obs_data_get_int returns int64: an
    // absurd width would otherwise reach resize() as a terabyte allocation and
    // abort OBS from a non-main thread.
    const int64_t raw_w = obs_data_get_int(settings, "canvas_width");
    const int64_t raw_h = obs_data_get_int(settings, "canvas_height");
    const uint32_t width = static_cast<uint32_t>(
        std::min<int64_t>(std::max<int64_t>(raw_w, kMinCanvasW), kMaxCanvasW)) & ~1u;
    const uint32_t height = static_cast<uint32_t>(
        std::min<int64_t>(std::max<int64_t>(raw_h, kMinCanvasH), kMaxCanvasH)) & ~1u;
    ctx->canvas_width.store(width, std::memory_order_release);
    ctx->canvas_height.store(height, std::memory_order_release);
    ctx->bg_color.store(static_cast<uint32_t>(obs_data_get_int(settings, "bg_color")),
                        std::memory_order_release);

    // Same clamp discipline as the canvas above, and for the same reason: the
    // sliders bound these but a hand-edited scene file does not. clamp_border()
    // bounds them again against each tile's own rect at draw time — this only
    // keeps an absurd setting out of the atomics.
    const int64_t raw_border_w = obs_data_get_int(settings, PROP_BORDER_WIDTH);
    const int64_t raw_radius   = obs_data_get_int(settings, PROP_CORNER_RADIUS);
    ctx->border_width.store(
        static_cast<uint32_t>(std::min<int64_t>(
            std::max<int64_t>(raw_border_w, 0), kMaxBorderWidth)),
        std::memory_order_release);
    ctx->corner_radius.store(
        static_cast<uint32_t>(std::min<int64_t>(
            std::max<int64_t>(raw_radius, 0), kMaxCornerRadius)),
        std::memory_order_release);
    ctx->border_color.store(
        static_cast<uint32_t>(obs_data_get_int(settings, PROP_BORDER_COLOR)),
        std::memory_order_release);
    // Anything that is not exactly "Rounded" is Square, so an unknown value
    // from a newer scene file degrades to the shape that existed first rather
    // than to an undefined one.
    ctx->border_rounded.store(
        obs_data_get_int(settings, PROP_BORDER_SHAPE) ==
            static_cast<long long>(TileBorderShape::Rounded),
        std::memory_order_release);

    // The outer glow, on the same clamp discipline. The size is NOT bounded
    // against the gutter or the margin: a halo wider than half the gutter
    // merges with its neighbour and one wider than the margin clips at the
    // canvas edge, and both are the operator's number rendered honestly rather
    // than silently overridden. The only bound is the slider's own, applied
    // here so a hand-edited scene file cannot ask for a quad the size of a
    // stadium. solve_glow_quad() clamps the resulting rect to the canvas.
    const int64_t raw_glow_size = obs_data_get_int(settings, PROP_GLOW_SIZE);
    const int64_t raw_glow_pct  = obs_data_get_int(settings, PROP_GLOW_INTENSITY);
    ctx->glow_size.store(
        static_cast<uint32_t>(std::min<int64_t>(
            std::max<int64_t>(raw_glow_size, 0), kMaxGlowSize)),
        std::memory_order_release);
    ctx->glow_intensity.store(
        static_cast<uint32_t>(std::min<int64_t>(
            std::max<int64_t>(raw_glow_pct, 0), kMaxGlowIntensity)),
        std::memory_order_release);
    ctx->glow_color.store(
        static_cast<uint32_t>(obs_data_get_int(settings, PROP_GLOW_COLOR)),
        std::memory_order_release);
    // The softness clamp is load-bearing rather than defensive: the shader's
    // curve stops being monotone above k = 2, so a hand-edited scene file
    // asking for 300% would brighten the halo just outside every tile and draw
    // a ring. Clamped here so the shader is only ever handed a k it is valid
    // for. See kMaxGlowSoftness.
    const int64_t raw_glow_soft = obs_data_get_int(settings, PROP_GLOW_SOFTNESS);
    ctx->glow_softness.store(
        static_cast<uint32_t>(std::min<int64_t>(
            std::max<int64_t>(raw_glow_soft, 0), kMaxGlowSoftness)),
        std::memory_order_release);

    ctx->animate.store(obs_data_get_bool(settings, PROP_ANIMATE),
                       std::memory_order_release);
    ctx->animate_ms.store(
        static_cast<uint32_t>(obs_data_get_int(settings, PROP_ANIMATE_MS)),
        std::memory_order_release);

    // Tile shape and spacing. resolve_tile_aspect() already refuses a ratio of
    // zero or less (and a NaN) and falls back to 16:9; the clamp here is the
    // same bound the slider carries, applied to scene data too because
    // obs_data_get_double will hand over anything a text editor put there.
    // A ratio outside the range is clamped rather than dropped: an operator who
    // typed 40 wanted "as wide as possible", and the widest the control offers
    // is a truer reading of that than silently reverting them to 16:9.
    const double raw_ratio = obs_data_get_double(settings, PROP_TILE_RATIO);
    const double bounded_ratio =
        (raw_ratio > 0.0)  // false for NaN, which resolve_tile_aspect handles
            ? std::min(std::max(raw_ratio, kMinCustomAspect), kMaxCustomAspect)
            : raw_ratio;
    ctx->tile_aspect.store(
        resolve_tile_aspect(obs_data_get_int(settings, PROP_TILE_SHAPE),
                            bounded_ratio),
        std::memory_order_release);

    // Percentages of canvas height, not pixels: spacing is structural and
    // scales with the canvas, deliberately unlike the border width and corner
    // radius above, which are absolute pixels because they are a drawn line
    // whose weight the operator is choosing directly. resolve_spacing_px()
    // bounds the result against the canvas as well; this bounds the setting.
    const auto read_spacing_pct = [settings](const char *key) -> double {
        const double raw = obs_data_get_double(settings, key);
        if (!(raw > 0.0)) return 0.0;  // negative or NaN: no spacing at all
        return std::min(raw, kMaxSpacingPct);
    };
    ctx->gutter_pct.store(read_spacing_pct(PROP_GUTTER_PCT),
                          std::memory_order_release);
    ctx->margin_pct.store(read_spacing_pct(PROP_MARGIN_PCT),
                          std::memory_order_release);

    const char *audio_group_raw = obs_data_get_string(settings, PROP_AUDIO_GROUP);
    const std::string audio_group = audio_group_raw ? audio_group_raw : "";

    // Deliberately outside every lock this function takes: set_source calls
    // into libobs (obs_source_add_active_child walks the source tree,
    // inc/dec_showing fire signals), and ctx->mutex is taken by the graphics
    // thread on every frame. A refused or unresolved selection is logged
    // inside set_source and simply leaves the previous background in place.
    //
    // A cycle is refused permanently, so the setting that named it is dropped
    // rather than kept: left in place it would show a background in the
    // dropdown that is not in effect and cannot ever be, be saved into the
    // scene, and make every subsequent update re-log the same refusal.
    // NotFound is deliberately not treated this way — a name that has not
    // resolved yet is the normal state during a scene load, and clearing it
    // would erase the operator's setting on every restart (tiles_source_load
    // below is the retry).
    if (ctx->background.set_source(
            ctx->source, obs_data_get_string(settings, PROP_BG_SOURCE)) ==
        TilesBackgroundResult::Refused) {
        obs_data_set_string(settings, PROP_BG_SOURCE, "");
    }

    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->fill_params = std::move(params);
        ctx->slot_crop = crops;
        // Clearing a group that had a value is an instruction — turn the
        // feature off — and not merely the absence of one. Detected here,
        // under the same lock that publishes the new value, so that the
        // transition cannot be lost between a read and a write if two
        // settings passes land back to back. The flag survives independently
        // of whether the request below actually queues anything (the
        // collection-load gate may swallow it), and the reconcile that
        // eventually acts on it clears it. See audio_off_pending.
        if (!ctx->audio_group.empty() && audio_group.empty())
            ctx->audio_off_pending.store(true, std::memory_order_release);
        ctx->audio_group = audio_group;
    }
    apply_assignments(ctx);
}

static void *tiles_source_create(obs_data_t *settings, obs_source_t *source)
{
    auto *ctx = new tiles_source();
    ctx->source = source;
    ctx->instance_id =
        s_tiles_instance_counter.fetch_add(1, std::memory_order_relaxed) +
        os_gettime_ns();
    tiles_source_update(ctx, settings);
    ZoomEngineClient::instance().add_roster_callback(ctx,
        [ctx, gate = ctx->gate]() {
            std::lock_guard<std::mutex> callback_lock(gate->mtx);
            if (!gate->alive) return;
            // Someone joined, left, or toggled their camera: in Auto mode the
            // wall's membership may have changed. Reflow first, then retry any
            // slot that is still silent under its current assignment.
            //
            // This callback runs *synchronously* on the IPC reader thread:
            // ZoomEngineClient::update_roster_state_and_notify calls
            // registered roster callbacks directly from handle_event/
            // reader_loop, with no queueing. apply_assignments's feed-plan
            // half is fine here — it is the existing, established pattern —
            // but this is exactly why its audio half only ever *requests* a
            // reconcile (see request_audio_reconcile) instead of doing one:
            // this thread must never scan or mutate the scene collection
            // directly, and must never be blocked behind a lock another
            // Tiles source's reconcile is holding.
            apply_assignments(ctx);
            resubscribe_silent_feeds(ctx);
        });
    return ctx;
}

// Restoring a scene collection creates every source before it loads any of
// them (obs_load_sources, libobs/obs.c:2406-2427), so when tiles_source_update
// runs from create, the background named in the saved settings usually does
// not exist yet and set_source rightly refuses it. This second pass runs after
// the whole collection exists, which is what makes a saved background come
// back on restart rather than only after the operator reopens Properties.
//
// Per-participant audio needs no equivalent second pass here, for a
// different reason than it looks like it should need one. Audio is always
// requested (never performed) from apply_assignments, and
// request_audio_reconcile does nothing at all — no task queued, inline or
// deferred — while s_collection_loading is true, which it always is for the
// entire duration of a scene-collection load: the gate is closed from
// ClearSceneData's OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP, which every
// load path emits before it touches anything (obs_queue_task(OBS_TASK_UI,
// ...) is NOT reliably deferred when the caller is already the UI thread,
// which a load runs on — see request_audio_reconcile). The settings pass
// tiles_source_create makes above already sent that (suppressed) request.
// The actual reconcile for every restored Tiles source, including this one,
// runs later: zoom_supersource_set_collection_loading(false) sweeps every
// live Tiles source once OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED or
// _FINISHED_LOADING confirms the whole batch — every group's own load pass
// included — has actually finished. Calling request_audio_reconcile again
// from here would be harmless (s_collection_loading is still true whenever
// this runs) but redundant.
static void tiles_source_load(void *data, obs_data_t *settings)
{
    auto *ctx = static_cast<tiles_source *>(data);
    // Same refusal handling as tiles_source_update: a saved scene whose
    // background names a cycle (hand-edited, or saved before this guard
    // existed) drops the setting instead of carrying it forward.
    if (ctx->background.set_source(
            ctx->source, obs_data_get_string(settings, PROP_BG_SOURCE)) ==
        TilesBackgroundResult::Refused) {
        obs_data_set_string(settings, PROP_BG_SOURCE, "");
    }
}

// Lets libobs see the background when it walks the source tree. This is not
// bookkeeping: obs_source_enum_full_tree returns immediately for a source with
// no enum_active_sources (libobs/obs-source.c:4663), so without this the cycle
// check inside obs_source_add_active_child cannot see past a tiles source —
// two tiles sources set as each other's background would both be accepted and
// then recurse until the render thread's stack ran out.
static void tiles_source_enum_active_sources(void *data,
                                             obs_source_enum_proc_t enum_callback,
                                             void *param)
{
    auto *ctx = static_cast<tiles_source *>(data);
    ctx->background.enum_active(ctx->source, enum_callback, param);
}

// No handling here for a pending or in-flight audio reconcile task, and
// none is needed: this function only ever runs once obs_source_t's
// refcount reaches zero, and request_audio_reconcile takes a reference
// (obs_source_get_ref) before queuing one, held until the task itself
// releases it. So this cannot run while a queued task exists — the
// reference blocks it — and by the time a running task calls
// obs_source_release at its own end, this may run, but that release
// happens after the task has finished touching ctx. Either way, `delete
// ctx` below never races a queued or running task.
static void tiles_source_destroy(void *data)
{
    auto *ctx = static_cast<tiles_source *>(data);
    {
        std::lock_guard<std::mutex> callback_lock(ctx->gate->mtx);
        ctx->gate->alive = false;
    }
    ZoomEngineClient::instance().remove_roster_callback(ctx);

    // Before the feeds are retired, and before ctx is freed: this is the one
    // dec_showing that balances the inc_showing taken at selection, and it
    // must not run after `background` has been destructed.
    ctx->background.clear(ctx->source);

    // Scoped so both locks are released before ctx (which owns them) is freed.
    {
        std::lock_guard<std::mutex> engine_lock(ctx->engine_mutex);
        FeedPlan plan;
        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            ctx->visible = false;
            plan = plan_feeds_locked(ctx);
        }
        execute_feed_plan(plan);
    }

    // The intermediate render target for tiles in motion. Destroy does not run
    // on the graphics thread, so the context has to be entered here — exactly
    // as tile_feed_retire() does for a feed's plane textures, and for the same
    // reason: leaking a tile-sized render target per Tiles source would
    // accumulate GPU memory for the length of a show.
    //
    // Null unless a tile actually moved at least once, so a wall whose
    // animation toggle was never on frees nothing because it allocated
    // nothing. Guarded rather than calling gs_texrender_destroy(nullptr) —
    // which is itself safe — so the common case does not enter the graphics
    // context at all: that call blocks until the graphics thread is out of
    // video_render, and paying for it on every source teardown is exactly the
    // kind of cost this feature promises not to add when it is off.
    if (ctx->motion_render) {
        obs_enter_graphics();
        gs_texrender_destroy(ctx->motion_render);
        obs_leave_graphics();
        ctx->motion_render = nullptr;
    }

    delete ctx;
}

// Subscribe while the wall is visible in any view, including the studio-mode
// preview: the operator has to be able to build and check the layout before
// taking it to program. Each visible tile holds one Zoom video subscription.
static void tiles_source_show(void *data)
{
    auto *ctx = static_cast<tiles_source *>(data);

    std::lock_guard<std::mutex> engine_lock(ctx->engine_mutex);
    FeedPlan plan;
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        if (ctx->visible) return;
        ctx->visible = true;
        plan = plan_feeds_locked(ctx);
    }
    execute_feed_plan(plan);
}

static void tiles_source_hide(void *data)
{
    auto *ctx = static_cast<tiles_source *>(data);

    std::lock_guard<std::mutex> engine_lock(ctx->engine_mutex);
    FeedPlan plan;
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        if (!ctx->visible) return;
        ctx->visible = false;
        plan = plan_feeds_locked(ctx);
    }
    execute_feed_plan(plan);
}

static uint32_t tiles_source_get_width(void *data)
{
    auto *ctx = static_cast<tiles_source *>(data);
    return ctx->canvas_width.load(std::memory_order_acquire);
}

static uint32_t tiles_source_get_height(void *data)
{
    auto *ctx = static_cast<tiles_source *>(data);
    return ctx->canvas_height.load(std::memory_order_acquire);
}

// One participant chooser, built the same way every other CoreVideo source
// builds one: a "none" entry at 0, then the live roster.
//
// The roster is passed in rather than fetched here. A properties page builds
// twelve of these choosers, and fetching per chooser took the engine client's
// lock and copied the roster twelve times — worse, a roster event landing
// mid-build produced a dialog where Tile 4's dropdown listed a participant
// Tile 5's did not. One snapshot for the whole page keeps it self-consistent.
static void add_roster_entries(obs_property_t *list,
                               const std::vector<ParticipantInfo> &roster)
{
    obs_property_list_add_int(list, obs_module_text("CoreVideoTiles.NoParticipant"), 0);
    for (const auto &p : roster) {
        std::string label = p.display_name.empty()
            ? "ID " + std::to_string(p.user_id)
            : p.display_name + " (" + std::to_string(p.user_id) + ")";
        if (p.has_video) label += " [video]";
        if (p.is_talking) label += " [talking]";
        obs_property_list_add_int(list, label.c_str(),
                                  static_cast<long long>(p.user_id));
    }
}

// Only one of the two control groups is ever relevant, so hide the other
// rather than leaving dead dropdowns on screen.
static bool tiles_fill_mode_modified(obs_properties_t *props, obs_property_t *,
                                     obs_data_t *settings)
{
    const bool manual = obs_data_get_int(settings, PROP_FILL_MODE) ==
                        static_cast<long long>(TileFillMode::Manual);

    obs_property_set_visible(obs_properties_get(props, PROP_MAX_TILES), !manual);
    for (std::size_t i = 1; i <= kMaxExcludes; ++i) {
        obs_property_set_visible(
            obs_properties_get(props, exclude_prop_name(i).c_str()), !manual);
    }
    for (std::size_t i = 1; i <= kMaxTileSlots; ++i) {
        obs_property_set_visible(
            obs_properties_get(props, tile_prop_name(i).c_str()), manual);
    }
    return true;  // properties changed: redraw the dialog
}

// The corner radius means nothing with square corners, so it is hidden rather
// than left on screen doing nothing. Kept separate from
// tiles_fill_mode_modified() because the two groups are independent: the
// fill-mode callback fires on a fill-mode change, and folding this in would
// make each one redraw controls it has no opinion about.
static bool tiles_border_shape_modified(obs_properties_t *props,
                                        obs_property_t *, obs_data_t *settings)
{
    const bool rounded = obs_data_get_int(settings, PROP_BORDER_SHAPE) ==
                         static_cast<long long>(TileBorderShape::Rounded);
    obs_property_set_visible(obs_properties_get(props, PROP_CORNER_RADIUS),
                             rounded);
    return true;  // properties changed: redraw the dialog
}

// The custom ratio means nothing under a preset, so it is hidden rather than
// left on screen doing nothing. Kept separate from the two callbacks above for
// the same reason they are separate from each other: each one redraws only the
// controls it has an opinion about.
static bool tiles_tile_shape_modified(obs_properties_t *props, obs_property_t *,
                                      obs_data_t *settings)
{
    const bool custom = obs_data_get_int(settings, PROP_TILE_SHAPE) ==
                        static_cast<long long>(TileAspectPreset::Custom);
    obs_property_set_visible(obs_properties_get(props, PROP_TILE_RATIO), custom);
    return true;  // properties changed: redraw the dialog
}

static void tiles_source_get_defaults(obs_data_t *settings)
{
    // 0xFF808080 — the neutral grey the CPU compositor used, so an existing
    // scene looks unchanged until the operator picks a colour.
    obs_data_set_default_int(settings, "bg_color", 0xFF808080);
    // Empty = no background source, i.e. the colour alone, which is exactly
    // what every scene saved before this control existed already does.
    obs_data_set_default_string(settings, PROP_BG_SOURCE, "");
    obs_data_set_default_int(settings, "canvas_width",  1920);
    obs_data_set_default_int(settings, "canvas_height", 1080);
    // Width 0 and Square corners: borders off, so a scene saved before this
    // control existed renders exactly as it did. The colour and radius defaults
    // are only what the operator sees when they first switch borders on.
    obs_data_set_default_int(settings, PROP_BORDER_WIDTH, 0);
    obs_data_set_default_int(settings, PROP_BORDER_COLOR, 0xFF000000);
    obs_data_set_default_int(settings, PROP_BORDER_SHAPE,
                             static_cast<long long>(TileBorderShape::Square));
    obs_data_set_default_int(settings, PROP_CORNER_RADIUS, 16);
    // Glow size 0: the glow is off, and the draw path skips the pass entirely
    // at 0, so a scene saved before this control existed renders byte-for-byte
    // as it did. The colour and intensity defaults are only what the operator
    // sees when they first move the size off zero — white at full strength,
    // which is the soft light halo the reference gallery has.
    obs_data_set_default_int(settings, PROP_GLOW_SIZE, 0);
    obs_data_set_default_int(settings, PROP_GLOW_COLOR, 0xFFFFFFFF);
    obs_data_set_default_int(settings, PROP_GLOW_INTENSITY, 100);
    // Softness 0 is the curve the glow shipped with, so a scene saved before
    // this control existed renders exactly as it did — the same no-regression
    // guarantee the size default carries, and the reason the default is 0
    // rather than whatever eventually matches the reference best.
    obs_data_set_default_int(settings, PROP_GLOW_SOFTNESS, 0);
    // Off by default: the animator's "off" is a complete bypass (it clears
    // its state and emits the solver's rects verbatim), so a scene saved
    // before this control existed renders through exactly the path it
    // renders through today. The duration default is only what the operator
    // sees when they first switch animation on.
    obs_data_set_default_bool(settings, PROP_ANIMATE, false);
    obs_data_set_default_int(settings, PROP_ANIMATE_MS, 350);
    // 16:9 tiles and a canvas_height/135 gutter and margin: exactly what the
    // wall was hard-coded to before these controls existed, so a scene saved
    // before them renders byte-for-byte as it did. The spacing default is a
    // percentage rather than a rounded-off one on purpose —
    // resolve_spacing_px() reproduces canvas_height/135.0 bit-for-bit from it,
    // and tests/tile-shape-test.cpp is what keeps that true.
    obs_data_set_default_int(settings, PROP_TILE_SHAPE,
                             static_cast<long long>(TileAspectPreset::Wide16x9));
    obs_data_set_default_double(settings, PROP_TILE_RATIO, kDefaultTileAspect);
    obs_data_set_default_double(settings, PROP_GUTTER_PCT, kDefaultSpacingPct);
    obs_data_set_default_double(settings, PROP_MARGIN_PCT, kDefaultSpacingPct);
    obs_data_set_default_int(settings, PROP_FILL_MODE,
                             static_cast<long long>(TileFillMode::Auto));
    obs_data_set_default_int(settings, PROP_MAX_TILES,
                             static_cast<long long>(kMaxTileSlots));
    // Every chooser defaults to "none"; Auto mode fills the wall on its own.
    for (std::size_t i = 1; i <= kMaxExcludes; ++i)
        obs_data_set_default_int(settings, exclude_prop_name(i).c_str(), 0);
    for (std::size_t i = 1; i <= kMaxTileSlots; ++i)
        obs_data_set_default_int(settings, tile_prop_name(i).c_str(), 0);
    // No crop on any slot, so a scene saved before these controls existed
    // renders byte-for-byte as it did: solve_slot_crop() with both sides at 0
    // is solve_cover_crop().
    for (std::size_t i = 1; i <= kMaxTileSlots; ++i) {
        obs_data_set_default_int(settings, crop_left_prop_name(i).c_str(), 0);
        obs_data_set_default_int(settings, crop_right_prop_name(i).c_str(), 0);
    }
    // Empty: per-participant audio is off. See PROP_AUDIO_GROUP.
    obs_data_set_default_string(settings, PROP_AUDIO_GROUP, "");
}

static obs_properties_t *tiles_source_get_properties(void *data)
{
    auto *ctx = static_cast<tiles_source *>(data);
    obs_properties_t *props = obs_properties_create();

    // One snapshot for every chooser on this page — see add_roster_entries().
    const std::vector<ParticipantInfo> roster =
        ZoomEngineClient::instance().roster();

    obs_property_t *mode = obs_properties_add_list(props, PROP_FILL_MODE,
        obs_module_text("CoreVideoTiles.FillMode"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode, obs_module_text("CoreVideoTiles.FillModeAuto"),
                              static_cast<long long>(TileFillMode::Auto));
    obs_property_list_add_int(mode, obs_module_text("CoreVideoTiles.FillModeManual"),
                              static_cast<long long>(TileFillMode::Manual));
    obs_property_set_modified_callback(mode, tiles_fill_mode_modified);

    obs_properties_add_int(props, PROP_MAX_TILES,
        obs_module_text("CoreVideoTiles.MaxTiles"), 1,
        static_cast<int>(kMaxTileSlots), 1);

    for (std::size_t i = 1; i <= kMaxExcludes; ++i) {
        const std::string name = exclude_prop_name(i);
        const std::string label =
            std::string(obs_module_text("CoreVideoTiles.Exclude")) + " " +
            std::to_string(i);
        add_roster_entries(obs_properties_add_list(props, name.c_str(),
            label.c_str(), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT), roster);
    }

    for (std::size_t i = 1; i <= kMaxTileSlots; ++i) {
        const std::string name = tile_prop_name(i);
        const std::string label =
            std::string(obs_module_text("CoreVideoTiles.Tile")) + " " +
            std::to_string(i);
        add_roster_entries(obs_properties_add_list(props, name.c_str(),
            label.c_str(), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT), roster);
    }

    obs_properties_add_color(props, "bg_color",
        obs_module_text("CoreVideoTiles.BackgroundColor"));

    // Any other OBS source, drawn over the colour and under the tiles. The
    // tiles source itself is in this list — obs_enum_sources enumerates every
    // non-private input — and picking it is refused by the cycle check in
    // TilesBackground::set_source rather than hidden here, because the same
    // refusal has to hold for a name that arrives from a hand-edited scene
    // file or obs-websocket, not just from this dropdown.
    obs_property_t *bg = obs_properties_add_list(props, PROP_BG_SOURCE,
        obs_module_text("CoreVideoTiles.BackgroundSource"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(bg, obs_module_text("CoreVideoTiles.BackgroundNone"), "");
    obs_enum_sources([](void *param, obs_source_t *src) -> bool {
        auto *list = static_cast<obs_property_t *>(param);
        const uint32_t flags = obs_source_get_output_flags(src);
        // Video-producing sources only: an audio-only source as a background
        // is a control that can do nothing.
        if (flags & OBS_SOURCE_VIDEO) {
            const char *n = obs_source_get_name(src);
            if (n) obs_property_list_add_string(list, n, n);
        }
        return true;
    }, bg);

    // Editable so a scene file naming a group that does not exist yet round-
    // trips instead of silently resetting to "off" on load.
    //
    // Deliberately no "Off" list entry. For an editable combo, OBS stores
    // whatever text is displayed, not the item's data value (Qt's
    // QComboBox::currentText(), not currentData()) — so a labelled entry
    // like "Off — no audio sources created" would itself become the stored
    // group name the moment it was selected, which both fails to turn the
    // feature off and then fails to find a group by that name on every
    // later reconcile. An empty, blank field is off, and it is unambiguous:
    // whatever text is showing IS the value that gets stored.
    obs_property_t *audio_group = obs_properties_add_list(
        props, PROP_AUDIO_GROUP, obs_module_text("CoreVideoTiles.AudioGroup"),
        OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
    // Both containers are offered because both work, and the choice belongs to
    // the operator. A nested scene is the better one for audio that has to
    // survive every scene change: a group sits one right-click away from
    // "Ungroup", which dissolves it and scatters the sources inside into the
    // parent scene. Scenes are listed first for that reason.
    //
    // Two enumerations are needed, not one: obs_enum_sources walks inputs and
    // groups but NOT scenes (libobs/obs.c:1837), so a scene-only sweep is the
    // only way a nested scene reaches this list.
    const auto add_container = [](void *param, obs_source_t *src) -> bool {
        const char *name = obs_source_get_name(src);
        if (name && *name)
            obs_property_list_add_string(
                static_cast<obs_property_t *>(param), name, name);
        return true;
    };
    obs_enum_scenes(add_container, audio_group);
    obs_enum_sources(
        [](void *param, obs_source_t *src) -> bool {
            if (!obs_group_from_source(src)) return true;
            const char *name = obs_source_get_name(src);
            if (name && *name)
                obs_property_list_add_string(
                    static_cast<obs_property_t *>(param), name, name);
            return true;
        },
        audio_group);
    obs_property_set_long_description(
        audio_group, obs_module_text("CoreVideoTiles.AudioGroup.Desc"));

    // Tile shape. Presets rather than a bare ratio because the shapes an
    // operator actually wants are a short list, with Custom behind a modified
    // callback for the ones that are not on it — the same pattern the corner
    // radius uses.
    obs_property_t *shape_list = obs_properties_add_list(props, PROP_TILE_SHAPE,
        obs_module_text("CoreVideoTiles.TileShape"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    static const struct { TileAspectPreset preset; const char *key; } kShapes[] = {
        {TileAspectPreset::Wide16x9,    "CoreVideoTiles.Shape16x9"},
        {TileAspectPreset::Standard4x3, "CoreVideoTiles.Shape4x3"},
        {TileAspectPreset::Photo5x4,    "CoreVideoTiles.Shape5x4"},
        {TileAspectPreset::Square1x1,   "CoreVideoTiles.Shape1x1"},
        {TileAspectPreset::Portrait3x4, "CoreVideoTiles.Shape3x4"},
        {TileAspectPreset::Tall9x16,    "CoreVideoTiles.Shape9x16"},
        {TileAspectPreset::Custom,      "CoreVideoTiles.ShapeCustom"},
    };
    for (const auto &s : kShapes) {
        obs_property_list_add_int(shape_list, obs_module_text(s.key),
                                  static_cast<long long>(s.preset));
    }
    obs_property_set_modified_callback(shape_list, tiles_tile_shape_modified);

    obs_properties_add_float(props, PROP_TILE_RATIO,
        obs_module_text("CoreVideoTiles.TileRatio"),
        kMinCustomAspect, kMaxCustomAspect, 0.01);

    // Spacing, as a percentage of canvas height so it scales with the canvas.
    // Three decimals is not fussiness: the snap pass truncates the gutter to an
    // even pixel, so at 1080p a value displayed as 0.74 rather than 0.741 would
    // solve 7.99 px and snap to a 6 px gutter.
    obs_properties_add_float_slider(props, PROP_GUTTER_PCT,
        obs_module_text("CoreVideoTiles.Gutter"), 0.0, kMaxSpacingPct, 0.001);
    obs_properties_add_float_slider(props, PROP_MARGIN_PCT,
        obs_module_text("CoreVideoTiles.Margin"), 0.0, kMaxSpacingPct, 0.001);

    // Tile borders. Width 0 is "no border", which is the default, so these four
    // controls are inert until the operator moves the width off zero.
    obs_properties_add_int_slider(props, PROP_BORDER_WIDTH,
        obs_module_text("CoreVideoTiles.BorderWidth"),
        0, static_cast<int>(kMaxBorderWidth), 1);
    obs_properties_add_color(props, PROP_BORDER_COLOR,
        obs_module_text("CoreVideoTiles.BorderColor"));

    obs_property_t *shape = obs_properties_add_list(props, PROP_BORDER_SHAPE,
        obs_module_text("CoreVideoTiles.BorderShape"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(shape,
        obs_module_text("CoreVideoTiles.BorderSquare"),
        static_cast<long long>(TileBorderShape::Square));
    obs_property_list_add_int(shape,
        obs_module_text("CoreVideoTiles.BorderRounded"),
        static_cast<long long>(TileBorderShape::Rounded));
    obs_property_set_modified_callback(shape, tiles_border_shape_modified);

    obs_properties_add_int_slider(props, PROP_CORNER_RADIUS,
        obs_module_text("CoreVideoTiles.CornerRadius"),
        0, static_cast<int>(kMaxCornerRadius), 1);

    // Outer glow. Size 0 is "no glow", which is the default, so the colour and
    // intensity are inert until the operator moves the size off zero — the same
    // arrangement as the border width above.
    //
    // Left visible rather than hidden behind a modified callback (as the corner
    // radius and the custom ratio are): those two are meaningless under the
    // setting they hang off, whereas a glow colour chosen before the size is
    // raised is a perfectly reasonable order to work in, and hiding the
    // controls would make the feature hard to find.
    //
    // Deliberately not clamped against the gutter or the margin. A halo wider
    // than half the gap merges with its neighbour into a wash and one wider
    // than the margin clips at the canvas edge; both are legitimate small and
    // obviously wrong large, and that is a judgement to make by eye.
    obs_properties_add_int_slider(props, PROP_GLOW_SIZE,
        obs_module_text("CoreVideoTiles.GlowSize"),
        0, static_cast<int>(kMaxGlowSize), 1);
    obs_properties_add_color(props, PROP_GLOW_COLOR,
        obs_module_text("CoreVideoTiles.GlowColor"));
    obs_properties_add_int_slider(props, PROP_GLOW_INTENSITY,
        obs_module_text("CoreVideoTiles.GlowIntensity"),
        0, static_cast<int>(kMaxGlowIntensity), 1);

    // How the halo falls off, so it can be matched against a reference image by
    // eye rather than by another build. 0% is the curve the glow shipped with
    // — strongest against the tile edge and dropping away immediately — and
    // 100% holds the halo's strength just outside the tile before falling away
    // through an inflection, which is how a blurred reference reads.
    //
    // What this control does NOT change is the peak: the halo is still at full
    // intensity AT the tile edge for every setting, whereas a Photoshop or
    // Gaussian outer glow sits around half strength there because half the blur
    // kernel falls inside the shape. Lowering the peak is what "Glow intensity"
    // already does, so matching such a reference means roughly 50% intensity
    // plus whatever softness looks right — one control each, rather than two
    // controls fighting over the same number.
    obs_properties_add_int_slider(props, PROP_GLOW_SOFTNESS,
        obs_module_text("CoreVideoTiles.GlowSoftness"),
        0, static_cast<int>(kMaxGlowSoftness), 1);

    // Off by default: upgrading must not change how any existing scene
    // looks. The animator's "off" is a complete bypass — it clears its state
    // and emits the solver's rects verbatim — so a scene saved before this
    // control existed renders through exactly the path it renders through
    // today.
    obs_properties_add_bool(props, PROP_ANIMATE,
                            obs_module_text("CoreVideoTiles.Animate"));
    obs_properties_add_int_slider(props, PROP_ANIMATE_MS,
                                  obs_module_text("CoreVideoTiles.AnimateMs"),
                                  100, 1000, 10);

    // Per-slot left/right crop, for reframing a badly-framed guest without
    // touching the grid. Eighteen sliders inline would swamp a dialog that
    // already carries thirteen choosers, so they go in a collapsible group —
    // the operator opens it only when they need it.
    //
    // Deliberately NOT hidden in Auto mode: a crop belongs to the tile slot,
    // not to whoever is currently in it, and Auto walls get reframed too.
    obs_properties_t *crop_group = obs_properties_create();
    for (std::size_t i = 1; i <= kMaxTileSlots; ++i) {
        const std::string left  = crop_left_prop_name(i);
        const std::string right = crop_right_prop_name(i);
        // Built by concatenation, as the tile and exclude labels above are, so
        // the same "Tile" string serves all of them.
        const std::string left_label =
            std::string(obs_module_text("CoreVideoTiles.Tile")) + " " +
            std::to_string(i) + " " +
            obs_module_text("CoreVideoTiles.CropLeftSuffix");
        const std::string right_label =
            std::string(obs_module_text("CoreVideoTiles.Tile")) + " " +
            std::to_string(i) + " " +
            obs_module_text("CoreVideoTiles.CropRightSuffix");
        obs_properties_add_int_slider(crop_group, left.c_str(),
            left_label.c_str(), 0, static_cast<int>(kMaxSlotCropPct), 1);
        obs_properties_add_int_slider(crop_group, right.c_str(),
            right_label.c_str(), 0, static_cast<int>(kMaxSlotCropPct), 1);
    }
    // OBS takes ownership of crop_group here (obs_properties_add_group ->
    // obs_property_group_content), so it must not be destroyed by this
    // function.
    obs_properties_add_group(props, PROP_CROP_GROUP,
        obs_module_text("CoreVideoTiles.CropGroup"),
        OBS_GROUP_NORMAL, crop_group);

    obs_properties_add_int(props, "canvas_width",
        obs_module_text("CoreVideoTiles.CanvasWidth"),
        kMinCanvasW, kMaxCanvasW, 2);
    obs_properties_add_int(props, "canvas_height",
        obs_module_text("CoreVideoTiles.CanvasHeight"),
        kMinCanvasH, kMaxCanvasH, 2);

    obs_properties_add_button(props, "btn_refresh",
        obs_module_text("CoreVideoTiles.RefreshParticipants"),
        [](obs_properties_t *, obs_property_t *, void *) -> bool { return true; });

    // Apply the correct initial group visibility before returning, mirroring
    // zoom_source_get_properties (src/zoom-source.cpp:1985-1987) — otherwise
    // every control defaults to visible and the dialog shows all 13 at once
    // until the operator first touches the fill-mode combo. OBS also calls
    // get_properties with data == nullptr when building a properties view for
    // the source *type* rather than an instance, so ctx can be null here; the
    // obs_data_create() fallback gives tiles_fill_mode_modified a fill_mode
    // read of 0 (TileFillMode::Auto), the correct default layout.
    obs_data_t *visibility_settings =
        ctx ? obs_source_get_settings(ctx->source) : obs_data_create();
    tiles_fill_mode_modified(props, nullptr, visibility_settings);
    // Same reason, for the corner radius: without this it would show for a
    // Square wall until the operator first touched the shape combo. The
    // obs_data_create() fallback reads border_shape as 0 (Square), which is
    // the correct default layout.
    tiles_border_shape_modified(props, nullptr, visibility_settings);
    // Same reason again, for the custom tile ratio: the obs_data_create()
    // fallback reads tile_shape as 0 (16:9), so the ratio starts hidden, which
    // is the correct layout for every preset.
    tiles_tile_shape_modified(props, nullptr, visibility_settings);
    obs_data_release(visibility_settings);

    return props;
}

// Called from plugin-main.cpp's frontend event callback: `loading = true` on
// OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING and _CLEANUP, `false` on
// OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED and _FINISHED_LOADING. See
// s_collection_loading and request_audio_reconcile for why this exists at
// all: obs_queue_task(OBS_TASK_UI, ...) runs inline rather than deferred when
// the caller is already the UI thread, which a scene-collection load always
// is.
//
// Turning the gate off is the moment every reconcile suppressed while it was
// on has to actually happen — nothing else provides that trigger, since
// request_audio_reconcile deliberately did not queue anything while
// s_collection_loading was true, and the queued task body bails for the same
// reason. obs_enum_sources filtered on the Tiles source id does the job
// without a separate registry of every ctx: request_audio_reconcile's own
// audio_group/coalescing checks then decide per-instance whether there is
// actually anything to do. obs_obj_get_data recovers each source's ctx — the
// same plugin-data pointer create() returned — from the obs_source_t
// obs_enum_sources hands back.
//
// The enum only *collects*; the reconciles run after it returns.
// obs_enum_sources holds the global obs->data.sources_mutex across every
// callback (libobs/obs.c:1837-1862), and once the gate is open a reconcile
// requested from the UI thread runs its whole scan-plan-apply inline —
// obs_source_create, obs_scene_add and their signal handlers included. Doing
// that from inside the callback would mutate the very list being walked while
// holding the lock that protects it, and stall every other thread that needs
// that lock for the duration. It does not self-deadlock (the mutex is
// recursive, libobs/obs.c:973) and the uthash append keeps the walk valid, so
// this is a stall and a gratuitous lock-order edge rather than a hang — but
// there is no reason to pay for either.
//
// Each collected source is held by a reference taken inside the callback:
// obs_enum_sources' own per-callback reference is released the moment the
// callback returns, and a bare ctx pointer could be dangling by the time the
// loop below runs it.
void zoom_supersource_set_collection_loading(bool loading)
{
    s_collection_loading.store(loading, std::memory_order_release);
    if (loading) return;

    std::vector<obs_source_t *> tiles_sources;
    obs_enum_sources(
        [](void *param, obs_source_t *src) -> bool {
            const char *id = obs_source_get_id(src);
            if (id && std::strcmp(id, kTilesSourceId) == 0) {
                if (obs_source_t *ref = obs_source_get_ref(src))
                    static_cast<std::vector<obs_source_t *> *>(param)->push_back(ref);
            }
            return true;
        },
        &tiles_sources);

    for (obs_source_t *src : tiles_sources) {
        if (auto *ctx = static_cast<tiles_source *>(obs_obj_get_data(src)))
            request_audio_reconcile(ctx);
        obs_source_release(src);
    }

    // One line per gate clear, on purpose: a gate stuck set (a collection
    // switch that threw after CHANGING, or a clearingFailed that keeps
    // disableSaving elevated so CHANGED is filtered out by
    // OBSStudioAPI.cpp:711) silently disables this whole feature. Stuck-set
    // fails safe, but it must not be invisible — its signature is a load with
    // no matching line here.
    blog(LOG_INFO,
         "[obs-zoom-plugin] Tiles audio: collection-load gate cleared, swept %d "
         "Tiles source(s)",
         static_cast<int>(tiles_sources.size()));
}

void zoom_supersource_register()
{
    obs_source_info info = {};
    info.id             = kTilesSourceId;
    info.type           = OBS_SOURCE_TYPE_INPUT;
    // The wall draws itself on the graphics thread instead of pushing finished
    // frames: OBS_SOURCE_CUSTOM_DRAW because it binds its own effect rather
    // than letting OBS draw one texture with the default one.
    info.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
                          OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name       = tiles_source_get_name;
    info.video_render   = tiles_source_render;
    info.create         = tiles_source_create;
    info.destroy        = tiles_source_destroy;
    info.update         = tiles_source_update;
    info.load           = tiles_source_load;
    info.enum_active_sources = tiles_source_enum_active_sources;
    info.show           = tiles_source_show;
    info.hide           = tiles_source_hide;
    info.get_width      = tiles_source_get_width;
    info.get_height     = tiles_source_get_height;
    info.get_properties = tiles_source_get_properties;
    info.get_defaults   = tiles_source_get_defaults;
    obs_register_source(&info);
}

void zoom_supersource_load_gfx()
{
    tiles_effect_load(s_tiles_effect);
}

void zoom_supersource_unload_gfx()
{
    // Module unload does not run on the graphics thread, so unlike the lazy
    // creation inside video_render this has to enter the context itself.
    obs_enter_graphics();
    destroy_neutral_textures();
    obs_leave_graphics();
    s_neutral_failed_logged = false;
    s_tile_texture_failed_logged = false;
    s_tile_pass_failed_logged = false;
    s_bg_pass_failed_logged = false;
    s_glow_pass_failed_logged = false;
    s_glow_unavailable_logged = false;
    s_motion_render_failed_logged = false;
    tiles_effect_destroy(s_tiles_effect);
}
