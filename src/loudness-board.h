#pragma once

// The preshow readiness board: one row per panelist, showing how far their
// loudness sits from the panel's, and whether that is acceptable.
//
// WHY THE HEADLINE NUMBER IS RELATIVE. An operator running a mic check does
// not primarily care that a panelist hits -23 LUFS. They care that panelist A
// is not 6 LU louder than panelist B, because that is what the audience
// hears. So the reference defaults to the panel's own MEDIAN gated integrated
// loudness and the number on each row is a deviation in LU. Median, never
// mean: one person on a laptop mic at -35 LUFS would drag a mean far enough
// to fail everybody else, which is the opposite of useful.
//
// WHY THE LAYOUT MATHS IS IN HERE TOO. This repo has no headless GPU harness
// and has ruled against building one -- an offscreen Qt harness certified the
// Talkback dock's layout three times and was wrong three times. The sanctioned
// approach is to extract the decision into a pure header and unit-test that,
// the way tests/tile-shape-test.cpp reproduces the tile shader's crop
// arithmetic in plain C++. So the row and bar rectangles are decided here and
// the renderer only fills them.
//
// Pure: no libobs, no Qt, no Zoom SDK.

#include "audio-subscription-state.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// One live audio source's measurement, flattened for the board. Every "has_"
// flag is load-bearing: "this panelist has not produced a measurable check"
// is a different statement from any loudness value, and collapsing it to a
// sentinel number is how a board ends up confidently reporting -70 LUFS for
// somebody who simply has not spoken yet.
//
// `kind` is why this struct exists rather than just handing the board a
// vector of doubles. Three kinds of CoreVideo audio source can feed a
// LoudnessMeter (src/audio-subscription-state.h): Participant, ActiveSpeaker
// (a resolved duplicate of whoever is currently talking) and Audience (the
// whole-meeting mix, no participant id at all). Only Participant readings may
// vote on the panel median -- see loudness_panel_median() below -- and only
// Participant readings are shown as rows at all -- see loudness_board_build()
// -- because an ActiveSpeaker row is a duplicate that shifts the reference on
// every speaker change and an Audience row has no panelist to describe.
struct LoudnessReading {
    std::string        source_uuid;
    std::string        display_name;
    uint32_t            participant_id  = 0;
    CoreVideoAudioKind kind             = CoreVideoAudioKind::Participant;
    bool        subscribed      = false;
    bool        has_short_term  = false;
    double      short_term_lufs = 0.0;
    bool        has_integrated  = false;
    double      integrated_lufs = 0.0;
    uint64_t    gated_blocks    = 0;
};

// Default: the panel's own median. The presets exist because a show sometimes
// has a delivery spec, but matching each other is the actual goal here, which
// is why PanelMedian is first and is the default.
enum class LoudnessReference {
    PanelMedian = 0,
    EbuR128     = 1,   // -23 LUFS
    AtscA85     = 2,   // -24 LKFS
    Streaming   = 3,   // -16 LUFS
};

enum class LoudnessRowStatus {
    NoAudio   = 0,   // subscribed to nobody, or nobody has spoken
    Measuring = 1,   // audible, but not enough gated blocks for a verdict
    Pass      = 2,
    Loud      = 3,
    Quiet     = 4,
};

// Minimum gated blocks before a reading is treated as a check rather than a
// noise. At a 100 ms hop this is 3 s of gated speech; the spec's 20 s mic
// check yields roughly 200. Set low enough that an operator sees a verdict
// while the panelist is still talking, high enough that a cough or a chair
// scrape cannot set the panel reference for everybody.
constexpr uint64_t kLoudnessBoardMinBlocks = 30;

// +/- this many LU from the reference still passes. 2 LU is below the ~3 LU
// step most listeners call "noticeably louder", so a passing board really is
// a matched panel.
constexpr double kLoudnessBoardDefaultToleranceLu = 2.0;

// The deviation at which the bar is full. Beyond it the bar clamps rather
// than growing, because past 6 LU the exact number stops mattering: the
// answer is already "fix this microphone".
constexpr double kLoudnessBoardFullScaleLu = 6.0;

constexpr int kLoudnessBoardHeaderPx = 28;
constexpr int kLoudnessBoardRowGapPx = 4;

