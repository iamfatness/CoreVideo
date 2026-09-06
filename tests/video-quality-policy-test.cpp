#include "video-quality-policy.h"
#include <iostream>
#include <map>
#include <string>
#include <vector>

struct Target { uint32_t requested_resolution; };
using Targets = std::map<std::string, Target>;
int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char *why) {
        if (!ok) { std::cerr << "FAIL: " << why << '\n'; ++failures; }
    };
    Targets targets{{"tile", {0}}, {"source", {2}}};
    check(shared_video_requested_resolution(targets, 0) == 2,
          "deferred/recovery order must preserve maximum HD intent");
    check(shared_video_requested_resolution(targets, 2) == 2,
          "reverse deferred order must request HD");
    uint32_t accepted = 0;
    int calls = 0;
    shared_video_upgrade_resolution(accepted, 2, [&](uint32_t r) {
        ++calls; check(r == 2, "upgrade must request HD"); return 0;
    });
    check(accepted == 2 && calls == 1, "360 tile then HD source must upgrade in place");
    shared_video_upgrade_resolution(accepted, 0, [&](uint32_t) { ++calls; return 0; });
    check(accepted == 2 && calls == 1, "HD source then tile must preserve warm HD");
    accepted = 0;
    shared_video_upgrade_resolution(accepted, 2, [&](uint32_t) { ++calls; return 5; });
    check(accepted == 0 && calls == 2, "refused upgrade must retain working quality");

    targets.erase("source");
    check(shared_video_requested_resolution(targets, 0) == 0,
          "removed or rebound HD source must not influence recovery");
    targets["tile"].requested_resolution = 1;
    check(shared_video_requested_resolution(targets, 1) == 1,
          "updated target intent must replace previous request");
    check(!video_quality_request_accepted(5, 2), "SDK refusal must not advertise accepted HD");
    check(!video_quality_request_accepted(-1, 2), "missing SDK result must not advertise success");
    check(video_quality_request_accepted(0, 2), "successful SDK request is accepted");
    for (uint32_t i = 0; i < 3; ++i)
        check(quality_upgrade_retry_allowed(i, false), "first three automatic attempts allowed");
    check(!quality_upgrade_retry_allowed(3, false), "automatic retry must stop after three attempts");
    check(!quality_upgrade_retry_allowed(1000, false), "long meetings cannot restart exhausted budget");
    check(quality_upgrade_retry_allowed(3, true), "manual retry remains available");
    check(quality_upgrade_cooldown_ns(1) == 120'000'000'000ULL &&
          quality_upgrade_cooldown_ns(2) == 240'000'000'000ULL,
          "automatic attempts retain existing backoff");
    // A single recovery subscribe serves both retained source UUIDs. Their
    // historical accepted quality must both be replaced by the new fallback.
    struct RetainedTarget {
        uint32_t requested_resolution;
        uint32_t shm_gen;
        uint64_t frame_count;
    };
    struct Subscription {
        uint32_t participant_id = 42;
        uint32_t resolution = 2;
        int renderer = 7;
        std::map<std::string, RetainedTarget> targets{
            {"fixed", {2, 11, 120}}, {"tile", {0, 12, 240}}};
    } recovered;
    std::vector<std::string> events;
    shared_video_accept_resolution(recovered, 0,
        [&](const std::string &event) { events.push_back(event); });
    check(events.size() == 2,
          "recovery must publish fallback quality to every retained target");
    if (events.size() == 2) {
        check(events[0] == R"({"cmd":"debug","stage":"video_source_bound","source_uuid":"fixed","participant_id":42,"requested":2,"actual":0})",
              "HD target must replace historical accepted1080 with fallback360");
        check(events[1] == R"({"cmd":"debug","stage":"video_source_bound","source_uuid":"tile","participant_id":42,"requested":0,"actual":0})",
              "tile must receive fallback360 with its own requested quality");
    }
    check(recovered.resolution == 0 && recovered.renderer == 7 &&
          recovered.targets.size() == 2 &&
          recovered.targets.at("fixed").shm_gen == 11 &&
          recovered.targets.at("tile").frame_count == 240,
          "quality publication must preserve renderer and retained media state");
    events.clear();
    shared_video_accept_resolution(recovered, 2,
        [&](const std::string &event) { events.push_back(event); });
    check(recovered.resolution == 2 && events.size() == 2 &&
          events[0].find("\"actual\":2") != std::string::npos &&
          events[1].find("\"actual\":2") != std::string::npos,
          "successful HD creation must update both deferred targets too");
    return failures ? 1 : 0;
}
