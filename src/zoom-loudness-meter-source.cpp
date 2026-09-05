#include "zoom-loudness-meter-source.h"

#include "loudness-board.h"
#include "zoom-participant-audio-source.h"
#include "zoom-tiles-effect.h"

#include <graphics/graphics.h>
#include <obs-module.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define PROP_REFERENCE  "reference"
#define PROP_TOLERANCE  "tolerance_lu"
#define PROP_WIDTH      "canvas_width"
#define PROP_HEIGHT     "canvas_height"
#define PROP_RESET      "btn_reset_windows"

static const char *kMeterSourceId = "corevideo_loudness_meter_source";

// Hard ceiling on child text sources, independent of the canvas cap in
// loudness_board_visible_rows(): each row costs two private sources and OBS
// renders every one of them, so the count is bounded by construction rather
// than by whatever canvas height an operator types in.
static constexpr size_t kMeterMaxRows = 16;

// 0xAARRGGBB, the same byte order picker_color_to_argb() produces for the
// Tiles wall, so gs_effect_set_color() reads them identically. Deliberately
// flat and high-contrast: this is read at a glance across a room, and the
// spec's legibility rule for anything meter-shaped is chunky segments and
// hard contrast, never hairlines.
static constexpr uint32_t kMeterBgArgb      = 0xFF12161Cu;
static constexpr uint32_t kMeterHeaderArgb  = 0xFF1E252Fu;
static constexpr uint32_t kMeterRowArgb     = 0xFF1A2029u;
static constexpr uint32_t kMeterCentreArgb  = 0xFF556070u;
static constexpr uint32_t kMeterPassArgb    = 0xFF2FBF6Fu;
static constexpr uint32_t kMeterLoudArgb    = 0xFFE04B4Bu;
static constexpr uint32_t kMeterQuietArgb   = 0xFFE0A03Cu;
static constexpr uint32_t kMeterIdleArgb    = 0xFF3A424Eu;

// Shared with the Tiles wall only by FILE PATH, not by handle -- and that is
// not the free dedupe it sounds like. gs_effect_create_from_file() allocates
// a brand-new gs_effect_t on every call; libobs does NOT cache or dedupe
// effects compiled from the same file. This second tiles_effect_load() call
// (the Tiles wall makes its own, separate one) compiles a second, independent
// copy of corevideo-tiles.effect. Harmless as written -- two handles, each
// destroyed exactly once by its own owner (this file's unload vs. the Tiles
// source's) -- but do NOT "deduplicate" the two loads into one shared
// gs_effect_t* on the strength of a caching story that isn't true: sharing a
// handle between two owners that each call gs_effect_destroy() on it once is
// a double-free at unload.
static TilesEffect s_meter_effect;
static bool s_meter_pass_failed_logged = false;

struct meter_row_widgets {
    obs_source_t *name  = nullptr;
    obs_source_t *value = nullptr;
    std::string   name_text;
    std::string   value_text;
};

struct loudness_meter_source {
    obs_source_t *source = nullptr;

    std::atomic<uint32_t> canvas_width{640};
    std::atomic<uint32_t> canvas_height{360};
    std::atomic<int>      reference{0};       // LoudnessReference
    std::atomic<int>      tolerance_milli_lu{2000};

    std::mutex         mutex;                 // guards `model` and `rows`
    LoudnessBoardModel model;
    std::string        applied_signature;
    // How many rows were applied alongside `applied_signature`. The
    // signature encodes reference/names/statuses/quantised deviations but
    // NOT `shown` -- `shown` depends on the CANVAS, not the panel -- so a
    // height change that newly reveals rows whose signature has not moved
    // (e.g. every row parked at "no audio" during a silent preshow, the
    // exact case this board exists for) must still trigger a refresh, or
    // those rows draw their band/chip/centre-line with no name and no value
    // until the next status change self-heals it.
    size_t              applied_shown = static_cast<size_t>(-1);
    meter_row_widgets  rows[kMeterMaxRows];

    float rebuild_accum = 0.0f;
};

