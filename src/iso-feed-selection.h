#pragma once
#include <algorithm>
#include <string>
#include <vector>

inline bool iso_feed_selected(const std::vector<std::string> &selected,
                              const std::string &uuid, bool routed, bool excluded)
{
    return routed && !excluded && !uuid.empty() &&
           std::find(selected.begin(), selected.end(), uuid) != selected.end();
}
