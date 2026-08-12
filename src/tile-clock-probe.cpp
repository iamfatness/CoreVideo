#include "tile-clock-probe.h"

#include <algorithm>
#include <map>

const char *timebase_verdict_id(TimebaseVerdict v)
{
    switch (v) {
    case TimebaseVerdict::Shared:  return "shared";
    case TimebaseVerdict::PerFeed: return "per-feed";
    default:                       return "insufficient";
    }
}

TimebaseReport analyze_clock_samples(const std::vector<ClockSample> &samples)
{
    TimebaseReport report;

    std::map<uint32_t, std::vector<int64_t>> by_feed;
    for (const ClockSample &s : samples) {
        const int64_t offset_us =
            static_cast<int64_t>(s.arrival_ns / 1000) - static_cast<int64_t>(s.media_pts_us);
        by_feed[s.feed_id].push_back(offset_us);
    }

    bool all_feeds_sufficient = true;
    for (auto &entry : by_feed) {
        std::vector<int64_t> &offsets = entry.second;
        std::sort(offsets.begin(), offsets.end());

        FeedOffsetStats stats;
        stats.feed_id          = entry.first;
        stats.sample_count     = offsets.size();
        stats.min_offset_us    = offsets.front();
        stats.median_offset_us = offsets[offsets.size() / 2];
        stats.spread_us        = offsets.back() - offsets.front();
        report.feeds.push_back(stats);

        if (offsets.size() < kMinSamplesPerFeed)
            all_feeds_sufficient = false;
    }

    if (report.feeds.size() < 2 || !all_feeds_sufficient) {
        report.verdict = TimebaseVerdict::Insufficient;
        return report;
    }

    int64_t lowest  = report.feeds.front().min_offset_us;
    int64_t highest = report.feeds.front().min_offset_us;
    for (const FeedOffsetStats &f : report.feeds) {
        lowest  = std::min(lowest, f.min_offset_us);
        highest = std::max(highest, f.min_offset_us);
    }
    report.cross_feed_spread_us = highest - lowest;
    report.verdict = report.cross_feed_spread_us <= kSharedTimebaseToleranceUs
                         ? TimebaseVerdict::Shared
                         : TimebaseVerdict::PerFeed;
    return report;
}