struct LoudnessBoardRow {
    std::string       name;
    std::string       detail;          // short status text for the row
    bool              has_deviation  = false;
    double            deviation_lu   = 0.0;
    bool              has_short_term = false;
    double            short_term_lufs = 0.0;
    // Short-term deviation from the panel reference, i.e. short_term_lufs -
    // reference. Populated only when the row ALSO has a real integrated
    // verdict (`has_deviation`, i.e. cleared min_blocks against an
    // established reference) -- loudness_meter_short_term() is UNGATED, so a
    // silent or barely-started source reads its own noise floor as a huge,
    // meaningless deviation, and this field must not carry that onto the
    // board. This is what loudness_board_bar_input() prefers when it exists:
    // the bar is meant to move live while the panelist talks, and short-term
    // is the fast measure that does that; the row TEXT stays on the
    // integrated verdict.
    bool              has_short_term_deviation = false;
    double            short_term_deviation_lu  = 0.0;
    bool              has_integrated = false;
    double            integrated_lufs = 0.0;
    LoudnessRowStatus status = LoudnessRowStatus::NoAudio;
};

// What the deviation BAR should be driven from for this row: short-term
// deviation when it exists (the fast, live-moving measure), falling back to
// the integrated deviation the bar has always used when it does not --
// falling back rather than vanishing, so a panelist between hops still shows
// a bar. Returns false only when neither exists, e.g. before a reference has
// ever been established.
//
// Deliberately NOT part of LoudnessBoardModel::signature: the signature
// drives child TEXT-source rebuilds (obs_source_update(), one per row, per
// CLAUDE.md's note on the churn that caused), and short-term moves roughly
// every 100 ms hop -- folding it in would rebuild every row's text children
// ~10x/sec for a value that never reaches the text. Bars are geometry the
// renderer fills every frame regardless, so reading this per-frame costs
// nothing extra.
inline bool loudness_board_bar_input(const LoudnessBoardRow &row, double *out_lu)
{
    if (row.has_short_term_deviation) {
        *out_lu = row.short_term_deviation_lu;
        return true;
    }
    if (row.has_deviation) {
        *out_lu = row.deviation_lu;
        return true;
    }
    return false;
}

struct LoudnessBoardModel {
    bool              has_reference  = false;
    double            reference_lufs = 0.0;
    LoudnessReference reference_kind = LoudnessReference::PanelMedian;
    std::vector<LoudnessBoardRow> rows;
    // Changes only when something an operator can SEE changed. The consumer
    // rebuilds its child text sources off this, and the Talkback dock's
    // 2026-08-29 live defect -- a merely reordered roster rebuilding the
    // whole widget list several times a second and eating the operator's
    // clicks -- is why it is derived from sorted content and never from
    // input order.
    std::string signature;
};

// The fixed presets. PanelMedian has no fixed value and returns false.
inline bool loudness_reference_fixed_target(LoudnessReference kind, double *out)
{
    switch (kind) {
    case LoudnessReference::EbuR128:   *out = -23.0; return true;
    case LoudnessReference::AtscA85:   *out = -24.0; return true;
    case LoudnessReference::Streaming: *out = -16.0; return true;
    case LoudnessReference::PanelMedian:
    default:                           return false;
    }
}

// Median of the gated integrated loudness of everyone who has actually
// produced a check. Even counts average the two middle values, which is the
// ordinary definition and keeps a two-person panel from arbitrarily electing
// one of them as the reference.
//
// Only Participant-kind readings vote. ActiveSpeaker resolves to whichever
// Participant is currently talking, so counting it too is counting that
// person twice and shifting the reference by up to a full LU depending on an
// arbitrary even/odd flip; Audience is the whole-meeting mix and describes no
// one microphone at all. Both are display concerns, not reference concerns --
// see loudness_board_build(), which excludes them from rows entirely for the
// same reason.
inline bool loudness_panel_median(const std::vector<LoudnessReading> &readings,
                                  uint64_t min_blocks, double *out)
{
    std::vector<double> values;
    values.reserve(readings.size());
    for (const LoudnessReading &r : readings) {
        if (r.kind != CoreVideoAudioKind::Participant) continue;
        if (!r.has_integrated) continue;
        if (r.gated_blocks < min_blocks) continue;
        if (!std::isfinite(r.integrated_lufs)) continue;
        values.push_back(r.integrated_lufs);
    }
    if (values.empty()) return false;
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    *out = (n % 2 == 1) ? values[n / 2]
                        : 0.5 * (values[n / 2 - 1] + values[n / 2]);
    return true;
}

inline const char *loudness_row_status_text(LoudnessRowStatus s)
{
    switch (s) {
    case LoudnessRowStatus::NoAudio:   return "no audio";
    case LoudnessRowStatus::Measuring: return "measuring";
    case LoudnessRowStatus::Pass:      return "ok";
    case LoudnessRowStatus::Loud:      return "too loud";
    case LoudnessRowStatus::Quiet:     return "too quiet";
    default:                           return "";
    }
}

