#!/usr/bin/env python3
"""Parse TILECLOCK lines from an OBS log and report the timebase verdict.

Usage: python tools/tile-clock-analyze.py <path-to-obs-log>
Mirrors analyze_clock_samples() in src/tile-clock-probe.cpp.
"""
import collections
import re
import sys

MIN_SAMPLES_PER_FEED = 30
SHARED_TOLERANCE_US = 50000
LINE = re.compile(r"TILECLOCK,(\w),(\d+),(\d+),(\d+)")


def main(path):
    by_feed = collections.defaultdict(list)
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            match = LINE.search(line)
            if not match:
                continue
            kind, feed_id, media_pts_us, arrival_ns = match.groups()
            if kind != "v":  # video only; audio ids may be 0 for mixed callbacks
                continue
            offset_us = int(arrival_ns) // 1000 - int(media_pts_us)
            by_feed[int(feed_id)].append(offset_us)

    if len(by_feed) < 2:
        print(f"verdict: insufficient ({len(by_feed)} feed(s) seen; need 2+)")
        return 1

    minimums = {}
    for feed_id, offsets in sorted(by_feed.items()):
        offsets.sort()
        minimums[feed_id] = offsets[0]
        print(f"feed {feed_id}: n={len(offsets)} min={offsets[0]}us "
              f"median={offsets[len(offsets) // 2]}us "
              f"spread={offsets[-1] - offsets[0]}us")

    if any(len(o) < MIN_SAMPLES_PER_FEED for o in by_feed.values()):
        print("verdict: insufficient (a feed has fewer than "
              f"{MIN_SAMPLES_PER_FEED} samples)")
        return 1

    cross = max(minimums.values()) - min(minimums.values())
    verdict = "shared" if cross <= SHARED_TOLERANCE_US else "per-feed"
    print(f"cross-feed spread: {cross}us")
    print(f"verdict: {verdict}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
