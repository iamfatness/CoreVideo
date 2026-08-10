#include "zoom-supersource.h"
#include "engine-ipc.h"
#include "zoom-engine-client.h"
#include "zoom-tile-fill.h"
#include "zoom-tile-grid.h"
#include "zoom-tile-slot.h"

#include <media-io/video-io.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Tiles are always 16:9 and never bordered (standing product rule).
static constexpr double kTileAspect = 16.0 / 9.0;
// Gutter and margin scale with the canvas: 8 px at 1080p.
static constexpr double kSpacingDivisor = 135.0;
// Neutral fill for the background and for tiles with no frame yet.
static constexpr uint8_t kNeutralY = 0x80;
static constexpr uint8_t kNeutralUV = 0x80;
static constexpr uint64_t kDefaultFrameIntervalNs = 1000000000ULL / 60ULL;
// Canvas bounds, matching the property ranges. Enforced against scene data too:
// an out-of-range value would otherwise reach resize() as a huge allocation.
static constexpr uint32_t kMinCanvasW = 16, kMaxCanvasW = 7680;
static constexpr uint32_t kMinCanvasH = 16, kMaxCanvasH = 4320;

static std::atomic<uint64_t> s_tiles_instance_counter{0};
static std::atomic<uint64_t> s_tile_feed_serial{0};
// Globally unique per decoded frame, so a scratch buffer can tell whether the
// pixels it holds are still the newest ones for its slot without any chance of
// a stale value colliding across feeds.
static std::atomic<uint64_t> s_frame_generation{0};

// One tile slot's feed. The engine publishes each subscription into its own
// shared-memory region, so a slot owns a uuid, a subscription, and a mapping.
//
// The uuid is derived from the slot index and stays fixed for the slot's
// lifetime, deliberately: the engine's command dispatch matches "subscribe" as
// a substring before it ever tests "unsubscribe", so an unsubscribe never
// reaches the branch that would drop the engine-side audio target for that
// uuid. Minting a fresh uuid per reassignment would therefore strand one
// orphaned audio target per reassignment, each still being fed mixed meeting
// audio for the rest of the session. Keeping the uuid pinned to the slot
// bounds that at one per slot, and matches how the per-participant source
// reuses its own uuid across resubscribes.
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
    bool has_frame = false;  // pixels present that the compositor has not taken
};
using TileFeedPtr = std::shared_ptr<TileFeed>;

// The compositor's private copy of one slot's newest frame. Owned solely by the
// worker thread, so the blit runs with no lock held.
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

    // Serializes whole plan-and-execute cycles (update/show/hide/destroy) so
    // engine IPC happens outside `mutex` yet still in a well-defined order.
    std::mutex engine_mutex;

    std::mutex mutex;  // guards participants, feeds, fill_params and visible
    std::vector<uint32_t> participants;
    // The settings the resolver needs. Cached because the roster callback runs
    // with no obs_data_t in hand — it only knows the participants changed.
    TileFillParams fill_params;
    std::vector<TileFeedPtr> feeds;  // parallel to participants, slot for slot
    bool visible = false;
    // Distinguishes this source's slot uuids from any other tiles source.
    uint64_t instance_id = 0;

    // Owned solely by the composite thread — no locking needed.
    std::vector<uint8_t> canvas_buf;
    std::vector<TileScratch> scratches;
    std::vector<TileFeedPtr> feeds_snapshot;  // reused: assign keeps capacity
    uint32_t    buf_width  = 0;
    uint32_t    buf_height = 0;
    std::size_t buf_tiles  = 0;
    uint64_t    composited_frames = 0;

    std::thread       worker;
    std::atomic<bool> running{false};

    std::shared_ptr<TilesCallbackGate> gate =
        std::make_shared<TilesCallbackGate>();
};

// ── Geometry helpers ─────────────────────────────────────────────────────────

// Truncates to an even pixel, for source-rect edges that must stay inside the
// frame. Tile placement is snapped by snap_tile_grid_even() instead, which
// preserves uniform spacing; see zoom-tile-grid.h.
static uint32_t even_floor(double v)
{
    if (v <= 0.0) return 0;
    return static_cast<uint32_t>(v) & ~1u;
}

// ── Pixel helpers ────────────────────────────────────────────────────────────

