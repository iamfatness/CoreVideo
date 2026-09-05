// tests/loudness-board-test.cpp
// The readiness board: what an operator actually reads during a mic check.
//
// The product claim is relative, not absolute. An operator does not primarily
// care that a panelist hits -23 LUFS; they care that panelist A is not 6 LU
// louder than panelist B. So the headline number is deviation from the panel
// MEDIAN of gated integrated loudness -- median, because one panelist on a
// laptop mic at -35 LUFS must not drag the reference everyone else is judged
// against, which is exactly what a mean does.
//
// The layout arithmetic is pinned here too rather than looked at on screen.
// This repo has no headless GPU harness and has ruled against building one
// (an offscreen Qt harness "certified it three times and was wrong three
// times"); the sanctioned approach is to extract the decision into a pure
// header and unit-test that, the way tests/tile-shape-test.cpp reproduces the
// shader's crop arithmetic.
#include "loudness-board.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

static bool near(double a, double b, double tol)
{
    return std::fabs(a - b) <= tol;
}

static LoudnessReading measured(const char *name, double integrated,
                                double short_term = -20.0,
                                uint64_t blocks = 200)
{
    LoudnessReading r;
    r.source_uuid     = std::string("uuid_") + name;
    r.display_name    = name;
    r.participant_id  = 1;
    r.subscribed      = true;
    r.has_short_term  = true;
    r.short_term_lufs = short_term;
    r.has_integrated  = true;
    r.integrated_lufs = integrated;
    r.gated_blocks    = blocks;
    return r;
}

