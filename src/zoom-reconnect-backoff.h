#pragma once

// Pure exponential-backoff-with-jitter math, factored out of
// ZoomReconnectManager::compute_delay_locked() so it can be unit tested on
// the host without OBS/Qt/Zoom-SDK dependencies or real randomness. No
// behavior change: this is the same formula, same clamp-then-jitter-then-
// clamp order, that compute_delay_locked() used inline before this
// refactor.

#include <algorithm>
#include <cmath>

// jitter_factor is expected to already be drawn from the caller's RNG (the
// production code uses std::uniform_real_distribution<double>(0.8, 1.2));
// this function only applies it and clamps. Passing jitter_factor == 1.0
// gives the unjittered schedule, which is what most tests want to assert.
inline double compute_backoff_delay_ms(int attempt,
                                        double base_delay_ms,
                                        double backoff_multiplier,
                                        double max_delay_ms,
                                        double jitter_factor)
{
    double delay = base_delay_ms * std::pow(backoff_multiplier, attempt);
    delay = std::min(delay, max_delay_ms);
    // Apply randomized jitter in [0.8, 1.2] so simultaneous recoveries do not
    // thunder against the broker/SDK at the same instant. The cap is applied
    // before jitter; clamp again afterwards so the result never exceeds it.
    delay *= jitter_factor;
    delay = std::min(delay, max_delay_ms);
    return delay;
}
