#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// One observation of a media frame or audio buffer arriving from the Zoom SDK.
struct ClockSample {
    uint32_t feed_id      = 0;   // Zoom participant/user id
    uint64_t media_pts_us = 0;   // YUVRawDataI420/AudioRawData GetTimeStamp()
    uint64_t arrival_ns   = 0;   // os_gettime_ns() at the ingest callback
};

enum class TimebaseVerdict {
    Insufficient,  // fewer than 2 feeds, or too few samples to judge
    Shared,        // feeds appear normalized to a common clock
    PerFeed,       // feeds carry independent sender clocks
};

struct FeedOffsetStats {
    uint32_t    feed_id           = 0;
    std::size_t sample_count      = 0;
    int64_t     min_offset_us     = 0;
    int64_t     median_offset_us  = 0;
    int64_t     spread_us         = 0;  // max - min within this feed
};

struct TimebaseReport {
    TimebaseVerdict              verdict = TimebaseVerdict::Insufficient;
    std::vector<FeedOffsetStats> feeds;
    int64_t                      cross_feed_spread_us = 0;
};

// Minimum samples per feed before its min-offset estimate is trusted.
constexpr std::size_t kMinSamplesPerFeed = 30;

// Feeds whose minimum offsets agree within this bound are treated as sharing
// a timebase. Generous on purpose: it need only separate "same clock plus
// network jitter" from "unrelated clocks", which differ by seconds.
constexpr int64_t kSharedTimebaseToleranceUs = 50000;

TimebaseReport analyze_clock_samples(const std::vector<ClockSample> &samples);
const char *timebase_verdict_id(TimebaseVerdict v);