// ── Text children ───────────────────────────────────────────────────────────
//
// The board needs real text: panelist display names come from Zoom and this
// project has already been burned by names like "Ronny Hofsoy, Tromso" with
// their real diacritics (the Talkback dock's 400 px tower). A hand-rolled
// bitmap font would reintroduce exactly that class of defect, so the labels
// are OBS's own text sources, created private to this source.
//
// The id is PROBED rather than assumed: OBS ships text_ft2 and text_gdiplus
// on different platforms and has renamed both across versions. A build with
// neither must lose the labels and keep the bars, loudly -- never render an
// empty board with no explanation.
static const char *probe_meter_text_source_id()
{
    const char *cached = nullptr;
    static const char *candidates[] = {
        "text_ft2_source_v2", "text_gdiplus_v3", "text_gdiplus_v2",
        "text_ft2_source",    "text_gdiplus",
    };
    for (const char *id : candidates) {
        // obs_get_source_output_flags() returns 0 for an id no module
        // registered; a text source always carries OBS_SOURCE_VIDEO.
        if (obs_get_source_output_flags(id) != 0) {
            cached = id;
            break;
        }
    }
    if (!cached) {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] CoreVideo Loudness Meter: no OBS text source "
             "module is available; the board will draw bars without labels");
    } else {
        blog(LOG_INFO,
             "[obs-zoom-plugin] CoreVideo Loudness Meter: labels will use "
             "text source '%s'",
             cached);
    }
    return cached;
}

static const char *meter_text_source_id()
{
    // Function-local static initialisation is thread-safe as of C++11 (the
    // standard guarantees the initializer runs exactly once even under a
    // race) -- load-bearing here, unlike the plain function-static
    // cached/probed pair this replaced: meter_create() can run on the UI
    // thread, a scene-load thread, or the control-API thread, and two
    // meters created concurrently on a fresh load raced that pair, with a
    // real chance of the probe (and its log line) running twice. "One log
    // line, not a stream" is a documented acceptance check for this source.
    static const char *id = probe_meter_text_source_id();
    return id;
}

static obs_source_t *make_text_child(const char *private_name, int px,
                                     uint32_t argb)
{
    const char *id = meter_text_source_id();
    if (!id) return nullptr;

    obs_data_t *settings = obs_data_create();
    obs_data_t *font     = obs_data_create();
    obs_data_set_string(font, "face", "Arial");
    obs_data_set_string(font, "style", "Bold");
    obs_data_set_int(font, "size", px);
    obs_data_set_int(font, "flags", 0);
    obs_data_set_obj(settings, "font", font);
    obs_data_set_string(settings, "text", "");
    // text_gdiplus uses "color"; text_ft2 uses "color1"/"color2". Setting all
    // three is harmless on either and avoids a per-id branch that would have
    // to be revisited every time OBS renames one.
    obs_data_set_int(settings, "color",  static_cast<long long>(argb));
    obs_data_set_int(settings, "color1", static_cast<long long>(argb));
    obs_data_set_int(settings, "color2", static_cast<long long>(argb));
    obs_data_release(font);

    obs_source_t *src = obs_source_create_private(id, private_name, settings);
    obs_data_release(settings);
    return src;
}

static void set_text_child(obs_source_t *src, const char *text)
{
    if (!src) return;
    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "text", text);
    obs_source_update(src, settings);
    obs_data_release(settings);
}

// ── Drawing ─────────────────────────────────────────────────────────────────

static void meter_fill_rect(const LoudnessBoardRect &r, uint32_t argb)
{
    if (r.w <= 0 || r.h <= 0) return;
    gs_technique_t *solid = s_meter_effect.tech_solid;
    if (!solid || !s_meter_effect.param_color) return;
    // The colour must be set BEFORE begin_pass: libobs uploads a pass's
    // parameters inside gs_technique_begin_pass() and does not re-upload them
    // for later draws in the same pass. Same rule the Tiles border uniforms
    // live under.
    gs_effect_set_color(s_meter_effect.param_color, argb);
    gs_technique_begin(solid);
    if (gs_technique_begin_pass(solid, 0)) {
        gs_matrix_push();
        gs_matrix_translate3f(static_cast<float>(r.x),
                              static_cast<float>(r.y), 0.0f);
        gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(r.w),
                       static_cast<uint32_t>(r.h));
        gs_matrix_pop();
        gs_technique_end_pass(solid);
    } else if (!s_meter_pass_failed_logged) {
        // Once only. A board that silently stops drawing looks like the
        // source went transparent, with no clue why.
        s_meter_pass_failed_logged = true;
        blog(LOG_ERROR,
             "[obs-zoom-plugin] CoreVideo Loudness Meter: "
             "gs_technique_begin_pass failed on the Solid technique; the "
             "board will not draw");
    }
    gs_technique_end(solid);
}