// Nearest-neighbour blit of one plane's sub-rectangle into a sub-rectangle of
// the destination. Matches the sampling the per-participant source already uses
// for its letterbox scaler.
static void blit_plane_nearest(const uint8_t *src, uint32_t src_stride,
                               uint32_t src_x, uint32_t src_y,
                               uint32_t src_w, uint32_t src_h,
                               uint8_t *dst, uint32_t dst_stride,
                               uint32_t dst_x, uint32_t dst_y,
                               uint32_t dst_w, uint32_t dst_h)
{
    if (!src || !dst || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0)
        return;

    const uint8_t *src_origin = src + static_cast<size_t>(src_y) * src_stride + src_x;
    uint8_t *dst_origin = dst + static_cast<size_t>(dst_y) * dst_stride + dst_x;

    for (uint32_t y = 0; y < dst_h; ++y) {
        const uint32_t sy = std::min<uint32_t>(
            static_cast<uint32_t>((static_cast<uint64_t>(y) * src_h) / dst_h),
            src_h - 1);
        const uint8_t *src_row = src_origin + static_cast<size_t>(sy) * src_stride;
        uint8_t *dst_row = dst_origin + static_cast<size_t>(y) * dst_stride;
        for (uint32_t x = 0; x < dst_w; ++x) {
            const uint32_t sx = std::min<uint32_t>(
                static_cast<uint32_t>((static_cast<uint64_t>(x) * src_w) / dst_w),
                src_w - 1);
            dst_row[x] = src_row[sx];
        }
    }
}

static void fill_plane_rect(uint8_t *dst, uint32_t dst_stride,
                            uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            uint8_t value)
{
    if (!dst || w == 0 || h == 0) return;
    uint8_t *row = dst + static_cast<size_t>(y) * dst_stride + x;
    for (uint32_t i = 0; i < h; ++i, row += dst_stride)
        std::memset(row, value, w);
}

// Fills one canvas rect (all three planes) with neutral grey. Coordinates must
// already be even.
static void fill_tile_neutral(uint8_t *y_plane, uint8_t *u_plane, uint8_t *v_plane,
                              uint32_t canvas_w,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    fill_plane_rect(y_plane, canvas_w, x, y, w, h, kNeutralY);
    fill_plane_rect(u_plane, canvas_w / 2, x / 2, y / 2, w / 2, h / 2, kNeutralUV);
    fill_plane_rect(v_plane, canvas_w / 2, x / 2, y / 2, w / 2, h / 2, kNeutralUV);
}

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
static void tile_feed_subscribe(const TileFeedPtr &feed)
{
    ZoomEngineClient::instance().subscribe(feed->uuid, feed->slot.participant_id(),
                                           false, false, VideoResolution::P720);
}

static void tile_feed_register(const TileFeedPtr &feed)
{
    ZoomEngineClient::instance().register_source(feed->uuid, {
        [feed](uint32_t width, uint32_t height, uint32_t participant_id,
               uint32_t shm_generation) {
            tile_feed_on_frame(feed, width, height, participant_id,
                               shm_generation);
        },
        {}  // tiles are video-only; audio stays on the dedicated audio sources
    });
}

// Releases this slot's read mapping of its shared-memory region. MUST be called
// before any subscribe that re-points an existing uuid, and before the subscribe
// reaches the engine.
//
// Why, in full, because this is not redundant and deleting it reintroduces a
// production incident (2026-08-08):
//
// Re-subscribing the same source_uuid at a different participant makes the
// engine run unsubscribe_locked() first (EngineVideo::subscribe,
// engine/src/engine-video.cpp), which destroys that uuid's SourceTarget. shm_gen
// is a member of SourceTarget (engine/src/engine-video.h), so the replacement
// target restarts at generation 0 and its first ensure_shm() creates generation
// 1 — which is the *legacy unsuffixed* region name (shm_region_name() in
// engine-ipc.h). Meanwhile we are still mapping the old section under that exact
// name. A Windows named section cannot be recreated at a larger size while any
// process still maps it: CreateFileMappingA hands back the existing smaller
// section and the larger MapViewOfFile fails. ensure_shm() then returns false
// and the engine drops the frame *without* emitting a frame event, and since the
// plugin only reopens on a generation change (shm_mapping_stale) and both sides
// read generation 1, the two never resynchronise. The tile stays black for the
// rest of the session; only hiding and re-showing the source recovers it.
//
// Dropping the mapping first leaves the name free for the engine to recreate at
// the new size. It discards no pixels: `frame` (the decoded copy the compositor
// reads) is separate from `shm` (the window onto the engine's buffer), and the
// next frame event reopens the mapping — ensure_shm() retries on every frame, so
// even if this release loses the race with the engine's first attempt, the
// following frame self-heals. Same release-then-subscribe order as the
// director-preview repoint in src/zoom-source.cpp.
//
// Takes feed->mtx, the innermost lock: callers hold ctx->engine_mutex with
// ctx->mutex released, matching tile_feed_retire().
static void tile_feed_release_mapping(const TileFeedPtr &feed)
{
    if (!feed) return;
    std::lock_guard<std::mutex> lock(feed->mtx);
    shm_region_destroy(feed->shm);
    // No mapping is held, so no generation is pinned open. The next frame event
    // reopens against whatever generation the engine reports.
    feed->shm_gen = 0;
}

