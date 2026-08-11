// tests/tile-crop-test.cpp
// Slot crop composed with cover-crop. The composition ORDER is what this pins:
// crop first, then cover. Reversing it cuts a different part of the frame and
// is the kind of error that looks "nearly right" on a centred talking head and
// obviously wrong on anyone sitting off to one side.

#include "zoom-tile-crop.h"

#include <cmath>
#include <iostream>

static bool near(double a, double b, double eps = 0.001)
{
    return std::fabs(a - b) < eps;
}

int main()
{
    const double aspect = 16.0 / 9.0;

    // No crop: identical to plain cover-crop of a 16:9 source, i.e. the whole
    // frame.
    CropRect c = solve_slot_crop(1920.0, 1080.0, aspect, 0.0, 0.0);
    if (!near(c.x, 0.0) || !near(c.width, 1920.0) ||
        !near(c.y, 0.0) || !near(c.height, 1080.0)) {
        std::cerr << "zero crop should be the whole 16:9 frame\n";
        return 1;
    }

    // Crop 25% off the left: the usable region starts at x=480 and is 1440
    // wide. That is 1440x1080 = 4:3, which is TALLER than 16:9, so the cover
    // step keeps full width and trims height to 1440/(16/9) = 810.
    c = solve_slot_crop(1920.0, 1080.0, aspect, 25.0, 0.0);
    if (!near(c.x, 480.0)) {
        std::cerr << "left crop must move the origin to 480, got " << c.x << "\n";
        return 1;
    }
    if (!near(c.width, 1440.0)) {
        std::cerr << "left crop width wrong: " << c.width << "\n";
        return 1;
    }
    if (!near(c.height, 810.0)) {
        std::cerr << "cover step should trim height to 810, got " << c.height << "\n";
        return 1;
    }
    if (!near(c.y, (1080.0 - 810.0) / 2.0)) {
        std::cerr << "cover step should centre vertically, got y=" << c.y << "\n";
        return 1;
    }

    // Symmetric crop stays centred horizontally.
    c = solve_slot_crop(1920.0, 1080.0, aspect, 10.0, 10.0);
    if (!near(c.x + c.width / 2.0, 960.0)) {
        std::cerr << "symmetric crop should stay centred, got centre "
                  << (c.x + c.width / 2.0) << "\n";
        return 1;
    }

    // Right crop alone moves the far edge in, not the origin.
    c = solve_slot_crop(1920.0, 1080.0, aspect, 0.0, 25.0);
    if (!near(c.x, 0.0)) {
        std::cerr << "right-only crop must not move the origin\n";
        return 1;
    }
    if (!near(c.width, 1440.0)) {
        std::cerr << "right-only crop width wrong: " << c.width << "\n";
        return 1;
    }

    // The order-discriminating case, and the reason it is here: every
    // assertion above uses a 16:9 source, where the cover step is the identity
    // and BOTH composition orders happen to give the same answer. They only
    // diverge once the source has overflow for the cover step to discard, so
    // pin the order on a source that does.
    //
    // 3840x1080 (32:9) with 25% off the left. Crop-then-cover: the usable
    // region is x=960 wide 2880, whose aspect (2.667) still exceeds 16:9, so
    // the cover step keeps FULL height and takes a centred 1920 of it —
    // (1440, 0, 1920, 1080).
    //
    // Cover-then-crop would instead cover to (960, 0, 1920, 1080) and then
    // trim, leaving a 4:3-ish remnant that has to be re-covered down to
    // 1440x810 (or 960x540 if the percentage is read against the source width).
    // Either way the height collapses below 1080, which is what this catches.
    c = solve_slot_crop(3840.0, 1080.0, aspect, 25.0, 0.0);
    if (!near(c.x, 1440.0) || !near(c.y, 0.0) ||
        !near(c.width, 1920.0) || !near(c.height, 1080.0)) {
        std::cerr << "composition order wrong: expected (1440,0,1920,1080), got ("
                  << c.x << "," << c.y << "," << c.width << "," << c.height
                  << ") — this is cover-then-crop, not crop-then-cover\n";
        return 1;
    }
    // And whatever the crop, the sampled rect is still exactly tile-shaped:
    // the cover step is what guarantees the tile is never letterboxed.
    if (!near(c.width / c.height, aspect)) {
        std::cerr << "cropped sample rect is not the tile aspect: "
                  << (c.width / c.height) << "\n";
        return 1;
    }

    // Crops that would leave nothing are clamped to a usable strip rather than
    // producing a zero-width sample rect.
    c = solve_slot_crop(1920.0, 1080.0, aspect, 60.0, 60.0);
    if (c.width <= 0.0 || c.height <= 0.0) {
        std::cerr << "over-crop must clamp to a usable strip, got "
                  << c.width << "x" << c.height << "\n";
        return 1;
    }
    if (c.x < 0.0 || c.x + c.width > 1920.0) {
        std::cerr << "clamped crop escaped the source frame\n";
        return 1;
    }

    // Negative percentages are meaningless and must not widen the source.
    c = solve_slot_crop(1920.0, 1080.0, aspect, -10.0, 0.0);
    if (c.x < 0.0 || c.x + c.width > 1920.0) {
        std::cerr << "negative crop escaped the source frame\n";
        return 1;
    }

    std::cout << "tile-crop: all tests passed\n";
    return 0;
}