static uint32_t status_color(LoudnessRowStatus s)
{
    switch (s) {
    case LoudnessRowStatus::Pass:  return kMeterPassArgb;
    case LoudnessRowStatus::Loud:  return kMeterLoudArgb;
    case LoudnessRowStatus::Quiet: return kMeterQuietArgb;
    default:                       return kMeterIdleArgb;
    }
}

static std::string row_value_text(const LoudnessBoardRow &row)
{
    char buf[96];
    if (row.has_deviation) {
        if (row.has_integrated) {
            std::snprintf(buf, sizeof(buf), "%+.1f LU   %.1f LUFS   %s",
                          row.deviation_lu, row.integrated_lufs,
                          row.detail.c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "%+.1f LU   %s",
                          row.deviation_lu, row.detail.c_str());
        }
    } else {
        std::snprintf(buf, sizeof(buf), "%s", row.detail.c_str());
    }
    return std::string(buf);
}

static std::string header_text(const LoudnessBoardModel &m, size_t shown,
                               size_t total)
{
    char buf[160];
    const char *kind = "panel median";
    switch (m.reference_kind) {
    case LoudnessReference::EbuR128:   kind = "EBU R128";   break;
    case LoudnessReference::AtscA85:   kind = "ATSC A/85";  break;
    case LoudnessReference::Streaming: kind = "streaming";  break;
    case LoudnessReference::PanelMedian:
    default: break;
    }
    if (!m.has_reference) {
        std::snprintf(buf, sizeof(buf),
                      "MIC CHECK   reference: %s (waiting for a first check)",
                      kind);
    } else if (shown < total) {
        std::snprintf(buf, sizeof(buf),
                      "MIC CHECK   reference: %s  %.1f LUFS   showing %d of %d",
                      kind, m.reference_lufs, static_cast<int>(shown),
                      static_cast<int>(total));
    } else {
        std::snprintf(buf, sizeof(buf),
                      "MIC CHECK   reference: %s  %.1f LUFS",
                      kind, m.reference_lufs);
    }
    return std::string(buf);
}

// ── OBS callbacks ───────────────────────────────────────────────────────────

static const char *meter_get_name(void *)
{
    return obs_module_text("CoreVideoLoudnessMeter.Name");
}

static void meter_apply_settings(loudness_meter_source *ctx,
                                 obs_data_t *settings)
{
    uint32_t w = static_cast<uint32_t>(obs_data_get_int(settings, PROP_WIDTH));
    uint32_t h = static_cast<uint32_t>(obs_data_get_int(settings, PROP_HEIGHT));
    if (w < 160)  w = 160;
    if (w > 3840) w = 3840;
    if (h < 90)   h = 90;
    if (h > 2160) h = 2160;
    ctx->canvas_width.store(w, std::memory_order_release);
    ctx->canvas_height.store(h, std::memory_order_release);
    ctx->reference.store(static_cast<int>(
                             obs_data_get_int(settings, PROP_REFERENCE)),
                         std::memory_order_release);
    double tol = obs_data_get_double(settings, PROP_TOLERANCE);
    if (!(tol > 0.0)) tol = kLoudnessBoardDefaultToleranceLu;
    if (tol > 12.0) tol = 12.0;
    ctx->tolerance_milli_lu.store(static_cast<int>(tol * 1000.0 + 0.5),
                                  std::memory_order_release);
}

static void *meter_create(obs_data_t *settings, obs_source_t *source)
{
    auto *ctx = new loudness_meter_source();
    ctx->source = source;
    meter_apply_settings(ctx, settings);

    char private_name[64];
    for (size_t i = 0; i < kMeterMaxRows; ++i) {
        std::snprintf(private_name, sizeof(private_name),
                      "corevideo_meter_name_%d", static_cast<int>(i));
        ctx->rows[i].name = make_text_child(private_name, 20, 0xFFF2F5F8u);
        std::snprintf(private_name, sizeof(private_name),
                      "corevideo_meter_value_%d", static_cast<int>(i));
        ctx->rows[i].value = make_text_child(private_name, 20, 0xFFF2F5F8u);
    }
    return ctx;
}

static void meter_destroy(void *data)
{
    auto *ctx = static_cast<loudness_meter_source *>(data);
    for (size_t i = 0; i < kMeterMaxRows; ++i) {
        if (ctx->rows[i].name)  obs_source_release(ctx->rows[i].name);
        if (ctx->rows[i].value) obs_source_release(ctx->rows[i].value);
    }
    delete ctx;
}

