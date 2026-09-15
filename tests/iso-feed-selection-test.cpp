#include "iso-feed-selection.h"
#include <iostream>

int main()
{
    const std::vector<std::string> selected{"camera-a", "camera-b"};
    if (!iso_feed_selected(selected, "camera-a", true, false) ||
        !iso_feed_selected(selected, "camera-b", true, false) ||
        iso_feed_selected(selected, "camera-c", true, false) ||
        iso_feed_selected(selected, "camera-a", false, false) ||
        iso_feed_selected(selected, "camera-a", true, true) ||
        iso_feed_selected({}, "camera-a", true, false) ||
        iso_feed_selected(selected, "", true, false)) {
        std::cerr << "ISO selection allowed an unselected, unrouted or unsupported feed\n";
        return 1;
    }
}
