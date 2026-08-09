// Unit tests for ISO encoder placement (iso-encoder-plan.h): NVENC session
// budgeting with automatic overflow to QSV/AMF/x264, and the startup-failure
// demotion chain. Motivated by the 2026-08-08 8x1080p test where 8 ISO NVENC
// sessions + OBS program output oversubscribed the GPU.
#include "iso-encoder-plan.h"

#include <iostream>
#include <string>
#include <vector>

static int g_failures = 0;

static void check(bool ok, const char *what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

static std::vector<std::string> place(const std::string &requested,
                                      int feeds,
                                      int budget_after_obs,
                                      const IsoEncoderAvailability &avail)
{
    // Mirrors the recorder: each new session consumes NVENC budget only if
    // it was actually placed on NVENC.
    std::vector<std::string> out;
    int remaining = budget_after_obs;
    for (int i = 0; i < feeds; ++i) {
        std::string enc =
            iso_choose_session_encoder(requested, remaining, avail);
        if (enc == "h264_nvenc")
            --remaining;
        out.push_back(enc);
    }
    return out;
}

static int count(const std::vector<std::string> &v, const std::string &e)
{
    int n = 0;
    for (const auto &x : v)
        if (x == e)
            ++n;
    return n;
}

int main()
{
    const IsoEncoderAvailability nv_qsv{true, true, false};
    const IsoEncoderAvailability nv_only{true, false, false};
    const IsoEncoderAvailability none{false, false, false};

    // The incident scenario: 8 feeds, program recording already holds one
    // NVENC session (budget 8 - 1 = 7). Auto places 7 on NVENC, 1 on QSV.
    {
        auto plan = place("auto", 8, 7, nv_qsv);
        check(count(plan, "h264_nvenc") == 7, "auto: 7 of 8 feeds on NVENC");
        check(count(plan, "h264_qsv") == 1, "auto: overflow feed on QSV");
    }

    // No QSV: overflow lands on x264.
    {
        auto plan = place("auto", 8, 6, nv_only);
        check(count(plan, "h264_nvenc") == 6, "auto: NVENC up to budget");
        check(count(plan, "libx264") == 2, "auto: overflow on x264 without QSV");
    }

    // Explicit NVENC choice is honored up to the budget, then overflows —
    // it must never oversubscribe.
    {
        auto plan = place("h264_nvenc", 8, 5, nv_qsv);
        check(count(plan, "h264_nvenc") == 5,
              "explicit nvenc: capped at budget");
        check(count(plan, "h264_qsv") == 3,
              "explicit nvenc: overflow goes to QSV");
    }

    // Explicit CPU choice is never second-guessed.
    check(count(place("libx264", 4, 8, nv_qsv), "libx264") == 4,
          "explicit x264: all feeds honored");

    // Explicit QSV: no session budget applies.
    check(count(place("h264_qsv", 8, 0, nv_qsv), "h264_qsv") == 8,
          "explicit qsv: unaffected by NVENC budget");

    // No hardware at all: everything on x264 even if requested.
    check(count(place("auto", 3, 5, none), "libx264") == 3,
          "auto without hardware: x264");

    // Budget exhausted before any feed: auto goes straight to QSV.
    check(place("auto", 1, 0, nv_qsv)[0] == "h264_qsv",
          "auto with zero budget: QSV first");

    // Demotion chain ends at x264 and never loops.
    check(iso_demote_encoder("h264_nvenc", nv_qsv) == "h264_qsv",
          "demote nvenc -> qsv");
    check(iso_demote_encoder("h264_qsv", nv_qsv) == "libx264",
          "demote qsv -> x264 (no AMF)");
    check(iso_demote_encoder("h264_nvenc", none) == "libx264",
          "demote nvenc -> x264 without other hardware");
    check(iso_demote_encoder("libx264", nv_qsv) == "libx264",
          "x264 never demotes further");

    if (g_failures == 0)
        std::cout << "All ISO encoder plan tests passed\n";
    return g_failures == 0 ? 0 : 1;
}