static void meter_update(void *data, obs_data_t *settings)
{
    meter_apply_settings(static_cast<loudness_meter_source *>(data), settings);
}

static uint32_t meter_get_width(void *data)
{
    auto *ctx = static_cast<loudness_meter_source *>(data);
    return ctx->canvas_width.load(std::memory_order_acquire);
}

static uint32_t meter_get_height(void *data)
{
    auto *ctx = static_cast<loudness_meter_source *>(data);
    return ctx->canvas_height.load(std::memory_order_acquire);
}

static void meter_enum_active_sources(void *data,
                                      obs_source_enum_proc_t enum_callback,
                                      void *param)
{
    auto *ctx = static_cast<loudness_meter_source *>(data);
    for (size_t i = 0; i < kMeterMaxRows; ++i) {
        if (ctx->rows[i].name)  enum_callback(ctx->source, ctx->rows[i].name, param);
        if (ctx->rows[i].value) enum_callback(ctx->source, ctx->rows[i].value, param);
    }
}

// The model is rebuilt at 10 Hz, not per frame. corevideo_loudness_readings()
// takes g_sources_mtx and every source's own mutex -- the same mutex the
// audio lane holds for a whole drain -- so asking it 60 times a second would
// put the graphics thread in contention with the media path for no visible
// gain: the numbers it reports move on a 100 ms hop anyway.
//
// Also gated on obs_source_showing(): OBS calls video_tick for every source
// regardless of whether it is on a visible scene, so a meter parked in an
// unused scene would otherwise poll g_sources_mtx plus every live source's
// own ctx->mtx ten times a second forever. CLAUDE.md records the
// mirror-image defect on the Talkback dock's roster poll (a folded, hidden
// section still rebuilding at 10 Hz) -- same fix, same reasoning: a source
// nobody is looking at should cost nothing.
static void meter_video_tick(void *data, float seconds)
{
    auto *ctx = static_cast<loudness_meter_source *>(data);
    if (!obs_source_showing(ctx->source)) return;
    ctx->rebuild_accum += seconds;
    if (ctx->rebuild_accum < 0.1f) return;
    // Subtract the interval rather than zeroing: zeroing discards whatever
    // remainder pushed this tick over 0.1s, and at 60 fps (16.7 ms/frame)
    // that landed every 7 frames -- ~117 ms, an ~8.6 Hz cadence, not the
    // 10 Hz this comment (and the brief) claims. Subtracting keeps the
    // carried remainder so the average cadence is the documented 10 Hz.
    ctx->rebuild_accum -= 0.1f;

    const auto readings = corevideo_loudness_readings();
    const double tol =
        static_cast<double>(ctx->tolerance_milli_lu.load(
            std::memory_order_acquire)) / 1000.0;
    const auto kind = static_cast<LoudnessReference>(
        ctx->reference.load(std::memory_order_acquire));
    LoudnessBoardModel model = loudness_board_build(
        readings, kind, tol, kLoudnessBoardMinBlocks);

    std::lock_guard<std::mutex> lk(ctx->mutex);
    ctx->model = std::move(model);
}

