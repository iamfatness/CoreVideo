#include "zoom-sdk-init-retry.h"

#include <iostream>
#include <string>
#include <vector>

static int g_failures = 0;

static void expect(const char *name, bool ok)
{
    if (!ok) {
        std::cerr << name << ": FAILED\n";
        ++g_failures;
    }
}

static void expect_eq_u64(const char *name, uint64_t actual, uint64_t expected)
{
    if (actual != expected) {
        std::cerr << name << ": expected " << expected << ", got " << actual << "\n";
        ++g_failures;
    }
}

int main()
{
    // ---------------------------------------------------------------------
    // The code-14 special case itself. Without it every assertion in this
    // block fails, because decide_sdk_init_retry() would never ask for a retry.
    // ---------------------------------------------------------------------
    {
        const SdkInitRetryDecision d = decide_sdk_init_retry("init", 14, 0);
        expect("code 14 at init stage retries", d.retry);
        expect("first code-14 failure is not exhausted", !d.exhausted);
        expect_eq_u64("first retry waits the base delay", d.delay_ms, 2000);
    }

    // The enumerator index must stay pinned to what zoom_sdk_def.h says
    // (SDKERR_SUCCESS = 0 is the only explicit value; OTHER_SDK_INSTANCE_RUNNING
    // is the 15th enumerator). If the constant is ever "fixed" to a neighbour,
    // the whole feature silently stops firing.
    expect("SDKERR_OTHER_SDK_INSTANCE_RUNNING is 14",
           kSdkErrOtherSdkInstanceRunning == 14);

    // ---------------------------------------------------------------------
    // The schedule: 2s, 4s, 8s, then clamped at 8s.
    // ---------------------------------------------------------------------
    {
        const uint64_t expected[] = {2000, 4000, 8000, 8000, 8000};
        uint64_t previous = 0;
        for (int attempt = 0; attempt < kSdkInitRetryMaxAttempts; ++attempt) {
            const SdkInitRetryDecision d = decide_sdk_init_retry("init", 14, attempt);
            expect("every attempt under the bound retries", d.retry);
            expect_eq_u64("scheduled delay", d.delay_ms, expected[attempt]);
            expect("delay never exceeds the cap",
                   d.delay_ms <= static_cast<uint64_t>(kSdkInitRetryMaxDelayMs));
            expect("delay never shrinks", d.delay_ms >= previous);
            previous = d.delay_ms;
        }
    }

    // ---------------------------------------------------------------------
    // The bound. If the retry were unbounded, decide_sdk_init_retry() would
    // keep returning retry=true here and the loop below would never terminate
    // with exhausted=true.
    // ---------------------------------------------------------------------
    {
        const SdkInitRetryDecision at_bound =
            decide_sdk_init_retry("init", 14, kSdkInitRetryMaxAttempts);
        expect("at the bound we stop retrying", !at_bound.retry);
        expect("at the bound we report exhaustion", at_bound.exhausted);
        expect_eq_u64("exhausted decisions carry no delay", at_bound.delay_ms, 0);

        const SdkInitRetryDecision past_bound =
            decide_sdk_init_retry("init", 14, kSdkInitRetryMaxAttempts + 50);
        expect("past the bound we still stop retrying", !past_bound.retry);
        expect("past the bound we still report exhaustion", past_bound.exhausted);
    }

    // Drive the decision the way the client does — feeding back the attempt
    // count — and require that it terminates. A hard iteration ceiling well
    // above the bound turns "unbounded" into a test failure instead of a hang.
    {
        int attempts = 0;
        uint64_t waited_ms = 0;
        bool exhausted = false;
        for (int guard = 0; guard < 1000; ++guard) {
            const SdkInitRetryDecision d = decide_sdk_init_retry("init", 14, attempts);
            if (d.retry) {
                ++attempts;
                waited_ms += d.delay_ms;
                continue;
            }
            exhausted = d.exhausted;
            break;
        }
        expect("the retry loop terminates", exhausted);
        expect_eq_u64("it terminates after exactly the bounded attempt count",
                      static_cast<uint64_t>(attempts),
                      static_cast<uint64_t>(kSdkInitRetryMaxAttempts));
        expect_eq_u64("the accumulated wait matches the advertised budget",
                      waited_ms, sdk_init_retry_total_budget_ms());
        // The whole point is to outlast an orphaned engine. The one measured in
        // the field took ~19s to exit, so a budget at or under that would ship a
        // feature that still fails the operator's first click.
        expect("the budget outlasts the observed ~19s orphan", waited_ms >= 19000);
        // ...but it is a bound, not a hang: an operator must get an answer.
        expect("the budget stays inside a live-show-tolerable window",
               waited_ms <= 45000);
    }

    // ---------------------------------------------------------------------
    // Every other failure must be untouched: no retry AND no exhaustion, so
    // the caller falls through to today's auth-failure path unchanged.
    // ---------------------------------------------------------------------
    {
        const int other_codes[] = {0, 1, 3, 8, 12, 13, 15, 16, 17, 22, 63, 500};
        for (int code : other_codes) {
            for (int attempt = 0; attempt <= kSdkInitRetryMaxAttempts + 1; ++attempt) {
                const SdkInitRetryDecision d =
                    decide_sdk_init_retry("init", code, attempt);
                if (d.retry || d.exhausted) {
                    std::cerr << "code " << code << " attempt " << attempt
                              << ": expected no retry and no exhaustion, got retry="
                              << d.retry << " exhausted=" << d.exhausted << "\n";
                    ++g_failures;
                }
            }
        }

        // 13 (SDKERR_UNKNOWN) and 15 (SDKERR_INTERNAL_ERROR) bracket 14: an
        // off-by-one in the constant would light one of these up.
        expect("code 13 does not retry", !decide_sdk_init_retry("init", 13, 0).retry);
        expect("code 15 does not retry", !decide_sdk_init_retry("init", 15, 0).retry);
    }

    // Code 14 outside the init stage is not the collision we understand, so it
    // keeps today's behaviour too.
    {
        const char *other_stages[] = {"create_auth", "sdk_auth", "", "join"};
        for (const char *stage : other_stages) {
            const SdkInitRetryDecision d = decide_sdk_init_retry(stage, 14, 0);
            if (d.retry || d.exhausted) {
                std::cerr << "stage '" << stage
                          << "' with code 14: expected today's behaviour, got retry="
                          << d.retry << " exhausted=" << d.exhausted << "\n";
                ++g_failures;
            }
        }
    }

    // Defensive: a negative attempt count must not produce a negative exponent
    // or a bogus delay.
    expect_eq_u64("negative attempt count is treated as the first attempt",
                  decide_sdk_init_retry("init", 14, -3).delay_ms, 2000);

    // ---------------------------------------------------------------------
    // The operator-facing message must name the cause and an action. This is
    // the whole difference between this failure and a generic auth_fail.
    // ---------------------------------------------------------------------
    {
        const std::string msg = sdk_init_other_instance_message(30000);
        expect("message names the cause",
               msg.find("another Zoom SDK instance") != std::string::npos);
        expect("message names the SDK error",
               msg.find("SDKERR_OTHER_SDK_INSTANCE_RUNNING") != std::string::npos);
        expect("message names the likely culprit process",
               msg.find("ZoomObsEngine.exe") != std::string::npos);
        expect("message reports how long we waited",
               msg.find("30s") != std::string::npos);
        expect("message tells the operator what to do",
               msg.find("Task Manager") != std::string::npos);
    }

    if (g_failures == 0)
        std::cout << "zoom-sdk-init-retry: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
