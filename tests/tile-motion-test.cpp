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
    {
        Spring1D a{0.0, 0.0}, b{0.0, 0.0};
        a = run(a, 100.0, 0.35, 0.5, 1.0 / 60.0);
        b = run(b, 100.0, 0.35, 0.5, 1.0 / 30.0);
        check(std::fabs(a.position - b.position) < 1.0,
              "60fps and 30fps diverged — motion is frame-rate dependent");
    }

    // A zero or negative settle time snaps rather than dividing by zero.
    {
        Spring1D s{0.0, 0.0};
        spring_advance(s, 100.0, 0.0, 1.0 / 60.0);
        check(s.position == 100.0, "zero settle time did not snap to target");
        check(s.velocity == 0.0, "zero settle time left residual velocity");
    }

    if (failures == 0) std::cout << "tile-motion tests passed\n";
    return failures == 0 ? 0 : 1;
}