static void meter_video_render(void *data, gs_effect_t *)
{
    auto *ctx = static_cast<loudness_meter_source *>(data);
    const int canvas_w =
        static_cast<int>(ctx->canvas_width.load(std::memory_order_acquire));
    const int canvas_h =
        static_cast<int>(ctx->canvas_height.load(std::memory_order_acquire));
    if (!s_meter_effect.valid()) return;

    LoudnessBoardModel model;
    {
        std::lock_guard<std::mutex> lk(ctx->mutex);
        model = ctx->model;
    }

    meter_fill_rect(LoudnessBoardRect{0, 0, canvas_w, canvas_h}, kMeterBgArgb);
    meter_fill_rect(LoudnessBoardRect{0, 0, canvas_w, kLoudnessBoardHeaderPx},
                    kMeterHeaderArgb);

    // The LAST slot is permanently the header's, never a panelist's, so a
    // change in row count cannot silently steal the header's text child --
    // hence the cap is kMeterMaxRows - 1 and not kMeterMaxRows.
    static constexpr size_t kMeterHeaderSlot = kMeterMaxRows - 1;
    const size_t total = model.rows.size();
    size_t shown = loudness_board_visible_rows(canvas_h, total);
    if (shown > kMeterHeaderSlot) shown = kMeterHeaderSlot;

    for (size_t i = 0; i < shown; ++i) {
        const LoudnessBoardRow &row = model.rows[i];
        const LoudnessBoardRect band =
            loudness_board_row_rect(canvas_w, canvas_h, shown, i);
        if (band.w <= 0 || band.h <= 0) continue;

        meter_fill_rect(band, kMeterRowArgb);

        // The status chip: a fat block at the left edge, which is the part
        // that reads first from across a room.
        meter_fill_rect(LoudnessBoardRect{band.x, band.y, 8, band.h},
                        status_color(row.status));

        // The zero line, drawn under the bar so a bar of zero width still
        // shows where the reference is.
        const LoudnessBoardRect zero =
            loudness_board_bar_rect(band, 0.0, kLoudnessBoardFullScaleLu);
        meter_fill_rect(LoudnessBoardRect{zero.x - 1, band.y, 2, band.h},
                        kMeterCentreArgb);

        // Driven by short-term deviation when it exists (the fast measure
        // that should move live while the panelist talks), falling back to
        // the integrated deviation otherwise -- see loudness_board_bar_input().
        // The row's pass/fail colour still comes from `row.status`, which is
        // the integrated verdict: only the bar's POSITION is live.
        double bar_dev_lu = 0.0;
        if (loudness_board_bar_input(row, &bar_dev_lu)) {
            const LoudnessBoardRect bar = loudness_board_bar_rect(
                band, bar_dev_lu, kLoudnessBoardFullScaleLu);
            meter_fill_rect(LoudnessBoardRect{bar.x, bar.y + 4, bar.w,
                                              bar.h > 8 ? bar.h - 8 : bar.h},
                            status_color(row.status));
        }
    }

    // Labels last, over the bars. Each child is only re-settings-updated when
    // its string changes: obs_source_update() allocates and takes the source's
    // own lock, and doing it per frame per row is the churn shape this project
    // already has a live incident about.
    //
    // Gated on `shown` changing as well as `model.signature`: the signature
    // encodes reference/names/statuses/quantised deviations but NOT `shown`,
    // which depends on the CANVAS. Growing the source's height can newly
    // reveal rows that previously held an empty string while the panel's own
    // signature has not moved (every row parked at "no audio" during a
    // silent preshow is exactly that case), and gating on the signature
    // alone would leave those rows drawing a band/chip/centre-line with no
    // text until the next status change happened to self-heal it.
    //
    // set_text_child() calls obs_source_update(), which takes libobs's own
    // source lock and allocates an obs_data_t -- neither belongs inside
    // ctx->mutex. What to apply is decided under the lock into a local
    // vector of (slot, text) pairs; the OBS calls happen after the lock is
    // released.
    const std::string head = header_text(model, shown, total);
    struct PendingLabel {
        obs_source_t *child;
        std::string   text;
    };
    std::vector<PendingLabel> pending;
    {
        std::lock_guard<std::mutex> lk(ctx->mutex);
        if (loudness_board_needs_label_refresh(ctx->applied_signature,
                                               ctx->applied_shown,
                                               model.signature, shown)) {
            ctx->applied_signature = model.signature;
            ctx->applied_shown     = shown;
            for (size_t i = 0; i < kMeterHeaderSlot; ++i) {
                const std::string name_text =
                    (i < shown) ? model.rows[i].name : std::string();
                const std::string value_text =
                    (i < shown) ? row_value_text(model.rows[i]) : std::string();
                if (ctx->rows[i].name_text != name_text) {
                    ctx->rows[i].name_text = name_text;
                    pending.push_back({ctx->rows[i].name, name_text});
                }
                if (ctx->rows[i].value_text != value_text) {
                    ctx->rows[i].value_text = value_text;
                    pending.push_back({ctx->rows[i].value, value_text});
                }
            }
        }
    }
    for (const PendingLabel &p : pending)
        set_text_child(p.child, p.text.c_str());

    for (size_t i = 0; i < shown; ++i) {
        const LoudnessBoardRect band =
            loudness_board_row_rect(canvas_w, canvas_h, shown, i);
        if (band.w <= 0 || band.h <= 0) continue;
        const int text_y = band.y + (band.h > 24 ? (band.h - 24) / 2 : 0);
        if (ctx->rows[i].name) {
            gs_matrix_push();
            gs_matrix_translate3f(static_cast<float>(band.x + 16),
                                  static_cast<float>(text_y), 0.0f);
            obs_source_video_render(ctx->rows[i].name);
            gs_matrix_pop();
        }
        if (ctx->rows[i].value) {
            gs_matrix_push();
            gs_matrix_translate3f(static_cast<float>(band.x + band.w / 2 + 8),
                                  static_cast<float>(text_y), 0.0f);
            obs_source_video_render(ctx->rows[i].value);
            gs_matrix_pop();
        }
    }

    // The header, in the slot reserved for it above. Updated on its own
    // string comparison rather than on the board signature, because the
    // "showing N of M" count changes with the CANVAS as well as the panel.
    if (ctx->rows[kMeterHeaderSlot].name) {
        obs_source_t *header = ctx->rows[kMeterHeaderSlot].name;
        bool needs_update = false;
        {
            std::lock_guard<std::mutex> lk(ctx->mutex);
            if (ctx->rows[kMeterHeaderSlot].name_text != head) {
                ctx->rows[kMeterHeaderSlot].name_text = head;
                needs_update = true;
            }
        }
        // Same rule as the row labels above: obs_source_update() must not
        // run while ctx->mutex is held.
        if (needs_update) set_text_child(header, head.c_str());
        gs_matrix_push();
        gs_matrix_translate3f(12.0f, 4.0f, 0.0f);
        obs_source_video_render(header);
        gs_matrix_pop();
    }
}

