#pragma once
#include <cstdint>
#include <string>

// One provider per participant stream prevents duplicate OBS sources from
// doubling audio. An alternate may take over after a 250 ms delivery gap.
inline bool iso_take_provider(std::string &owner, uint64_t &last, const std::string &source,
                              uint64_t now)
{
    if (now <= last)
        return false;
    if (!owner.empty() && owner != source && now - last < 250000000ULL)
        return false;
    owner = source;
    last = now;
    return true;
}
