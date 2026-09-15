#include "zoom-tile-frame-read.h"
#include <iostream>

int main()
{
    // A good 4x4 I420 frame is pending when a resize event arrives before draw.
    const std::vector<uint8_t> good(24, 80);
    std::vector<uint8_t> pending = good, candidate;
    const bool accepted = tile_read_frame(pending, candidate, [](auto &destination) {
        // Exactly the SHM reader contract: copy first, reject when the sequence
        // changed under the copy. The old ready flag and dimensions stay valid.
        destination.assign(6, 235);
        return false;
    });
    if (accepted || pending != good) {
        std::cerr << "Rejected resized read corrupted a pending frame\n";
        return 1;
    }
    if (!tile_read_frame(pending, candidate, [](auto &destination) {
        destination.assign(6, 120);
        return true;
    }) || pending != std::vector<uint8_t>(6, 120)) {
        std::cerr << "Valid resized frame was not published\n";
        return 1;
    }
    const auto resized = pending;
    for (int i = 0; i < 100; ++i) {
        if (tile_read_frame(pending, candidate, [i](auto &destination) {
            destination.assign(24, uint8_t(i));
            return false;
        }) || pending != resized) {
            std::cerr << "Repeated rejected read replaced the held picture\n";
            return 1;
        }
    }
    std::cout << "Tile pending pixels survive failed reads and resize recovery\n";
}
