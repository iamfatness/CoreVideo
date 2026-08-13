#include "tile-motion.h"

#include <cmath>
#include <iostream>

static int failures = 0;
static void check(bool ok, const char *message)
{
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; ++failures; }
}

// Advance a spring for `seconds` in fixed steps, returning the final state.
static Spring1D run(Spring1D s, double target, double settle, double seconds, double dt)
{
    for (double t = 0.0; t < seconds; t += dt)
        spring_advance(s, target, settle, dt);
    return s;
}

int main()
{
    // Settles to the target within the settle time, and stays there.
    {
        Spring1D s{0.0, 0.0};
        s = run(s, 100.0, 0.35, 0.35, 1.0 / 60.0);
        check(std::fabs(s.position - 100.0) < 2.0,
              "spring did not substantially reach its target within the settle time");
        s = run(s, 100.0, 0.35, 1.0, 1.0 / 60.0);
        check(std::fabs(s.position - 100.0) < 0.01, "spring did not come to rest on target");
        check(std::fabs(s.velocity) < 0.01, "spring still moving after settling");
    }

    // Critically damped: never overshoots.
    {
        Spring1D s{0.0, 0.0};
        double peak = 0.0;
        for (int i = 0; i < 120; ++i) {
            spring_advance(s, 100.0, 0.35, 1.0 / 60.0);
            peak = std::fmax(peak, s.position);
        }
        check(peak <= 100.0001, "spring overshot its target — not critically damped");
    }

    // Retargeting mid-flight preserves velocity: the whole point of the model.
    {
        Spring1D s{0.0, 0.0};
        s = run(s, 100.0, 0.35, 0.1, 1.0 / 60.0);
        const double v_before = s.velocity;
        check(v_before > 1.0, "test setup: spring should be moving before retarget");
        spring_advance(s, 500.0, 0.35, 1.0 / 60.0);
        check(s.velocity > v_before * 0.5,
              "velocity collapsed on retarget — motion would visibly hitch");
    }

    // Frame-rate independence: same elapsed time, same place.
    //
    // Sampled MID-FLIGHT, at 0.1s of a 0.35s settle, where the spring is
    // travelling at roughly 540 px/s and any dependence on step size has
    // somewhere to show. The earlier version of this check sampled at 0.5s —
    // 1.43x the settle time — where both springs are within 0.1px of the
    // target no matter how they got there, so it passed for any advance
    // function that eventually converges. Replacing spring_advance()'s closed
    // form with a fixed per-frame lerp that ignores dt entirely passed it,
    // and the whole rest of this file.
    //
    // Stepped by explicit COUNT rather than through run(), which accumulates
    // `t += dt` until it passes a wall-clock bound: 6 * (1/60) evaluates to
    // slightly under 0.1 in binary floating point, so that loop takes a
    // seventh step and the two rates end up comparing different elapsed
    // times. 1/30 is exactly 2 * (1/60) — a power-of-two scaling, so exact —
    // which makes 3 steps and 6 steps the same 0.1s to the last bit.
    {
        Spring1D a{0.0, 0.0}, b{0.0, 0.0};
        for (int i = 0; i < 6; ++i) spring_advance(a, 100.0, 0.35, 1.0 / 60.0);
        for (int i = 0; i < 3; ++i) spring_advance(b, 100.0, 0.35, 1.0 / 30.0);

        check(a.position > 20.0 && a.position < 90.0,
              "test setup: the sample point is not mid-flight, so this check "
              "cannot distinguish frame-rate dependence from convergence");
        check(a.velocity > 100.0,
              "test setup: the spring is barely moving at the sample point");

        // Measured divergence here is 2e-14 px; a dt-ignoring lerp diverges
        // by 22.5 px. The bound is set orders of magnitude away from both.
        check(std::fabs(a.position - b.position) < 1e-6,
              "60fps and 30fps diverged mid-flight — motion is frame-rate dependent");
        check(std::fabs(a.velocity - b.velocity) < 1e-6,
              "60fps and 30fps disagreed on velocity mid-flight — a tile "
              "retargeted at this instant would move differently at each rate");
    }

    // A zero or negative settle time snaps rather than dividing by zero.
    {
        Spring1D s{0.0, 0.0};
        spring_advance(s, 100.0, 0.0, 1.0 / 60.0);
        check(s.position == 100.0, "zero settle time did not snap to target");
        check(s.velocity == 0.0, "zero settle time left residual velocity");
    }

    // A zero or negative dt is a no-op: no time passed, so nothing about the
    // spring should change. This is distinct from settle_seconds <= 0, which
    // legitimately snaps — a duplicate timestamp, first frame, or resume
    // from pause must not teleport a tile mid-flight to its target.
    {
        Spring1D s{0.0, 0.0};
        s = run(s, 100.0, 0.35, 0.1, 1.0 / 60.0);
        const double pos_before = s.position;
        const double vel_before = s.velocity;
        check(vel_before > 1.0, "test setup: spring should be moving before the zero-dt call");

        spring_advance(s, 100.0, 0.35, 0.0);
        check(s.position == pos_before, "zero dt changed position — should be a no-op");
        check(s.velocity == vel_before, "zero dt changed velocity — should be a no-op");

        spring_advance(s, 100.0, 0.35, -1.0 / 60.0);
        check(s.position == pos_before, "negative dt changed position — should be a no-op");
        check(s.velocity == vel_before, "negative dt changed velocity — should be a no-op");
    }

    if (failures == 0) std::cout << "tile-motion tests passed\n";
    return failures == 0 ? 0 : 1;
}
