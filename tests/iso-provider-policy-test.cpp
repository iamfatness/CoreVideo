#include "iso-provider-policy.h"
#include <iostream>

int main()
{
    std::string owner;
    uint64_t last = 0;
    const auto require = [](bool value) {
        if (!value)
            throw "provider policy regression";
    };
    try {
        require(iso_take_provider(owner, last, "camera-a", 1000000000));
        require(!iso_take_provider(owner, last, "camera-b", 1010000000));
        require(last == 1000000000 && owner == "camera-a");
        require(iso_take_provider(owner, last, "camera-a", 1020000000));
        require(!iso_take_provider(owner, last, "camera-a", 1020000000));
        require(!iso_take_provider(owner, last, "camera-a", 1010000000));
        require(iso_take_provider(owner, last, "camera-b", 1270000000));
        require(owner == "camera-b");
        require(!iso_take_provider(owner, last, "camera-a", 1280000000));
        owner.clear(); // Source removed: permit immediate alternate delivery.
        require(iso_take_provider(owner, last, "camera-a", 1290000000));
    } catch (const char *error) {
        std::cerr << error << '\n';
        return 1;
    }
}
