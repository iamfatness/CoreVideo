#include "tile-clock-probe.h"

#include <iostream>
#include <vector>

static std::vector<ClockSample> series(uint32_t feed_id, int64_t offset_us,
                                       std::size_t n, int64_t jitter_us = 0)
{
    std::vector<ClockSample> out;
    for (std::size_t i = 0; i < n; ++i) {
        const uint64_t pts = 1000000ull + i * 33333ull;
        // Jitter is additive-only: real network delay never runs early.
        const int64_t extra = jitter_us ? static_cast<int64_t>((i * 7919) % jitter_us) : 0;
        ClockSample s{};
        s.feed_id = feed_id;
        s.media_pts_us = pts;
        s.arrival_ns = static_cast<uint64_t>(
            (static_cast<int64_t>(pts) + offset_us + extra) * 1000);
        out.push_back(s);
    }
    return out;
}

static bool expect_verdict(const char *name, const std::vector<ClockSample> &samples,
                           TimebaseVerdict expected)
{
    const TimebaseReport r = analyze_clock_samples(samples);
    if (r.verdict != expected) {
        std::cerr << name << ": expected " << timebase_verdict_id(expected)
                  << ", got " << timebase_verdict_id(r.verdict) << "\n";
        return false;
    }
    return true;
}

int main()
{
    // Two feeds, same clock, only network jitter between them.
    std::vector<ClockSample> shared = series(1, 20000, 60, 4000);
    for (const ClockSample &s : series(2, 23000, 60, 4000)) shared.push_back(s);
    if (!expect_verdict("shared timebase", shared, TimebaseVerdict::Shared))
        return 1;

    // Two feeds whose clocks are seconds apart -> independent sender clocks.
    std::vector<ClockSample> per_feed = series(1, 20000, 60, 4000);
    for (const ClockSample &s : series(2, 5000000, 60, 4000)) per_feed.push_back(s);
    if (!expect_verdict("per-feed timebase", per_feed, TimebaseVerdict::PerFeed))
        return 1;

    // A single feed can never answer the cross-feed question.
    if (!expect_verdict("single feed", series(1, 20000, 60), TimebaseVerdict::Insufficient))
        return 1;

    // Too few samples to trust the minimum filter.
    std::vector<ClockSample> sparse = series(1, 20000, 5);
    for (const ClockSample &s : series(2, 21000, 5)) sparse.push_back(s);
    if (!expect_verdict("sparse", sparse, TimebaseVerdict::Insufficient))
        return 1;

    // Minimum-offset estimate must survive additive jitter.
    const TimebaseReport r = analyze_clock_samples(series(1, 20000, 60, 4000));
    if (r.feeds.size() != 1) return 1;
    if (r.feeds[0].min_offset_us < 19000 || r.feeds[0].min_offset_us > 21000) {
        std::cerr << "min offset estimate drifted: " << r.feeds[0].min_offset_us << "\n";
        return 1;
    }

    std::cout << "tile-clock-probe: all tests passed\n";
    return 0;
}