static void tile_feed_retire(const TileFeedPtr &feed)
{
    if (!feed) return;
    ZoomEngineClient::instance().unsubscribe(feed->uuid);
    ZoomEngineClient::instance().unregister_source(feed->uuid);
    blog(LOG_INFO, "[obs-zoom-plugin] Tile slot retired: uuid=%s participant_id=%u",
         feed->uuid.c_str(), feed->slot.participant_id());
    // A callback dispatched before unregister_source() may still be running; it
    // holds feed->mtx, so this blocks until it finishes and then locks it out.
    std::lock_guard<std::mutex> lock(feed->mtx);
    feed->alive = false;
    shm_region_destroy(feed->shm);
}

// The engine work implied by a change to the assignment list. Computing it is
// pure bookkeeping; performing it is blocking pipe I/O. They are split so the
// I/O never runs under ctx->mutex, which the compositor takes every frame.
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
static void resubscribe_silent_feeds(tiles_source *ctx)
{
    std::lock_guard<std::mutex> engine_lock(ctx->engine_mutex);

    std::vector<TileFeedPtr> retry;
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        for (const auto &feed : ctx->feeds) {
            if (!feed) continue;
            std::lock_guard<std::mutex> feed_lock(feed->mtx);
            // frame_epoch is the epoch of the last accepted frame and is not
            // cleared when the compositor takes the pixels, so this stays true
            // for a healthy tile and flips back to "silent" on every repoint.
            if (feed->alive && !feed->slot.frame_is_current(feed->frame_epoch))
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
}

// ── Compositing ──────────────────────────────────────────────────────────────

// Moves the slot's newest frame into worker-owned scratch, if there is one.
// The lock is held only for an O(1) buffer swap — never across the blit, which
// would head-of-line block the engine reader thread that feeds every source in
// the plugin. The swap hands our previous buffer back to the reader to refill,
// so neither side allocates after warm-up.
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
        // the outgoing participant survives an extra composite.
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

// Cover-crops the scratch frame into the tile rect. No locks are held here.
static void blit_tile(const TileScratch &scratch, uint8_t *y_plane,
                      uint8_t *u_plane, uint8_t *v_plane, uint32_t canvas_w,
                      uint32_t dst_x, uint32_t dst_y, uint32_t dst_w,
                      uint32_t dst_h)
{
    const uint32_t src_w = scratch.width;
    const uint32_t src_h = scratch.height;
    const size_t y_len = static_cast<size_t>(src_w) * src_h;

    // Sample the largest centred 16:9 sub-rectangle so the tile fills
    // completely and is never letterboxed.
    const CropRect crop = solve_cover_crop(static_cast<double>(src_w),
                                           static_cast<double>(src_h),
                                           kTileAspect);
    uint32_t crop_w = std::max<uint32_t>(2, even_floor(crop.width));
    uint32_t crop_h = std::max<uint32_t>(2, even_floor(crop.height));
    crop_w = std::min(crop_w, src_w);
    crop_h = std::min(crop_h, src_h);
    const uint32_t crop_x = std::min(even_floor(crop.x), src_w - crop_w);
    const uint32_t crop_y = std::min(even_floor(crop.y), src_h - crop_h);

    const uint8_t *src_y = scratch.pixels.data();
    const uint8_t *src_u = src_y + y_len;
    const uint8_t *src_v = src_u + y_len / 4;
    const uint32_t src_stride_uv = src_w / 2;

    blit_plane_nearest(src_y, src_w, crop_x, crop_y, crop_w, crop_h,
                       y_plane, canvas_w, dst_x, dst_y, dst_w, dst_h);
    blit_plane_nearest(src_u, src_stride_uv, crop_x / 2, crop_y / 2,
                       crop_w / 2, crop_h / 2,
                       u_plane, canvas_w / 2, dst_x / 2, dst_y / 2,
                       dst_w / 2, dst_h / 2);
    blit_plane_nearest(src_v, src_stride_uv, crop_x / 2, crop_y / 2,
                       crop_w / 2, crop_h / 2,
                       v_plane, canvas_w / 2, dst_x / 2, dst_y / 2,
                       dst_w / 2, dst_h / 2);
}

static void composite_once(tiles_source *ctx)
{
    // Copy-assign into a reused vector rather than constructing one: this runs
    // every frame, and allocation churn in the composite path is exactly what
    // caused the documented operator-stutter incident.
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->feeds_snapshot = ctx->feeds;
    }
    const std::vector<TileFeedPtr> &feeds = ctx->feeds_snapshot;
    const uint32_t canvas_w = ctx->canvas_width.load(std::memory_order_acquire);
    const uint32_t canvas_h = ctx->canvas_height.load(std::memory_order_acquire);
    if (canvas_w < 2 || canvas_h < 2) return;

    const size_t y_size = static_cast<size_t>(canvas_w) * canvas_h;
    const size_t total = y_size + y_size / 2;

    // The buffer is reused across frames: allocation churn at 4K is the
    // documented root cause of the operator stutter incident in this project.
    // A full clear is only needed when the layout changes, because every tile
    // overwrites its own rect completely and empty rects are painted below.
    bool clear_all = false;
    if (ctx->canvas_buf.size() < total) {
        ctx->canvas_buf.resize(total);
        clear_all = true;
    }
    if (ctx->buf_width != canvas_w || ctx->buf_height != canvas_h ||
        ctx->buf_tiles != feeds.size()) {
        ctx->buf_width = canvas_w;
        ctx->buf_height = canvas_h;
        ctx->buf_tiles = feeds.size();
        clear_all = true;
    }
    // Grow-only: a slot that comes back reuses its buffer instead of allocating.
    if (ctx->scratches.size() < feeds.size()) ctx->scratches.resize(feeds.size());

    uint8_t *y_plane = ctx->canvas_buf.data();
    uint8_t *u_plane = y_plane + y_size;
    uint8_t *v_plane = u_plane + y_size / 4;
    if (clear_all) {
        std::memset(y_plane, kNeutralY, y_size);
        // U and V are contiguous and share the same neutral value.
        std::memset(u_plane, kNeutralUV, y_size / 2);
    }

    TileGridParams params;
    params.canvas_width  = static_cast<double>(canvas_w);
    params.canvas_height = static_cast<double>(canvas_h);
    params.tile_aspect   = kTileAspect;
    params.gutter        = static_cast<double>(canvas_h) / kSpacingDivisor;
    params.margin        = params.gutter;

    // Snapping happens once, for the whole grid, so every gutter is identical;
    // see snap_tile_grid_even() in zoom-tile-grid.h.
    const std::vector<SnappedTileRect> rects =
        snap_tile_grid_even(solve_tile_grid(feeds.size(), params), params);

    size_t drawn = 0;
    for (size_t i = 0; i < rects.size() && i < feeds.size(); ++i) {
        const SnappedTileRect &r = rects[i];
        if (r.width < 2 || r.height < 2) continue;

        if (tile_take_snapshot(feeds[i], ctx->scratches[i])) {
            blit_tile(ctx->scratches[i], y_plane, u_plane, v_plane, canvas_w,
                      r.x, r.y, r.width, r.height);
            ++drawn;
        } else {
            fill_tile_neutral(y_plane, u_plane, v_plane, canvas_w,
                              r.x, r.y, r.width, r.height);
        }
    }

    // Rate-limited so the rig can confirm the wall is live and see how many
    // tiles actually have a feed, without flooding the log.
    if (ctx->composited_frames == 0 || ctx->composited_frames % 300 == 0) {
        blog(LOG_INFO,
             "[obs-zoom-plugin] Tiles composite: canvas=%ux%u tiles=%zu with_video=%zu count=%llu",
             canvas_w, canvas_h, rects.size(), drawn,
             static_cast<unsigned long long>(ctx->composited_frames));
    }
    ++ctx->composited_frames;

    obs_source_frame frame = {};
    frame.format = VIDEO_FORMAT_I420;
    frame.width  = canvas_w;
    frame.height = canvas_h;
    frame.data[0] = y_plane;
    frame.data[1] = u_plane;
    frame.data[2] = v_plane;
    frame.linesize[0] = canvas_w;
    frame.linesize[1] = canvas_w / 2;
    frame.linesize[2] = canvas_w / 2;
    // Naive latest-frame selection by design in this phase: there is no shared
    // presentation clock yet, so the wall is stamped at emit time. Phase 2
    // replaces this with a real tile clock.
    frame.timestamp = os_gettime_ns();
    frame.full_range = true;
    video_format_get_parameters_for_format(VIDEO_CS_709, VIDEO_RANGE_FULL,
                                           frame.format, frame.color_matrix,
                                           frame.color_range_min,
                                           frame.color_range_max);
    obs_source_output_video(ctx->source, &frame);
}

