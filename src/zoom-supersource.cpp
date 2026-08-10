#include "zoom-supersource.h"
#include "engine-ipc.h"
#include "zoom-engine-client.h"
#include "zoom-tile-fill.h"
#include "zoom-tile-grid.h"
#include "zoom-tile-retry.h"
#include "zoom-tile-slot.h"
#include "zoom-tile-texture.h"
#include "zoom-tiles-effect.h"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Tiles are always 16:9 and never bordered (standing product rule).
static constexpr double kTileAspect = 16.0 / 9.0;
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

    // Owned solely by the OBS graphics thread (video_render) — no locking
    // needed. Copy-assigned into rather than constructed per frame: allocation
    // churn on a 60 Hz draw path is the documented root cause of the
    // operator-stutter incident.
    std::vector<TileFeedPtr> render_feeds;
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
// the new size. It discards no pixels: `frame` (the decoded copy the draw path
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

// Binds one tile's three planes and opens a pass for them.
//
// THE BINDING MUST HAPPEN BEFORE THE PASS OPENS, AND EVERY TILE NEEDS ITS OWN
// PASS. gs_technique_begin_pass() uploads the effect's parameters to the shader
// (upload_parameters(), libobs/graphics/effect.c:209) and nothing re-uploads
// them per draw call. Each tile binds *different* textures, so rebinding inside
// an already-open pass silently draws the previously-bound planes — or nothing.
// (The alternative is one pass plus gs_effect_update_params() after every
// rebind; a pass per tile is the same cost and much harder to get wrong.)
//
// Returns false when the pass could not be opened, in which case the draw must
// be skipped: gs_technique_begin_pass() loads the pass's vertex and pixel
// shaders, so drawing after a failed call runs against whatever shader was
// loaded last — a wall drawn with some other source's effect.
static bool tiles_begin_pass(gs_technique_t *tech, gs_texture_t *y,
                             gs_texture_t *u, gs_texture_t *v)
{
    gs_effect_set_texture(s_tiles_effect.param_y, y);
    gs_effect_set_texture(s_tiles_effect.param_u, u);
    gs_effect_set_texture(s_tiles_effect.param_v, v);
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
static void tiles_draw_neutral(gs_technique_t *tech, uint32_t x, uint32_t y,
                               uint32_t w, uint32_t h)
{
    if (!tiles_begin_pass(tech, s_neutral_y, s_neutral_u, s_neutral_v)) return;
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

    gs_technique_t *tech = s_tiles_effect.tech_i420;
    gs_technique_begin(tech);

    // PARITY-CRITICAL, DO NOT DELETE AS A REDUNDANT DRAW. The CPU compositor
    // memset the whole canvas to kNeutralY/kNeutralUV before drawing any tile,
    // so the gutters, the margins and any unfilled area were neutral grey — not
    // transparent. Painting the canvas here reproduces that exactly, and through
    // the same 1x1 0x80 textures and the same I420 technique the tiles use, so
    // it is bit-identical to the old fill by construction rather than by an RGB
    // constant somebody has to trust.
    //
    // It looks redundant now that the tiles carry participant video and cover
    // their own rects opaquely. It is not: the gutters and margins are never
    // covered by a tile, and they are exactly where the parity baseline in
    // docs/design-reference/tiles-gpu-parity/ observes the neutral 0x80.
    tiles_draw_neutral(tech, 0, 0, canvas_w, canvas_h);

    size_t drawn = 0;
    for (size_t i = 0; i < rects.size() && i < feeds.size(); ++i) {
        const SnappedTileRect &r = rects[i];
        if (r.width < 2 || r.height < 2) continue;  // as the CPU path skips them

        // Exactly the compositor's rule for "this tile has something to show":
        // tile_take_snapshot() already folds in TileSlotState's epoch check, so
        // a frame captured under a previous assignment is refused here for the
        // same reason it was refused there. No new rule is invented.
        const TileFeedPtr &feed = feeds[i];
        if (!feed || !tile_take_snapshot(feed, ctx->render_scratches[i]) ||
            !tile_upload_frame(feed, ctx->render_scratches[i])) {
            tiles_draw_neutral(tech, r.x, r.y, r.width, r.height);
            continue;
        }

        // Sample the largest centred 16:9 sub-rectangle so the tile fills
        // completely and is never letterboxed — the same solve_cover_crop() the
        // CPU blit used, mapped straight onto gs_draw_sprite_subregion(), which
        // samples exactly such a sub-rectangle.
        const CropRect crop = solve_cover_crop(static_cast<double>(feed->tex_w),
                                               static_cast<double>(feed->tex_h),
                                               kTileAspect);
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
            tiles_draw_neutral(tech, r.x, r.y, r.width, r.height);
            continue;
        }

        if (!tiles_begin_pass(tech, feed->tex_y, feed->tex_u, feed->tex_v))
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
    tiles_effect_destroy(s_tiles_effect);
}