static bool meter_reset_clicked(obs_properties_t *, obs_property_t *, void *)
{
    corevideo_reset_loudness_windows();
    return false;
}

static obs_properties_t *meter_get_properties(void *)
{
    obs_properties_t *props = obs_properties_create();

    obs_property_t *ref = obs_properties_add_list(
        props, PROP_REFERENCE,
        obs_module_text("CoreVideoLoudnessMeter.Reference"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(
        ref, obs_module_text("CoreVideoLoudnessMeter.Reference.PanelMedian"),
        static_cast<int>(LoudnessReference::PanelMedian));
    obs_property_list_add_int(
        ref, obs_module_text("CoreVideoLoudnessMeter.Reference.R128"),
        static_cast<int>(LoudnessReference::EbuR128));
    obs_property_list_add_int(
        ref, obs_module_text("CoreVideoLoudnessMeter.Reference.A85"),
        static_cast<int>(LoudnessReference::AtscA85));
    obs_property_list_add_int(
        ref, obs_module_text("CoreVideoLoudnessMeter.Reference.Streaming"),
        static_cast<int>(LoudnessReference::Streaming));

    obs_properties_add_float_slider(
        props, PROP_TOLERANCE,
        obs_module_text("CoreVideoLoudnessMeter.Tolerance"), 0.5, 6.0, 0.5);
    obs_properties_add_int(props, PROP_WIDTH,
        obs_module_text("CoreVideoLoudnessMeter.Width"), 160, 3840, 10);
    obs_properties_add_int(props, PROP_HEIGHT,
        obs_module_text("CoreVideoLoudnessMeter.Height"), 90, 2160, 10);
    obs_properties_add_button(props, PROP_RESET,
        obs_module_text("CoreVideoLoudnessMeter.Reset"), meter_reset_clicked);
    return props;
}

static void meter_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, PROP_REFERENCE,
                             static_cast<int>(LoudnessReference::PanelMedian));
    obs_data_set_default_double(settings, PROP_TOLERANCE,
                                kLoudnessBoardDefaultToleranceLu);
    obs_data_set_default_int(settings, PROP_WIDTH, 640);
    obs_data_set_default_int(settings, PROP_HEIGHT, 360);
}

void corevideo_loudness_meter_source_register()
{
    obs_source_info info = {};
    info.id           = kMeterSourceId;
    info.type         = OBS_SOURCE_TYPE_INPUT;
    // CUSTOM_DRAW because it binds the plugin's own effect rather than
    // letting OBS draw one texture with the default one, exactly as the Tiles
    // wall does.
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
                        OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name     = meter_get_name;
    info.create       = meter_create;
    info.destroy      = meter_destroy;
    info.update       = meter_update;
    info.video_tick   = meter_video_tick;
    info.video_render = meter_video_render;
    info.get_width    = meter_get_width;
    info.get_height   = meter_get_height;
    info.enum_active_sources = meter_enum_active_sources;
    info.get_properties = meter_get_properties;
    info.get_defaults   = meter_get_defaults;
    obs_register_source(&info);
}

void corevideo_loudness_meter_load_gfx()
{
    tiles_effect_load(s_meter_effect);
}

void corevideo_loudness_meter_unload_gfx()
{
    tiles_effect_destroy(s_meter_effect);
    s_meter_pass_failed_logged = false;
}