static uint64_t output_frame_interval_ns()
{
    obs_video_info ovi = {};
    if (obs_get_video_info(&ovi) && ovi.fps_num > 0 && ovi.fps_den > 0) {
        return static_cast<uint64_t>(ovi.fps_den) * 1000000000ULL /
            static_cast<uint64_t>(ovi.fps_num);
    }
    return kDefaultFrameIntervalNs;
}

// Composites on a dedicated thread rather than in video_tick: a CPU I420
// composite of a 4K wall would otherwise run on the OBS graphics thread and
// stall rendering for every other source.
static void tiles_worker(tiles_source *ctx)
{
    os_set_thread_name("corevideo-tiles");
    while (ctx->running.load(std::memory_order_acquire)) {
        const uint64_t started = os_gettime_ns();
        composite_once(ctx);
        os_sleepto_ns(started + output_frame_interval_ns());
    }
}

static void tiles_worker_start(tiles_source *ctx)
{
    if (ctx->running.exchange(true, std::memory_order_acq_rel)) return;
    ctx->worker = std::thread(tiles_worker, ctx);
}

static void tiles_worker_stop(tiles_source *ctx)
{
    if (!ctx->running.exchange(false, std::memory_order_acq_rel)) return;
    if (ctx->worker.joinable()) ctx->worker.join();
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
static constexpr std::size_t kMaxTileSlots  = 9;
static constexpr std::size_t kMaxExcludes   = 3;

static std::string tile_prop_name(std::size_t slot)
{
    return "tile_" + std::to_string(slot);
}

static std::string exclude_prop_name(std::size_t slot)
{
    return "exclude_" + std::to_string(slot);
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

    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->fill_params = std::move(params);
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

static void tiles_source_destroy(void *data)
{
    auto *ctx = static_cast<tiles_source *>(data);
    {
        std::lock_guard<std::mutex> callback_lock(ctx->gate->mtx);
        ctx->gate->alive = false;
    }
    ZoomEngineClient::instance().remove_roster_callback(ctx);
    tiles_worker_stop(ctx);

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
    tiles_worker_start(ctx);
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
    // Join before retiring: the worker holds no feed reference afterwards.
    tiles_worker_stop(ctx);
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
static void add_roster_entries(obs_property_t *list)
{
    obs_property_list_add_int(list, obs_module_text("CoreVideoTiles.NoParticipant"), 0);
    for (const auto &p : ZoomEngineClient::instance().roster()) {
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

static void tiles_source_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "canvas_width",  1920);
    obs_data_set_default_int(settings, "canvas_height", 1080);
    obs_data_set_default_int(settings, PROP_FILL_MODE,
                             static_cast<long long>(TileFillMode::Auto));
    obs_data_set_default_int(settings, PROP_MAX_TILES,
                             static_cast<long long>(kMaxTileSlots));
    // Every chooser defaults to "none"; Auto mode fills the wall on its own.
    for (std::size_t i = 1; i <= kMaxExcludes; ++i)
        obs_data_set_default_int(settings, exclude_prop_name(i).c_str(), 0);
    for (std::size_t i = 1; i <= kMaxTileSlots; ++i)
        obs_data_set_default_int(settings, tile_prop_name(i).c_str(), 0);
}

static obs_properties_t *tiles_source_get_properties(void *data)
{
    auto *ctx = static_cast<tiles_source *>(data);
    obs_properties_t *props = obs_properties_create();

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
            label.c_str(), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT));
    }

    for (std::size_t i = 1; i <= kMaxTileSlots; ++i) {
        const std::string name = tile_prop_name(i);
        const std::string label =
            std::string(obs_module_text("CoreVideoTiles.Tile")) + " " +
            std::to_string(i);
        add_roster_entries(obs_properties_add_list(props, name.c_str(),
            label.c_str(), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT));
    }

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
    obs_data_release(visibility_settings);

    return props;
}

void zoom_supersource_register()
{
    obs_source_info info = {};
    info.id             = "corevideo_tiles_source";
    info.type           = OBS_SOURCE_TYPE_INPUT;
    info.output_flags   = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name       = tiles_source_get_name;
    info.create         = tiles_source_create;
    info.destroy        = tiles_source_destroy;
    info.update         = tiles_source_update;
    info.show           = tiles_source_show;
    info.hide           = tiles_source_hide;
    info.get_width      = tiles_source_get_width;
    info.get_height     = tiles_source_get_height;
    info.get_properties = tiles_source_get_properties;
    info.get_defaults   = tiles_source_get_defaults;
    obs_register_source(&info);
}
