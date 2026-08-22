// tests/i420-range-expand-test.cpp
// Expanding the occasional limited-range frame the Zoom SDK delivers into the
// full range the rest of the pipeline declares.
//
// The defect this guards (2026-08-22, live meeting): the engine requests
// BT709 FULL and the plugin tags every frame VIDEO_RANGE_FULL, but 16 frames
// out of 15,203 arrived limited-range anyway, across 5 of 6 participants.
// Rendered unexpanded, each was a one-frame brightness pop -- the "gamma
// flash". See src/i420-range-expand.h for the measured histograms.
#include "i420-range-expand.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// How the SDK's limited-range frame would have been produced from full-range
// content: studio swing packs 0..255 into 16..235 (luma) / 16..240 (chroma).
static uint8_t to_limited_luma(uint8_t v)
{
    return static_cast<uint8_t>(16 + (v * 219 + 127) / 255);
}

static uint8_t to_limited_chroma(uint8_t v)
{
    return static_cast<uint8_t>(16 + (v * 224 + 127) / 255);
}

int main()
{
    // --- Luma endpoints: studio swing opens to the full 0..255 ---
    check(i420_expand_luma_sample(16) == 0,
          "limited-range black (16) did not expand to 0 -- blacks would stay "
          "lifted, which is the washed-out half of the live defect");
    check(i420_expand_luma_sample(235) == 255,
          "limited-range white (235) did not expand to 255 -- whites would "
          "stay crushed");

    // --- Superblack/superwhite clamp rather than wrap ---
    // The live flash frame measured min=12, below the nominal floor: real
    // frames carry out-of-gamut samples and a wrap here would put white
    // speckle in the shadows.
    check(i420_expand_luma_sample(12) == 0,
          "a superblack sample (12, exactly what the live flash frame's floor "
          "was) did not clamp to 0");
    check(i420_expand_luma_sample(0) == 0, "luma 0 did not clamp to 0");
    check(i420_expand_luma_sample(240) == 255,
          "a superwhite sample did not clamp to 255");
    check(i420_expand_luma_sample(255) == 255, "luma 255 did not clamp to 255");

    // --- Neutral chroma MUST survive exactly ---
    // 128 is grey. If expansion moves it even one code value, every pixel of
    // every corrected frame picks up a colour cast -- which would be a far
    // more visible regression than the flash being fixed.
    check(i420_expand_chroma_sample(128) == 128,
          "neutral chroma (128) did not map to exactly 128 -- expansion is "
          "tinting the picture");
    check(i420_expand_chroma_sample(16) == 0,
          "chroma floor (16) did not expand to 0");
    check(i420_expand_chroma_sample(240) == 255,
          "chroma ceiling (240) did not expand to 255");

    // --- Monotonic: expansion may not reorder code values ---
    {
        bool ok = true;
        for (int i = 1; i < 256; ++i) {
            if (i420_expand_luma_sample(static_cast<uint8_t>(i)) <
                i420_expand_luma_sample(static_cast<uint8_t>(i - 1)))
                ok = false;
            if (i420_expand_chroma_sample(static_cast<uint8_t>(i)) <
                i420_expand_chroma_sample(static_cast<uint8_t>(i - 1)))
                ok = false;
        }
        check(ok, "expansion is not monotonic -- a brighter input sample maps "
                  "to a darker output, which would posterise or invert detail");
    }

    // --- Round trip: full -> limited (what the SDK did) -> expanded back ---
    // This is the defect modelled directly. Recovery must be within rounding
    // error, or correcting a flash frame would leave it visibly off from the
    // full-range frames on either side of it.
    {
        int worst_luma = 0, worst_chroma = 0;
        for (int v = 0; v < 256; ++v) {
            const int back_y =
                i420_expand_luma_sample(to_limited_luma(static_cast<uint8_t>(v)));
            const int back_c =
                i420_expand_chroma_sample(to_limited_chroma(static_cast<uint8_t>(v)));
            worst_luma = std::max(worst_luma, std::abs(back_y - v));
            worst_chroma = std::max(worst_chroma, std::abs(back_c - v));
        }
        check(worst_luma <= 1,
              "luma round trip lost more than one code value -- an expanded "
              "frame would not match the full-range frames around it");
        check(worst_chroma <= 1,
              "chroma round trip lost more than one code value");
    }

    // --- Whole-frame expansion: planes, sizes, and the U/V split ---
    {
        const size_t y_len = 64; // 16x4 luma, 16 samples each of U and V
        std::vector<uint8_t> y(y_len, 16);      // limited black
        std::vector<uint8_t> u(y_len / 4, 128); // neutral
        std::vector<uint8_t> v(y_len / 4, 240); // chroma ceiling
        y[0] = 235;                              // limited white

        i420_expand_limited_to_full(y.data(), u.data(), v.data(),
                                    y.data(), u.data(), v.data(), y_len);

        check(y[0] == 255 && y[1] == 0,
              "in-place whole-frame expansion did not map the luma plane "
              "(expected 235->255 and 16->0)");
        check(u[0] == 128,
              "the U plane was not treated as chroma -- neutral shifted");
        check(v[0] == 255,
              "the V plane was not expanded (chroma ceiling should reach 255)");
        check(u.size() == y_len / 4 && v.size() == y_len / 4,
              "chroma planes are not y_len/4 -- the frame layout assumption "
              "this function is built on has changed");
    }

    // --- Guards: refuse rather than half-convert ---
    // A partially expanded frame is worse than an untouched one, because it is
    // not uniformly wrong -- half the picture would pop and half would not.
    {
        std::vector<uint8_t> y(8, 16), u(2, 16), v(2, 16);
        i420_expand_limited_to_full(y.data(), u.data(), v.data(),
                                    y.data(), u.data(), v.data(), 6); // 6 % 4
        check(y[0] == 16 && u[0] == 16,
              "a y_len that is not a whole number of chroma samples was "
              "partially converted instead of refused");

        i420_expand_limited_to_full(nullptr, u.data(), v.data(),
                                    y.data(), u.data(), v.data(), 8);
        check(y[0] == 16, "a null source plane was not refused");

        i420_expand_limited_to_full(y.data(), u.data(), v.data(),
                                    y.data(), u.data(), v.data(), 0);
        check(y[0] == 16, "y_len == 0 was not refused");
    }

    // --- Regression: the live flash frame's histogram is actually repaired ---
    // Modelled on the measured frame, not on a synthetic ramp. In the
    // full-range frames either side, ~13,600 of 32,400 sampled luma values sat
    // below 16 -- a real shot with a lot of genuine shadow. In the flash frame
    // those same pixels were squeezed up against the studio-swing floor, which
    // is why its under-16 count collapsed to 3 while its floor read 12 and its
    // ceiling 234. So the fixture puts ~42% of the picture just above the
    // floor and spreads the rest, then asserts expansion puts the shadows back
    // where the neighbouring frames had them.
    {
        const size_t y_len = 1024;
        const size_t shadow = (y_len * 42) / 100;
        std::vector<uint8_t> y(y_len), u(y_len / 4, 128), v(y_len / 4, 128);
        for (size_t i = 0; i < y_len; ++i) {
            y[i] = i < shadow
                ? static_cast<uint8_t>(16 + (i % 5))          // crushed shadows
                : static_cast<uint8_t>(21 + ((i - shadow) * 213) /
                                            (y_len - shadow - 1)); // 21..234
        }
        // The handful of out-of-gamut samples that made the live frame read
        // min=12 while still counting only 3 below the floor.
        y[0] = 12; y[1] = 13; y[2] = 14;
        size_t under16_before = 0;
        for (uint8_t s : y) if (s < 16) ++under16_before;
        check(under16_before < y_len / 100,
              "the fixture does not reproduce the flash frame: its whole point "
              "is that almost nothing reads below 16 before expansion");

        i420_expand_limited_to_full(y.data(), u.data(), v.data(),
                                    y.data(), u.data(), v.data(), y_len);

        uint8_t lo = 255, hi = 0;
        size_t under16_after = 0;
        for (uint8_t s : y) {
            if (s < lo) lo = s;
            if (s > hi) hi = s;
            if (s < 16) ++under16_after;
        }
        check(lo == 0,
              "the flash frame's lifted floor was not brought back to 0 -- the "
              "blacks would still sit high against the frames either side");
        check(hi >= 253,
              "the flash frame's capped ceiling was not opened back up to ~255");
        check(under16_after > (y_len * 30) / 100,
              "the deep blacks the neighbouring full-range frames carry (~13,600 "
              "of 32,400 samples, versus 3 in the flash frame) were not "
              "restored -- the shadows would still read lifted on air");
        check(u[0] == 128 && v[0] == 128,
              "repairing the luma range introduced a colour cast");
    }

    if (failures == 0)
        std::cout << "i420-range-expand: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
