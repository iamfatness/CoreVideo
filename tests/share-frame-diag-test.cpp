// tests/share-frame-diag-test.cpp
#include "share-frame-diag.h"

#include <cstdio>
#include <vector>

static int g_failures = 0;
static void check(bool ok, const char *what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

static std::vector<uint8_t> plane(uint32_t w, uint32_t h, uint8_t fill)
{
    return std::vector<uint8_t>(static_cast<size_t>(w) * h, fill);
}

int main()
{
    // Full-range and limited-range black are both dark.
    {
        auto y = plane(64, 32, 0);
        check(share_frame_is_dark(share_luma_stats(y.data(), 64, 32, 64, 4)),
              "full-range black is dark");
        auto l = plane(64, 32, 16);
        check(share_frame_is_dark(share_luma_stats(l.data(), 64, 32, 64, 4)),
              "limited-range black is dark");
    }
    // A dark slide with one line of white text is NOT dark.
    {
        auto y = plane(64, 32, 10);
        for (uint32_t x = 0; x < 64; ++x) y[8 * 64 + x] = 235;
        const ShareLumaStats s = share_luma_stats(y.data(), 64, 32, 64, 4);
        check(s.max == 235, "text row is sampled");
        check(!share_frame_is_dark(s), "dark slide with text is not dark");
    }
    // A mid-grey desktop is not dark.
    {
        auto y = plane(64, 32, 128);
        const ShareLumaStats s = share_luma_stats(y.data(), 64, 32, 64, 4);
        check(s.mean == 128, "mean of a flat plane");
        check(!share_frame_is_dark(s), "grey is not dark");
    }
    // Stride padding is never sampled.
    {
        std::vector<uint8_t> y(80 * 16, 255);
        for (uint32_t r = 0; r < 16; ++r)
            for (uint32_t x = 0; x < 64; ++x) y[r * 80 + x] = 0;
        check(share_frame_is_dark(share_luma_stats(y.data(), 64, 16, 80, 2)),
              "bright padding beyond width is ignored");
    }
    // Empty input is never dark (no verdict without samples).
    check(!share_frame_is_dark(share_luma_stats(nullptr, 64, 32, 64, 4)),
          "null plane is not dark");
    // Logging cadence: all of the first 20, then every 100th.
    check(!share_dark_should_log(0), "count 0 never logs");
    check(share_dark_should_log(1) && share_dark_should_log(20), "first 20 log");
    check(!share_dark_should_log(21) && !share_dark_should_log(99), "21..99 quiet");
    check(share_dark_should_log(100) && share_dark_should_log(200), "every 100th logs");

    if (g_failures == 0)
        std::printf("share-frame-diag-test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