inline LoudnessBoardModel loudness_board_build(
    const std::vector<LoudnessReading> &readings,
    LoudnessReference kind, double tolerance_lu, uint64_t min_blocks)
{
    LoudnessBoardModel model;
    model.reference_kind = kind;
    if (!(tolerance_lu > 0.0)) tolerance_lu = kLoudnessBoardDefaultToleranceLu;

    double reference = 0.0;
    if (loudness_reference_fixed_target(kind, &reference)) {
        // A fixed target does not depend on the panel, so it survives a panel
        // nobody has spoken on. The median does not, and must not be invented.
        model.has_reference  = true;
        model.reference_lufs = reference;
    } else if (loudness_panel_median(readings, min_blocks, &reference)) {
        model.has_reference  = true;
        model.reference_lufs = reference;
    }

    // Ordered by CONTENT alone -- name, then uuid to break a duplicate-name
    // tie -- so a roster that merely reorders produces an identical board.
    //
    // Only Participant-kind readings become rows. ActiveSpeaker is a resolved
    // duplicate of whichever Participant is currently talking (a second row
    // for the same person), and Audience has no participant id and so no
    // display name -- both used to reach this board and either duplicate a
    // real panelist's row or render as a phantom "- unassigned -" entry.
    // Filtering here, in the pure header, rather than in the OBS glue that
    // calls it: this repo's convention is that pure logic gets unit tests and
    // OBS glue does not, and which readings become rows is exactly the kind
    // of decision this file exists to hold so it can be pinned without a
    // meeting.
    std::vector<const LoudnessReading *> ordered;
    ordered.reserve(readings.size());
    for (const LoudnessReading &r : readings) {
        if (r.kind != CoreVideoAudioKind::Participant) continue;
        ordered.push_back(&r);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const LoudnessReading *a, const LoudnessReading *b) {
                  if (a->display_name != b->display_name)
                      return a->display_name < b->display_name;
                  return a->source_uuid < b->source_uuid;
              });

    model.rows.reserve(ordered.size());
    for (const LoudnessReading *r : ordered) {
        LoudnessBoardRow row;
        row.name = r->display_name.empty()
                       ? (r->participant_id != 0
                              ? "ID " + std::to_string(r->participant_id)
                              : std::string("- unassigned -"))
                       : r->display_name;
        row.has_short_term  = r->has_short_term;
        row.short_term_lufs = r->short_term_lufs;
        row.has_integrated  = r->has_integrated;
        row.integrated_lufs = r->integrated_lufs;

        if (!r->has_integrated || r->gated_blocks == 0) {
            row.status = LoudnessRowStatus::NoAudio;
        } else if (r->gated_blocks < min_blocks) {
            row.status = LoudnessRowStatus::Measuring;
        } else if (model.has_reference) {
            row.has_deviation = true;
            row.deviation_lu  = r->integrated_lufs - model.reference_lufs;
            if (row.deviation_lu > tolerance_lu)
                row.status = LoudnessRowStatus::Loud;
            else if (row.deviation_lu < -tolerance_lu)
                row.status = LoudnessRowStatus::Quiet;
            else
                row.status = LoudnessRowStatus::Pass;   // boundary is inclusive
        } else {
            row.status = LoudnessRowStatus::Measuring;
        }
        row.detail = loudness_row_status_text(row.status);

        // Short-term deviation is gated on the row already having a REAL
        // VERDICT -- the same condition that sets `has_deviation` above
        // (Pass/Loud/Quiet, i.e. cleared min_blocks against an established
        // reference) -- NOT merely on a short-term reading existing.
        // loudness_meter_short_term() is UNGATED -- a subscribed-but-silent
        // source reads its own noise floor (e.g. -90 LUFS), which against a
        // ~-22 LUFS panel reference is a ~-68 LU deviation, clamped to full
        // scale. Without this gate a silent preshow -- this board's NORMAL
        // state, and a panelist who has merely started talking but not yet
        // cleared min_blocks (Measuring) -- would paint a full-length bar
        // pegged hard left, in idle grey, right next to text reading "no
        // audio" or "measuring": it reads as "far too quiet" when the truth
        // is "not measured at all". Piggybacking on `has_deviation` rather
        // than restating its condition also means loudness_board_bar_input()'s
        // integrated fallback is always available whenever the short-term
        // one is populated -- the bar can still fall back, never vanish.
        if (row.has_deviation && row.has_short_term &&
            std::isfinite(row.short_term_lufs)) {
            row.has_short_term_deviation = true;
            row.short_term_deviation_lu =
                row.short_term_lufs - model.reference_lufs;
        }
        model.rows.push_back(std::move(row));
    }

    // Deviation is quantised to 0.1 LU in the signature: the renderer prints
    // one decimal place, so a change smaller than that is invisible and must
    // not cost a text-source rebuild.
    std::string sig;
    sig.reserve(model.rows.size() * 24 + 16);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "R%d:%s%.1f|",
                  static_cast<int>(kind), model.has_reference ? "" : "x",
                  model.has_reference ? model.reference_lufs : 0.0);
    sig += buf;
    for (const LoudnessBoardRow &row : model.rows) {
        sig += row.name;
        std::snprintf(buf, sizeof(buf), "|%d|%s%.1f;",
                      static_cast<int>(row.status),
                      row.has_deviation ? "" : "x",
                      row.has_deviation ? row.deviation_lu : 0.0);
        sig += buf;
    }
    model.signature = std::move(sig);
    return model;
}

