// tests/i420-scale-test.cpp
// The filtered letterbox scaler that screen-share frames go through.
//
// The defect this guards: every share frame was resampled into the source's
// fixed canvas by nearest neighbour. A share is almost never canvas-sized -- a
// MacBook shares 2560x1600, a 1440p monitor 2560x1440, a window whatever size
// it happens to be -- so a share of text was decimated by a non-integer ratio,
// dropping whole pixel columns and rows: thin strokes vanished or doubled and
// shimmered as the content scrolled. See src/i420-scale.h.
#include "i420-scale.h"

#include <algorithm>
#include <cstdint>
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

static std::vector<uint8_t> plane(uint32_t w, uint32_t h, uint8_t value)
{
    return std::vector<uint8_t>(static_cast<size_t>(w) * h, value);
}

int main()
{
    std::vector<uint8_t> scratch;

    // --- Same size is an exact copy --------------------------------------
    {
        const uint32_t w = 8, h = 4;
        std::vector<uint8_t> src(w * h);
        for (size_t i = 0; i < src.size(); ++i)
            src[i] = static_cast<uint8_t>(i * 7);
        std::vector<uint8_t> dst(w * h, 0);
        scale_plane_filtered(src.data(), w, h, w, dst.data(), w, h, w, scratch);
        check(dst == src, "a same-size scale must copy the plane unchanged");
    }

    // --- The anti-aliasing invariant -------------------------------------
    // One-pixel vertical stripes, 0/255. Halving them by nearest neighbour
    // picks every other column, so the result is a flat 0 (or a flat 255):
    // the content disappears. An area filter averages each pair to mid-grey.
    {
        const uint32_t sw = 64, sh = 8, dw = 32, dh = 8;
        std::vector<uint8_t> src(sw * sh);
        for (uint32_t y = 0; y < sh; ++y)
            for (uint32_t x = 0; x < sw; ++x)
                src[y * sw + x] = (x & 1) ? 255 : 0;
        std::vector<uint8_t> dst(dw * dh, 0);
        scale_plane_filtered(src.data(), sw, sh, sw, dst.data(), dw, dh, dw,
                             scratch);
        bool all_grey = true;
        for (uint8_t v : dst)
            all_grey = all_grey && v >= 125 && v <= 130;
        check(all_grey,
              "halving 1px stripes must average to grey, not alias to a flat 0/255");
    }

    // The same at the share ratio that motivated this (2560 -> 1728, 1.48x).
    // Nearest neighbour drops roughly every third column, so a stripe
    // pattern collapses into irregular runs of solid 0 and solid 255.
    {
        const uint32_t sw = 2560, sh = 4, dw = 1728, dh = 4;
        std::vector<uint8_t> src(sw * sh);
        for (uint32_t y = 0; y < sh; ++y)
            for (uint32_t x = 0; x < sw; ++x)
                src[y * sw + x] = (x & 1) ? 255 : 0;
        std::vector<uint8_t> dst(dw * dh, 0);
        scale_plane_filtered(src.data(), sw, sh, sw, dst.data(), dw, dh, dw,
                             scratch);
        uint32_t extremes = 0;
        for (uint8_t v : dst)
            if (v < 40 || v > 215) ++extremes;
        check(extremes == 0,
              "a 1.48x downscale of 1px stripes produced solid 0/255 pixels (aliasing)");
    }

    // --- Weights sum to one: flat stays flat at awkward ratios -----------
    {
        const uint32_t sw = 1728 + 1, sh = 1117, dw = 1662, dh = 1080;
        auto src = plane(sw, sh, 200);
        std::vector<uint8_t> dst(dw * dh, 0);
        scale_plane_filtered(src.data(), sw, sh, sw, dst.data(), dw, dh, dw,
                             scratch);
        const bool flat = std::all_of(dst.begin(), dst.end(),
                                      [](uint8_t v) { return v == 200; });
        check(flat, "a flat plane must stay exactly flat through a non-integer downscale");
    }
    {
        const uint32_t sw = 1280, sh = 720, dw = 1920, dh = 1080;
        auto src = plane(sw, sh, 17);
        std::vector<uint8_t> dst(dw * dh, 0);
        scale_plane_filtered(src.data(), sw, sh, sw, dst.data(), dw, dh, dw,
                             scratch);
        const bool flat = std::all_of(dst.begin(), dst.end(),
                                      [](uint8_t v) { return v == 17; });
        check(flat, "a flat plane must stay exactly flat through an upscale");
    }

    // --- Upscale interpolates: a ramp stays monotonic, ends preserved ----
    {
        const uint32_t sw = 16, sh = 2, dw = 40, dh = 2;
        std::vector<uint8_t> src(sw * sh);
        for (uint32_t y = 0; y < sh; ++y)
            for (uint32_t x = 0; x < sw; ++x)
                src[y * sw + x] = static_cast<uint8_t>(x * 16);
        std::vector<uint8_t> dst(dw * dh, 0);
        scale_plane_filtered(src.data(), sw, sh, sw, dst.data(), dw, dh, dw,
                             scratch);
        bool monotonic = true;
        uint32_t distinct = 1;
        for (uint32_t x = 1; x < dw; ++x) {
            monotonic = monotonic && dst[x] >= dst[x - 1];
            if (dst[x] != dst[x - 1]) ++distinct;
        }
        check(monotonic, "an upscaled ramp must stay monotonic");
        check(dst[0] == 0 && dst[dw - 1] == 240,
              "an upscale must preserve the ramp's end values");
        check(distinct > sw,
              "an upscale must interpolate, not just repeat source pixels");
    }

    // --- Strides are honoured on both sides ------------------------------
    {
        const uint32_t sw = 6, sh = 4, sstride = 10;
        const uint32_t dw = 3, dh = 2, dstride = 7;
        std::vector<uint8_t> src(sstride * sh, 99); // padding = 99
        for (uint32_t y = 0; y < sh; ++y)
            for (uint32_t x = 0; x < sw; ++x)
                src[y * sstride + x] = 50;
        std::vector<uint8_t> dst(dstride * dh, 7);
        scale_plane_filtered(src.data(), sw, sh, sstride, dst.data(), dw, dh,
                             dstride, scratch);
        bool ok = true;
        for (uint32_t y = 0; y < dh; ++y)
            for (uint32_t x = 0; x < dstride; ++x)
                ok = ok && dst[y * dstride + x] == (x < dw ? 50 : 7);
        check(ok, "source padding leaked in, or destination padding was overwritten");
    }

    // --- Letterbox geometry ----------------------------------------------
    // 2560x1600 (16:10) into a 1920x1080 canvas: fit 1728x1080, pillarboxed
    // 96 px each side, borders black (Y 0, chroma 128), content untouched.
    {
        const uint32_t sw = 2560, sh = 1600, cw = 1920, ch = 1080;
        auto y = plane(sw, sh, 180);
        auto u = plane(sw / 2, sh / 2, 90);
        auto v = plane(sw / 2, sh / 2, 160);
        std::vector<uint8_t> dst(static_cast<size_t>(cw) * ch * 3 / 2, 1);
        scale_i420_letterbox_filtered(y.data(), u.data(), v.data(), sw, sh,
                                      dst.data(), cw, ch, scratch);
        const uint8_t *dy = dst.data();
        const uint8_t *du = dy + cw * ch;
        const uint8_t *dv = du + cw * ch / 4;
        check(dy[540 * cw + 0] == 0 && dy[540 * cw + 95] == 0,
              "left pillar must be black luma");
        check(dy[540 * cw + 96] == 180 && dy[540 * cw + 1823] == 180,
              "content must span exactly x=96..1823");
        check(dy[540 * cw + 1824] == 0 && dy[540 * cw + 1919] == 0,
              "right pillar must be black luma");
        check(du[270 * (cw / 2) + 10] == 128 && dv[270 * (cw / 2) + 10] == 128,
              "pillar chroma must be neutral 128");
        check(du[270 * (cw / 2) + 480] == 90 && dv[270 * (cw / 2) + 480] == 160,
              "content chroma must be carried through");
    }

    // Degenerate input leaves a black canvas, never reads out of bounds.
    {
        const uint32_t cw = 64, ch = 36;
        std::vector<uint8_t> dst(cw * ch * 3 / 2, 1);
        scale_i420_letterbox_filtered(nullptr, nullptr, nullptr, 0, 0,
                                      dst.data(), cw, ch, scratch);
        check(dst[0] == 0 && dst[cw * ch] == 128,
              "degenerate input must produce a black canvas");
    }

    if (failures == 0) {
        std::cout << "i420-scale-test: all checks passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << "i420-scale-test: " << failures << " failure(s)\n";
    return EXIT_FAILURE;
}