int main()
{
    // ── Median, not mean ───────────────────────────────────────────────────
    {
        std::vector<LoudnessReading> panel = {
            measured("Ana",   -18.0), measured("Ben",   -21.0),
            measured("Cara",  -23.0), measured("Dev",   -24.0),
            measured("Erik",  -30.0),
        };
        double median = 0.0;
        check(loudness_panel_median(panel, kLoudnessBoardMinBlocks, &median),
              "no panel median was produced from five measured panelists");
        check(near(median, -23.0, 1e-9),
              "the panel reference is not the median -- the mean of this "
              "panel is -23.2, and Erik at -30 is exactly the outlier the "
              "median exists to survive");
    }

    // ── An even panel averages the two middle values ───────────────────────
    {
        std::vector<LoudnessReading> panel = {
            measured("Ana", -18.0), measured("Ben", -21.0),
            measured("Cara", -23.0), measured("Dev", -24.0),
        };
        double median = 0.0;
        check(loudness_panel_median(panel, kLoudnessBoardMinBlocks, &median),
              "no median from an even-sized panel");
        check(near(median, -22.0, 1e-9),
              "an even-sized panel's median was not the mean of the two "
              "middle values");
    }

    // ── Unmeasured panelists must not vote on the reference ────────────────
    {
        LoudnessReading quiet;
        quiet.source_uuid  = "uuid_Fay";
        quiet.display_name = "Fay";
        quiet.subscribed   = true;
        // never spoke: no integrated value at all
        std::vector<LoudnessReading> panel = {
            measured("Ana", -18.0), measured("Ben", -22.0), quiet,
        };
        double median = 0.0;
        check(loudness_panel_median(panel, kLoudnessBoardMinBlocks, &median),
              "a panel with one silent member produced no median");
        check(near(median, -20.0, 1e-9),
              "a panelist with no integrated reading was counted in the "
              "median -- a person who has not spoken is not a data point");
    }

    // ── A too-short check does not count either ────────────────────────────
    {
        std::vector<LoudnessReading> panel = {
            measured("Ana", -18.0, -18.0, 200),
            measured("Ben", -22.0, -22.0, 200),
            measured("Cough", -5.0, -5.0, 4),   // four blocks: 400 ms
        };
        double median = 0.0;
        check(loudness_panel_median(panel, kLoudnessBoardMinBlocks, &median),
              "no median produced");
        check(near(median, -20.0, 1e-9),
              "a 400 ms cough set the panel reference -- the minimum gated "
              "block count is not being applied");
    }

    // ── No measurable panelist means NO reference, not zero ────────────────
    {
        std::vector<LoudnessReading> panel;
        double median = 0.0;
        check(!loudness_panel_median(panel, kLoudnessBoardMinBlocks, &median),
              "an empty panel produced a reference value");
    }

    // ── CRITICAL: only Participant-kind readings vote on the median ────────
    // ActiveSpeaker resolves to whichever Participant is currently talking
    // (a duplicate vote for that person) and Audience is the whole-meeting
    // mix (no one microphone at all). A median computed over a vector that
    // mixes all three kinds must equal the median of the Participants alone.
    {
        std::vector<LoudnessReading> participants_only = {
            measured("Ana", -18.0), measured("Ben", -21.0),
            measured("Cara", -23.0), measured("Dev", -24.0),
        };
        double participants_median = 0.0;
        check(loudness_panel_median(participants_only, kLoudnessBoardMinBlocks,
                                    &participants_median),
              "no median from the Participants-only panel");

        std::vector<LoudnessReading> mixed = participants_only;
        LoudnessReading speaker = measured("ActiveSpeakerDup", -21.0);
        speaker.kind = CoreVideoAudioKind::ActiveSpeaker;
        mixed.push_back(speaker);
        LoudnessReading audience = measured("WholeRoomMix", -12.0);
        audience.kind = CoreVideoAudioKind::Audience;
        audience.display_name.clear();
        audience.participant_id = 0;
        mixed.push_back(audience);

        double mixed_median = 0.0;
        check(loudness_panel_median(mixed, kLoudnessBoardMinBlocks,
                                    &mixed_median),
              "no median from the mixed-kind panel");
        check(near(mixed_median, participants_median, 1e-9),
              "an ActiveSpeaker or Audience reading voted on the panel "
              "median -- only Participant-kind readings may");

        // The board must not render ActiveSpeaker/Audience as rows either --
        // a duplicate row for the current speaker, or a phantom
        // "- unassigned -" row for the room mix, is the same defect wearing
        // a different face.
        LoudnessBoardModel model = loudness_board_build(
            mixed, LoudnessReference::PanelMedian,
            kLoudnessBoardDefaultToleranceLu, kLoudnessBoardMinBlocks);
        check(model.rows.size() == participants_only.size(),
              "a non-Participant reading produced a row on the board");
        for (const LoudnessBoardRow &row : model.rows) {
            check(row.name != "ActiveSpeakerDup" && row.name != "WholeRoomMix" &&
                  row.name != "- unassigned -",
                  "a non-Participant reading's name leaked onto the board");
        }
    }

    // ── A single qualifying panelist is always its own reference ───────────
    // With only one Participant clearing the minimum block count, the median
    // degenerates to that one value, so their own deviation is always 0 LU --
    // they must always pass, regardless of what absolute level they measured
    // at.
    {
        std::vector<LoudnessReading> panel = {
            measured("Solo", -41.0),   // far outside any EBU/ATSC/streaming target
        };
        LoudnessBoardModel model = loudness_board_build(
            panel, LoudnessReference::PanelMedian,
            kLoudnessBoardDefaultToleranceLu, kLoudnessBoardMinBlocks);
        check(model.has_reference, "a single qualifying panelist produced no reference");
        check(near(model.reference_lufs, -41.0, 1e-9),
              "a single panelist's own reading is not their own reference");
        check(model.rows.size() == 1, "wrong row count for a solo panelist");
        check(model.rows[0].has_deviation && near(model.rows[0].deviation_lu, 0.0, 1e-9),
              "a solo panelist's deviation from their own reference is not 0 LU");
        check(model.rows[0].status == LoudnessRowStatus::Pass,
              "a solo panelist -- always their own reference -- must always "
              "pass, at any absolute loudness level");
    }

    // ── Deviation sign, and status ─────────────────────────────────────────
    {
        std::vector<LoudnessReading> panel = {
            measured("Ana",  -18.0), measured("Ben",  -21.0),
            measured("Cara", -23.0), measured("Dev",  -24.0),
            measured("Erik", -30.0),
        };
        const LoudnessBoardModel m = loudness_board_build(
            panel, LoudnessReference::PanelMedian,
            kLoudnessBoardDefaultToleranceLu, kLoudnessBoardMinBlocks);
        check(m.has_reference && near(m.reference_lufs, -23.0, 1e-9),
              "the built model's reference is not the panel median");
        check(m.rows.size() == 5, "the board did not produce one row per panelist");
        // Rows are ordered by name from CONTENT alone.
        check(m.rows[0].name == "Ana" && m.rows[4].name == "Erik",
              "rows are not in deterministic name order");
        check(near(m.rows[0].deviation_lu, 5.0, 1e-9),
              "a panelist 5 LU above the median did not report +5 LU -- "
              "louder than the reference must be POSITIVE");
        check(near(m.rows[4].deviation_lu, -7.0, 1e-9),
              "a panelist 7 LU below the median did not report -7 LU");
        check(m.rows[0].status == LoudnessRowStatus::Loud,
              "+5 LU was not flagged as too loud at a 2 LU tolerance");
        check(m.rows[4].status == LoudnessRowStatus::Quiet,
              "-7 LU was not flagged as too quiet");
        check(m.rows[2].status == LoudnessRowStatus::Pass,
              "the panelist sitting exactly on the median did not pass");
        check(m.rows[1].status == LoudnessRowStatus::Pass,
              "-21 against a -23 median is +2 LU, exactly the tolerance, and "
              "must pass -- the boundary is inclusive");
    }

    // ── Fixed-target presets ───────────────────────────────────────────────
    {
        std::vector<LoudnessReading> panel = { measured("Ana", -18.0) };
        const LoudnessBoardModel r128 = loudness_board_build(
            panel, LoudnessReference::EbuR128, 2.0, kLoudnessBoardMinBlocks);
        check(r128.has_reference && near(r128.reference_lufs, -23.0, 1e-9),
              "EBU R128 preset is not -23 LUFS");
        check(near(r128.rows[0].deviation_lu, 5.0, 1e-9),
              "-18 against the R128 target is not +5 LU");

        const LoudnessBoardModel a85 = loudness_board_build(
            panel, LoudnessReference::AtscA85, 2.0, kLoudnessBoardMinBlocks);
        check(near(a85.reference_lufs, -24.0, 1e-9),
              "ATSC A/85 preset is not -24 LKFS");

        const LoudnessBoardModel str = loudness_board_build(
            panel, LoudnessReference::Streaming, 2.0, kLoudnessBoardMinBlocks);
        check(near(str.reference_lufs, -16.0, 1e-9),
              "the streaming preset is not -16 LUFS");
    }

    // ── A fixed target works with NOBODY measured; the median does not ─────
    {
        LoudnessReading silent;
        silent.source_uuid  = "uuid_Ana";
        silent.display_name = "Ana";
        silent.subscribed   = true;
        std::vector<LoudnessReading> panel = { silent };

        const LoudnessBoardModel med = loudness_board_build(
            panel, LoudnessReference::PanelMedian, 2.0, kLoudnessBoardMinBlocks);
        check(!med.has_reference,
              "a panel median was invented from a panel nobody has spoken on");
        check(med.rows.size() == 1 && !med.rows[0].has_deviation &&
              med.rows[0].status == LoudnessRowStatus::NoAudio,
              "a silent panelist was given a deviation");

        const LoudnessBoardModel fixed = loudness_board_build(
            panel, LoudnessReference::EbuR128, 2.0, kLoudnessBoardMinBlocks);
        check(fixed.has_reference,
              "a FIXED target disappeared because nobody had spoken -- the "
              "target does not depend on the panel");
        check(!fixed.rows[0].has_deviation,
              "a silent panelist got a deviation against a fixed target");
    }

    // ── Measuring: subscribed and audible, but not enough blocks yet ───────
    {
        std::vector<LoudnessReading> panel = {
            measured("Ana", -20.0, -20.0, 200),
            measured("Ben", -20.0, -20.0, 5),
        };
        const LoudnessBoardModel m = loudness_board_build(
            panel, LoudnessReference::PanelMedian, 2.0, kLoudnessBoardMinBlocks);
        check(m.rows[1].status == LoudnessRowStatus::Measuring,
              "a panelist with 5 gated blocks was given a verdict rather than "
              "reported as still measuring");
        check(!m.rows[1].has_deviation,
              "a still-measuring panelist was given a deviation");
    }

    // ── IMPORTANT 2: the deviation bar is driven by short-term deviation,
    // falling back to integrated deviation, never vanishing ─────────────────
    {
        // A row with both short-term and integrated deviation available:
        // the bar must prefer short-term.
        LoudnessBoardRow both;
        both.has_short_term_deviation = true;
        both.short_term_deviation_lu  = 1.5;
        both.has_deviation            = true;
        both.deviation_lu             = -3.0;
        double bar_lu = 0.0;
        check(loudness_board_bar_input(both, &bar_lu),
              "no bar input produced when both deviations were available");
        check(near(bar_lu, 1.5, 1e-9),
              "the bar did not prefer short-term deviation over integrated");

        // Short-term unavailable: falls back to integrated, not to nothing.
        LoudnessBoardRow integrated_only;
        integrated_only.has_deviation = true;
        integrated_only.deviation_lu  = -3.0;
        bar_lu = 0.0;
        check(loudness_board_bar_input(integrated_only, &bar_lu),
              "the bar vanished when short-term deviation was unavailable "
              "instead of falling back to integrated");
        check(near(bar_lu, -3.0, 1e-9),
              "the bar's fallback value was not the integrated deviation");

        // Neither available: no bar input at all.
        LoudnessBoardRow neither;
        bar_lu = 12345.0;
        check(!loudness_board_bar_input(neither, &bar_lu),
              "a bar input was produced with neither deviation available");
    }

    // ── loudness_board_build() populates short-term deviation, and it is NOT
    // part of the signature ─────────────────────────────────────────────────
    {
        std::vector<LoudnessReading> panel = {
            measured("Ana", -20.0, -20.0, 200),
            measured("Ben", -24.0, -18.0, 200),   // integrated -24, short-term -18
        };
        const LoudnessBoardModel m1 = loudness_board_build(
            panel, LoudnessReference::PanelMedian, 2.0, kLoudnessBoardMinBlocks);
        check(m1.has_reference, "no reference produced");
        // reference is the median of the two integrated values: -22.
        check(m1.rows[1].has_short_term_deviation,
              "Ben's short-term deviation was not populated");
        check(near(m1.rows[1].short_term_deviation_lu, -18.0 - m1.reference_lufs,
                   1e-9),
              "Ben's short-term deviation is not short_term_lufs - reference");

        // Moving ONLY the short-term value must not change the signature --
        // folding it in would rebuild every row's text children ~10x/sec for
        // a value the row text never shows (see loudness_board_bar_input()).
        panel[1].short_term_lufs = -10.0;
        const LoudnessBoardModel m2 = loudness_board_build(
            panel, LoudnessReference::PanelMedian, 2.0, kLoudnessBoardMinBlocks);
        check(m1.signature == m2.signature,
              "the board signature changed when only short-term loudness "
              "moved -- short-term must not force a text-child rebuild");
    }

    // ── REGRESSION B: an unmeasured row must draw NO bar, even with a
    // short-term reading ────────────────────────────────────────────────────
    // loudness_meter_short_term() is ungated: a subscribed-but-silent source
    // still reads its own noise floor (e.g. -90 LUFS), which against a real
    // panel reference is a huge, meaningless deviation. NoAudio (never
    // spoken / zero gated blocks) and Measuring (some gated blocks, not yet
    // enough for a verdict) must both refuse to populate
    // has_short_term_deviation, and loudness_board_bar_input() must refuse
    // to produce ANY bar for them -- a silent preshow is this board's normal
    // state, and it must not paint a full-length bar pegged hard left on
    // every row in it.
    {
        std::vector<LoudnessReading> panel = {
            measured("Ana", -20.0, -20.0, 200),   // establishes a reference
            measured("Ben", -20.0, -20.0, 200),
        };
        // NoAudio: subscribed, never spoken, but with a stray short-term
        // reading (the shape a noise-floor read would actually take).
        LoudnessReading silent_with_noise_floor;
        silent_with_noise_floor.source_uuid     = "uuid_Cara";
        silent_with_noise_floor.display_name    = "Cara";
        silent_with_noise_floor.subscribed      = true;
        silent_with_noise_floor.has_short_term  = true;
        silent_with_noise_floor.short_term_lufs = -90.0;
        silent_with_noise_floor.has_integrated  = false;
        silent_with_noise_floor.gated_blocks    = 0;
        panel.push_back(silent_with_noise_floor);

        // Measuring: a handful of gated blocks (well under
        // kLoudnessBoardMinBlocks) with a short-term reading that has not
        // yet converged on real speech either.
        panel.push_back(measured("Dev", -20.0, -80.0, 5));

        const LoudnessBoardModel m = loudness_board_build(
            panel, LoudnessReference::PanelMedian, 2.0, kLoudnessBoardMinBlocks);
        check(m.has_reference, "no reference produced");

        const LoudnessBoardRow *cara = nullptr;
        const LoudnessBoardRow *dev  = nullptr;
        for (const LoudnessBoardRow &row : m.rows) {
            if (row.name == "Cara") cara = &row;
            if (row.name == "Dev")  dev  = &row;
        }
        check(cara != nullptr && dev != nullptr, "expected rows missing");

        check(cara->status == LoudnessRowStatus::NoAudio,
              "Cara was not reported as NoAudio");
        check(!cara->has_short_term_deviation,
              "a NoAudio row with a stray short-term reading populated a "
              "short-term deviation");
        double bar_lu = 0.0;
        check(!loudness_board_bar_input(*cara, &bar_lu),
              "a NoAudio row produced a bar input -- a silent preshow must "
              "draw no bar, not a full-length one from the noise floor");

        check(dev->status == LoudnessRowStatus::Measuring,
              "Dev was not reported as Measuring");
        check(!dev->has_short_term_deviation,
              "a still-Measuring row populated a short-term deviation");
        bar_lu = 0.0;
        check(!loudness_board_bar_input(*dev, &bar_lu),
              "a still-Measuring row produced a bar input");
    }

    // ── The signature changes on content and NOT on input order ────────────
    // The Talkback dock shipped a live defect (2026-08-29) where a merely
    // REORDERED roster rebuilt the whole widget list several times a second
    // and threw away the operator's clicks. The board's consumer rebuilds
    // child text sources off this signature, so the same rule applies here.
    {
        std::vector<LoudnessReading> a = {
            measured("Ana", -20.0), measured("Ben", -22.0),
        };
        std::vector<LoudnessReading> b = { a[1], a[0] };   // same set, reordered
        const LoudnessBoardModel ma = loudness_board_build(
            a, LoudnessReference::PanelMedian, 2.0, kLoudnessBoardMinBlocks);
        const LoudnessBoardModel mb = loudness_board_build(
            b, LoudnessReference::PanelMedian, 2.0, kLoudnessBoardMinBlocks);
        check(ma.signature == mb.signature,
              "reordering the input changed the board signature -- the "
              "consumer will rebuild its text children on every roster event");

        std::vector<LoudnessReading> c = {
            measured("Ana", -20.0), measured("Ben", -26.0),
        };
        const LoudnessBoardModel mc = loudness_board_build(
            c, LoudnessReference::PanelMedian, 2.0, kLoudnessBoardMinBlocks);
        check(ma.signature != mc.signature,
              "a 4 LU change in one panelist did not change the signature");
    }

    // ── Layout: rows tile the canvas below the header, in order ────────────
    {
        const LoudnessBoardRect r0 = loudness_board_row_rect(640, 360, 4, 0);
        const LoudnessBoardRect r3 = loudness_board_row_rect(640, 360, 4, 3);
        check(r0.x == 0 && r0.w == 640, "a row does not span the canvas width");
        check(r0.y == kLoudnessBoardHeaderPx,
              "the first row does not start below the header band");
        check(r0.h == 79,
              "a 4-row board on a 640x360 canvas did not give 79 px rows "
              "((360-28)/4 - 4 gap)");
        check(r3.y == kLoudnessBoardHeaderPx + 83 * 3,
              "row 3 is not at the fourth slot");
        check(r3.y + r3.h <= 360,
              "the last row overflows the canvas");
        const LoudnessBoardRect bad = loudness_board_row_rect(640, 360, 4, 9);
        check(bad.w == 0 && bad.h == 0,
              "an out-of-range row index produced a drawable rect");
        const LoudnessBoardRect none = loudness_board_row_rect(640, 360, 0, 0);
        check(none.w == 0 && none.h == 0,
              "a zero-row board produced a drawable rect");
    }

    // ── Layout: the bar grows from the centre of the right half ────────────
    {
        const LoudnessBoardRect row{0, 28, 640, 79};
        const LoudnessBoardRect zero =
            loudness_board_bar_rect(row, 0.0, kLoudnessBoardFullScaleLu);
        check(zero.w == 0 && zero.x == 480,
              "a zero deviation did not collapse to nothing at the centre "
              "line (x=480 on a 640 px row)");

        const LoudnessBoardRect hot =
            loudness_board_bar_rect(row, 3.0, kLoudnessBoardFullScaleLu);
        check(hot.x == 480 && hot.w == 80,
              "+3 LU of a 6 LU full scale did not fill half the right side");

        const LoudnessBoardRect cold =
            loudness_board_bar_rect(row, -6.0, kLoudnessBoardFullScaleLu);
        check(cold.x == 320 && cold.w == 160,
              "-6 LU did not fill the left half of the meter");

        const LoudnessBoardRect clipped =
            loudness_board_bar_rect(row, 40.0, kLoudnessBoardFullScaleLu);
        check(clipped.x == 480 && clipped.w == 160 &&
              clipped.x + clipped.w <= 640,
              "an off-the-scale deviation drew past the canvas instead of "
              "clamping at full scale");
        check(cold.y == row.y && cold.h == row.h,
              "the bar's vertical extent does not match its row");
    }

    // ── The board is bounded, and it says so ───────────────────────────────
    // A 25-person Zoom Events room would give rows a few pixels tall, which
    // is not a readiness board, it is a texture. The renderer caps the rows
    // it draws; the cap has to be a decision that can be reasoned about here
    // rather than a magic number buried in a draw loop.
    {
        check(loudness_board_visible_rows(360, 3) == 3,
              "three panelists on a 360 px canvas did not all fit");
        check(loudness_board_visible_rows(360, 40) ==
              (360 - kLoudnessBoardHeaderPx) / kLoudnessBoardMinRowPx,
              "forty panelists were not capped to what the canvas can show "
              "at the minimum readable row height");
        check(loudness_board_visible_rows(360, 0) == 0,
              "an empty panel produced rows to draw");
        check(loudness_board_visible_rows(0, 10) == 0,
              "a zero-height canvas produced rows to draw");
        const size_t capped = loudness_board_visible_rows(360, 40);
        const LoudnessBoardRect last =
            loudness_board_row_rect(640, 360, capped, capped - 1);
        // Compared against the SLOT (drawn height + the inter-row gap), not
        // the drawn height alone: kLoudnessBoardMinRowPx is the minimum
        // whole-row allotment ("a name and a number... plus the gap", per
        // its own comment), and loudness_board_row_rect always carves the
        // gap back out of whatever slot it is given. Comparing the cap's
        // capacity formula (floor(body_h / kLoudnessBoardMinRowPx), pinned
        // by the equality check above) against the post-gap .h directly is
        // unsatisfiable by construction whenever kLoudnessBoardRowGapPx > 0:
        // capacity = floor(body_h/M) only guarantees body_h/capacity >= M,
        // i.e. the SLOT is at least M, not the slot minus the gap. For this
        // canvas that is 25 px of slot for a 24 px minimum, and row_rect's
        // own -4 px gap then drops the drawn height to 21.
        check(last.h + kLoudnessBoardRowGapPx >= kLoudnessBoardMinRowPx,
              "the capped row count still produced rows below the minimum "
              "readable slot height");
    }

    // -- The label refresh gate fires on shown OR signature, not signature
    // alone -----------------------------------------------------------------
    // A canvas resize changes `shown` without touching a single panelist's
    // reading -- the silent-preshow case, where every row sits stably at
    // "no audio" and the signature never moves at all. If the gate ignored
    // `shown`, newly-revealed rows would keep whatever text (usually none)
    // was applied while they were off-screen.
    {
        check(!loudness_board_needs_label_refresh("sig-a", 3, "sig-a", 3),
              "an unchanged signature and unchanged shown count asked for a "
              "refresh");
        check(loudness_board_needs_label_refresh("sig-a", 3, "sig-b", 3),
              "a changed signature with unchanged shown did not ask for a "
              "refresh");
        check(loudness_board_needs_label_refresh("sig-a", 3, "sig-a", 5),
              "a changed shown count with an UNCHANGED signature did not ask "
              "for a refresh -- this is the canvas-resize-during-a-silent-"
              "preshow case the gate exists for");
        check(loudness_board_needs_label_refresh("sig-a", 3, "sig-b", 5),
              "both changing at once did not ask for a refresh");
    }

    if (failures == 0)
        std::cout << "loudness-board: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
