// src/tile-motion.h
#pragma once

#include <cmath>

struct Spring1D {
    double position = 0.0;
    double velocity = 0.0;
};

inline void spring_advance(Spring1D &s, double target, double settle_seconds,
                           double dt_seconds)
{
    // No duration to travel in: this is what the operator gets by setting
    // duration to 0, and there is no meaningful "in flight" to preserve, so
    // snap straight to the target.
    if (settle_seconds <= 0.0) {
        s.position = target;
        s.velocity = 0.0;
        return;
    }

    // No time has passed: leave the spring exactly where it is. The exact
    // closed form below never divides by dt_seconds — it only appears inside
    // exp(-omega*dt) and as a multiplier, both of which are well-defined and
    // vanish cleanly at dt=0 — so this is a true no-op, not a special case
    // worked around for division safety. A duplicate timestamp, first frame,
    // or resume-from-pause must not teleport a tile mid-flight to its target.
    if (dt_seconds <= 0.0)
        return;

    // Exact solution of the critically damped spring, not a numerical
    // integrator. An approximate integrator overshoots — measurably, and by an
    // amount that does not vanish as dt shrinks — and overshoot on a tile means
    // it sails past its slot and comes back, which is precisely the cheap look
    // this feature exists to avoid. The closed form also makes the result
    // identical at any frame rate by construction rather than by tolerance.
    //
    // kSettleFactor is the critically damped 1%-remaining constant, the root of
    // (1 + k)e^-k = 0.01. It is NOT 4.6: that is the first-order constant, and
    // a second-order critically damped system decays as (1 + wt)e^-wt, which at
    // 4.6 is still 5.6% short of its target.
    constexpr double kSettleFactor = 6.6384;
    const double omega = kSettleFactor / settle_seconds;

    const double delta = s.position - target;
    const double decay = std::exp(-omega * dt_seconds);
    const double c     = s.velocity + omega * delta;

    s.position = target + (delta + c * dt_seconds) * decay;
    s.velocity = (s.velocity - omega * c * dt_seconds) * decay;
}
