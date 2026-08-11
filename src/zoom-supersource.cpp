#include "zoom-supersource.h"
#include "engine-ipc.h"
#include "shm-resubscribe.h"
#include "zoom-engine-client.h"
#include "zoom-tile-border.h"
#include "zoom-tile-crop.h"
#include "zoom-tile-fill.h"
#include "zoom-tile-grid.h"
#include "zoom-tile-retry.h"
#include "zoom-tile-slot.h"
#include "zoom-tile-texture.h"
#include "zoom-tiles-background.h"
#include "zoom-tiles-effect.h"

#include <graphics/vec4.h>  // obs.h brings in vec2/vec3 but not vec4
#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Tiles are always 16:9 (standing product rule). The operator's border is drawn
// *inside* that rect by the shader rather than around it, so it changes what a
// tile looks like without changing the grid the tiles were solved into — the
// parity-verified geometry in snap_tile_grid_even() is untouched.
static constexpr double kTileAspect = 16.0 / 9.0;
// Border property bounds. Enforced against scene data as well as the sliders:
// obs_data_get_int returns int64 and scene files are hand-editable. These only
// bound the *setting*; clamp_border() then bounds it against the actual tile.
static constexpr int64_t kMaxBorderWidth  = 64;
static constexpr int64_t kMaxCornerRadius = 128;
// How many tile slots the wall has. Declared up here rather than beside the
// other property constants below because `tiles_source` sizes its per-slot crop
// array from it.
static constexpr std::size_t kMaxTileSlots = 9;
// Per-slot crop bound, in percent of the source width. 45 each side means left
// and right together can never exceed 90%, so kMinCropRemainder in
// zoom-tile-crop.h stays a defensive backstop for hand-edited scene files
// rather than something the sliders reach.
static constexpr int64_t kMaxSlotCropPct = 45;
// Gutter and margin scale with the canvas: 8 px at 1080p.
static constexpr double kSpacingDivisor = 135.0;
// Neutral fill for the background and for tiles with no frame yet.
static constexpr uint8_t kNeutralY = 0x80;
static constexpr uint8_t kNeutralUV = 0x80;
// Canvas bounds, matching the property ranges. Enforced against scene data too:
// an out-of-range value would otherwise reach resize() as a huge allocation.
static constexpr uint32_t kMinCanvasW = 16, kMaxCanvasW = 7680;
static constexpr uint32_t kMinCanvasH = 16, kMaxCanvasH = 4320;

static std::atomic<uint64_t> s_tiles_instance_counter{0};
static std::atomic<uint64_t> s_tile_feed_serial{0};
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

    // Byte-for-byte the parameters the CPU compositor solved from, including
    // the even-snapping pass. The named constants are load-bearing: substituting
    // 16.0/9.0 or 135.0, or dropping the snap, moves every tile by up to a
    // pixel against the parity baseline in docs/design-reference/. Solved from
    // feeds.size(), as the compositor did, not from participants.size(): the
    // two can differ while a plan is in flight.
    TileGridParams params;
    params.canvas_width  = static_cast<double>(canvas_w);
    params.canvas_height = static_cast<double>(canvas_h);
    params.tile_aspect   = kTileAspect;
    params.gutter        = static_cast<double>(canvas_h) / kSpacingDivisor;
    params.margin        = params.gutter;
    const std::vector<SnappedTileRect> rects =
        snap_tile_grid_even(solve_tile_grid(feeds.size(), params), params);

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
    for (size_t i = 0; i < rects.size() && i < feeds.size(); ++i) {
        const SnappedTileRect &r = rects[i];
        if (r.width < 2 || r.height < 2) continue;  // as the CPU path skips them

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
            tiles_draw_neutral(tech, r.x, r.y, r.width, r.height, pass);
            continue;
        }

        // Narrow the usable source by this slot's crop, then sample the largest
        // centred 16:9 sub-rectangle of what is left, so the tile fills
        // completely and is never letterboxed — the same solve_cover_crop() the
        // CPU blit used, mapped straight onto gs_draw_sprite_subregion(), which
        // samples exactly such a sub-rectangle.
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
                                              kTileAspect,
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
            tiles_draw_neutral(tech, r.x, r.y, r.width, r.height, pass);
            continue;
        }

        // The same sub-rectangle, in normalized texture coords, so the shader
        // can undo the crop and recover a 0..1 coordinate across the tile for
        // its distance field. Derived from the truncated integers actually
        // passed to gs_draw_sprite_subregion() below, and from tex_w/tex_h,
        // because those are precisely what build_subsprite_norm() divides by
        // when it builds the quad's UVs — anything else would put the border
        // slightly out of register with the video.
        //
        // This is why the slot crop above changes nothing here: it moves the
        // sub-rectangle, and these four come from that same moved rectangle by
        // construction. Any future change must keep computing crop_uv from the
        // exact integers handed to gs_draw_sprite_subregion(), or borders will
        // silently misregister on every tile with a non-zero crop.
        pass.crop_u  = static_cast<float>(crop_x) / static_cast<float>(feed->tex_w);
        pass.crop_v  = static_cast<float>(crop_y) / static_cast<float>(feed->tex_h);
        pass.crop_cu = static_cast<float>(crop_w) / static_cast<float>(feed->tex_w);
        pass.crop_cv = static_cast<float>(crop_h) / static_cast<float>(feed->tex_h);

        if (!tiles_begin_pass(tech, feed->tex_y, feed->tex_u, feed->tex_v, pass))
            continue;
        gs_matrix_push();
        gs_matrix_translate3f(static_cast<float>(r.x), static_cast<float>(r.y),
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
    }

    gs_technique_end(tech);
    gs_blend_state_pop();

    // Rate-limited so the rig can confirm the draw path is live and see both
    // the grid it solved and how many tiles actually have a feed, without
    // flooding the log at the frame rate.
    if (ctx->rendered_frames == 0 || ctx->rendered_frames % 300 == 0) {
        blog(LOG_INFO,
             "[obs-zoom-plugin] Tiles render: canvas=%ux%u tiles=%zu with_video=%zu count=%llu",
             canvas_w, canvas_h, rects.size(), drawn,
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
// The collapsible group the eighteen crop sliders live in. Named so a scene
// file can be read, though the group itself stores no value of its own.
static constexpr const char *PROP_CROP_GROUP = "crop_group";
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

// Recomputes the wall from the cached settings plus the live roster, and
// performs whatever engine work the change implies. Safe to call from the
// settings path and from the roster callback.
static void apply_assignments(tiles_source *ctx)
{
    std::lock_guard<std::mutex> engine_lock(ctx->engine_mutex);

    // Fetched before ctx->mutex: roster() takes the engine client's lock, and
    // taking them in the other order anywhere would invite a deadlock.
    const std::vector<ParticipantInfo> roster =
        ZoomEngineClient::instance().roster();

    FeedPlan plan;
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        std::vector<uint32_t> next =
            resolve_tile_assignments(ctx->participants, roster, ctx->fill_params);
        if (ctx->participants == next) return;
        ctx->participants.swap(next);
        plan = plan_feeds_locked(ctx);
    }
    execute_feed_plan(plan);
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
    obs_data_release(visibility_settings);

    return props;
}

void zoom_supersource_register()
{
    obs_source_info info = {};
    info.id             = "corevideo_tiles_source";
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
    tiles_effect_destroy(s_tiles_effect);
}