struct LoudnessBoardRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// One row's band. A zero-size result means "do not draw", which is what every
// degenerate input produces -- the renderer checks w/h rather than
// re-validating the arguments it just passed in.
inline LoudnessBoardRect loudness_board_row_rect(int canvas_w, int canvas_h,
                                                 size_t row_count,
                                                 size_t row_index)
{
    LoudnessBoardRect r;
    if (canvas_w <= 0 || canvas_h <= 0 || row_count == 0 ||
        row_index >= row_count)
        return r;
    const int body_top = kLoudnessBoardHeaderPx;
    const int body_h   = canvas_h - body_top;
    if (body_h <= 0) return r;
    const int slot = body_h / static_cast<int>(row_count);
    if (slot <= 0) return r;
    const int h = slot - kLoudnessBoardRowGapPx;
    r.x = 0;
    r.w = canvas_w;
    r.y = body_top + slot * static_cast<int>(row_index);
    r.h = (h > 0) ? h : slot;
    return r;
}

// The deviation bar, growing right (louder) or left (quieter) from the centre
// of the row's right half. Clamped at full scale rather than allowed to run
// off the canvas: past 6 LU the exact number has stopped mattering.
inline LoudnessBoardRect loudness_board_bar_rect(const LoudnessBoardRect &row,
                                                 double deviation_lu,
                                                 double full_scale_lu)
{
    LoudnessBoardRect r;
    if (row.w <= 0 || row.h <= 0 || !(full_scale_lu > 0.0)) return r;
    const int meter_w = row.w / 2;            // right half is the meter
    const int meter_x = row.x + row.w - meter_w;
    const int half    = meter_w / 2;
    const int centre  = meter_x + half;

    double d = deviation_lu;
    if (!std::isfinite(d)) d = 0.0;
    if (d >  full_scale_lu) d =  full_scale_lu;
    if (d < -full_scale_lu) d = -full_scale_lu;

    const int len = static_cast<int>(std::fabs(d) / full_scale_lu *
                                     static_cast<double>(half) + 0.5);
    r.y = row.y;
    r.h = row.h;
    r.w = len;
    r.x = (d >= 0.0) ? centre : centre - len;
    return r;
}

// The shortest row that is still a readiness board rather than a texture: a
// name and a number at a size an operator reads across a control room, plus
// the gap. A 25-person Zoom Events room would otherwise produce 13 px rows.
constexpr int kLoudnessBoardMinRowPx = 24;

// How many rows this canvas can actually show. Beyond it the renderer draws
// the first N (which, because rows are name-ordered, is stable frame to frame
// rather than shuffling) and says so in the header band.
inline size_t loudness_board_visible_rows(int canvas_h, size_t row_count)
{
    if (canvas_h <= kLoudnessBoardHeaderPx || row_count == 0) return 0;
    const int body_h = canvas_h - kLoudnessBoardHeaderPx;
    const size_t capacity =
        static_cast<size_t>(body_h / kLoudnessBoardMinRowPx);
    if (capacity == 0) return 0;
    return row_count < capacity ? row_count : capacity;
}

// Whether the meter source's already-applied child text labels need
// refreshing. `signature` captures the panel's own content -- reference,
// names, statuses, quantised deviations -- but NOT how many rows are
// currently being DRAWN: `shown` is a function of the canvas, not the
// panel, via loudness_board_visible_rows() above. A bare signature
// comparison misses exactly the case that matters most for this board:
// resizing the source taller reveals rows that were previously off-screen
// (and so still hold an empty applied string) while every panelist sits
// unchanged at "no audio" during a silent preshow -- the signature never
// moves, so a refresh gated on it alone would leave those newly-visible
// rows blank until the next status change happened to fix it. Comparing
// `shown` as well means either changing alone forces a refresh.
inline bool loudness_board_needs_label_refresh(
    const std::string &applied_signature, size_t applied_shown,
    const std::string &signature, size_t shown)
{
    return applied_signature != signature || applied_shown != shown;
}
