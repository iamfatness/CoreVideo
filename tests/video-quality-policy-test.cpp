#include "video-quality-policy.h"
#include <iostream>
#include <map>
#include <string>

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
    check(targets.size() == 2, "refusal must retain both targets");
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
    return failures ? 1 : 0;
}
