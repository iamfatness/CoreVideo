#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

// Shared renderer quality is a request, not the dimensions Zoom delivers.
template<class Targets>
uint32_t shared_video_requested_resolution(const Targets &targets, uint32_t incoming)
{
    for (const auto &entry : targets)
        incoming = std::max(incoming, entry.second.requested_resolution);
    return incoming;
}

template<class SetResolution>
void shared_video_upgrade_resolution(uint32_t &accepted, uint32_t requested,
                                     SetResolution set_resolution)
{
    // Raising quality must neither unsubscribe nor replace a working renderer.
    if (requested > accepted && set_resolution(requested) == 0)
        accepted = requested;
}

inline bool video_quality_request_accepted(int code, int resolution)
{
    return code == 0 && resolution >= 0 && resolution <= 2;
}

inline constexpr uint32_t kQualityUpgradeMaxAttempts = 3;
inline uint64_t quality_upgrade_cooldown_ns(uint32_t attempts)
{
    return 60'000'000'000ULL << std::min<uint32_t>(attempts, 3);
}
inline bool quality_upgrade_retry_allowed(uint32_t attempts, bool force)
{
    return force || attempts < kQualityUpgradeMaxAttempts;
}

// Commit a newly accepted renderer request. Caller owns subscription locking.
template<class Subscription, class Publish>
void shared_video_accept_resolution(Subscription &subscription,
                                    uint32_t accepted, Publish publish)
{
    subscription.resolution = accepted;
    // Every source filters diagnostics by its own UUID. Recovery only calls
    // subscribe once per participant, so the selected source cannot stand in
    // for the other retained outputs, including after a lower fallback.
    for (const auto &target : subscription.targets) {
        publish(
            R"({"cmd":"debug","stage":"video_source_bound","source_uuid":")" +
            target.first + R"(","participant_id":)" +
            std::to_string(subscription.participant_id) + R"(,"requested":)" +
            std::to_string(target.second.requested_resolution) + R"(,"actual":)" +
            std::to_string(subscription.resolution) + "}");
    }
}
