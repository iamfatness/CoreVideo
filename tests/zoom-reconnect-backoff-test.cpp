#include "zoom-reconnect-backoff.h"

#include <cmath>
#include <iostream>

static int g_failures = 0;

static void expect_near(const char *name, double actual, double expected, double tol = 1e-6)
{
    if (std::fabs(actual - expected) > tol) {
        std::cerr << name << ": expected " << expected << ", got " << actual << "\n";
        ++g_failures;
    }
}

int main()
{
    // Unjittered exponential growth: base=2000, multiplier=2.0, max=30000.
    expect_near("attempt 0, no jitter",
                compute_backoff_delay_ms(0, 2000, 2.0, 30000, 1.0), 2000);
    expect_near("attempt 1, no jitter",
                compute_backoff_delay_ms(1, 2000, 2.0, 30000, 1.0), 4000);
    expect_near("attempt 2, no jitter",
                compute_backoff_delay_ms(2, 2000, 2.0, 30000, 1.0), 8000);
    expect_near("attempt 3, no jitter",
                compute_backoff_delay_ms(3, 2000, 2.0, 30000, 1.0), 16000);

    // attempt 4 would be 32000 pre-clamp; clamps to max_delay_ms.
    expect_near("attempt 4 clamps to max",
                compute_backoff_delay_ms(4, 2000, 2.0, 30000, 1.0), 30000);
    expect_near("attempt 10 stays clamped",
                compute_backoff_delay_ms(10, 2000, 2.0, 30000, 1.0), 30000);

    // Jitter is applied multiplicatively after the pre-jitter clamp.
    expect_near("attempt 0 with 0.8x jitter",
                compute_backoff_delay_ms(0, 2000, 2.0, 30000, 0.8), 1600);
    expect_near("attempt 0 with 1.2x jitter",
                compute_backoff_delay_ms(0, 2000, 2.0, 30000, 1.2), 2400);

    // Jitter must not push the delay above max_delay_ms even though the
    // pre-jitter value (16000) was already under the cap: 16000 * 1.2 =
    // 19200, still under 30000, so this just checks the multiply is right...
    expect_near("attempt 3 with 1.2x jitter",
                compute_backoff_delay_ms(3, 2000, 2.0, 30000, 1.2), 19200);

    // ...whereas an attempt whose pre-jitter delay is already at the cap
    // must stay at the cap even after a >1.0 jitter factor (the second
    // clamp in compute_backoff_delay_ms is load-bearing here).
    expect_near("attempt 4 with 1.2x jitter still clamps",
                compute_backoff_delay_ms(4, 2000, 2.0, 30000, 1.2), 30000);

    // Boundary: attempt 0 with base_delay_ms already above max_delay_ms
    // clamps immediately, before jitter is even applied.
    expect_near("base delay above cap clamps pre-jitter",
                compute_backoff_delay_ms(0, 50000, 2.0, 30000, 1.0), 30000);

    // Different policy shape (smaller multiplier, matches a "gentler"
    // backoff policy) still follows the same formula.
    expect_near("multiplier 1.5, attempt 2",
                compute_backoff_delay_ms(2, 1000, 1.5, 60000, 1.0), 2250);

    return g_failures == 0 ? 0 : 1;
}
