#pragma once

#include <cstdint>
#include <vector>

// The reader may modify its destination even when its seqlock check fails.
// A pending frame must remain byte-identical until another valid frame arrives.
template<class Reader>
bool tile_read_frame(std::vector<uint8_t> &pending,
                     std::vector<uint8_t> &candidate, Reader &&read)
{
    if (!read(candidate)) return false;
    pending.swap(candidate);
    return true;
}
