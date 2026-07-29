#include "zoom-reconnect-schedule.h"

#include <iostream>

static int g_failures = 0;

static void expect(const char *name, bool cond)
{
    if (!cond) {
        std::cerr << name << ": failed\n";
        ++g_failures;
    }
}

int main()
{
    // Initial state: nothing pending, generation 0.
    {
        ZoomReconnectSchedule s;
        expect("initial not pending", !s.pending());
        expect("initial generation is 0", s.generation() == 0);
        expect("initial generation 0 is current", s.is_current(0));
        expect("generation 1 is not yet current", !s.is_current(1));
    }

    // schedule() sets pending and bumps generation.
    {
        ZoomReconnectSchedule s;
        const uint64_t gen = s.schedule();
        expect("schedule sets pending", s.pending());
        expect("schedule bumps generation to 1", s.generation() == 1);
        expect("schedule returns the new generation", gen == 1);
        expect("new generation is current", s.is_current(gen));
        expect("stale generation 0 is not current", !s.is_current(0));
    }

    // consume_pending() clears pending but does NOT bump generation --
    // this is the hand-off from the timer thread to execute_retry(): the
    // fire-generation captured right after consume_pending() must still
    // read as current.
    {
        ZoomReconnectSchedule s;
        const uint64_t gen = s.schedule();
        s.consume_pending();
        expect("consume_pending clears pending", !s.pending());
        expect("consume_pending does not bump generation", s.generation() == gen);
        expect("fire generation still current after consume", s.is_current(gen));
    }

    // reset() (cancel / give-up / success) clears pending state.
    {
        ZoomReconnectSchedule s;
        s.schedule();
        const uint64_t reset_gen = s.reset();
        expect("reset clears pending", !s.pending());
        expect("reset bumps generation again", reset_gen == 2);
        expect("reset generation is current", s.is_current(reset_gen));
    }

    // Generation-guard: a fire-generation captured before a cancel/reset
    // must be recognized as stale afterwards (this is what makes
    // execute_retry() a no-op after cancel() races with the timer firing).
    {
        ZoomReconnectSchedule s;
        const uint64_t fire_gen = s.schedule();
        s.consume_pending(); // timer thread hands off to execute_retry
        expect("fire generation current before cancel", s.is_current(fire_gen));

        s.reset(); // cancel() races in before execute_retry() runs
        expect("fire generation stale after cancel", !s.is_current(fire_gen));
        expect("cancel leaves nothing pending", !s.pending());
    }

    // Generation-guard: rescheduling (a fresh trigger()/on_join_failed()
    // call) also invalidates any earlier fire-generation, so a slow wake-up
    // for the old schedule cannot fire a retry that was superseded by a new
    // one.
    {
        ZoomReconnectSchedule s;
        const uint64_t first_gen = s.schedule();
        const uint64_t second_gen = s.schedule(); // reschedule before first fires
        expect("reschedule bumps generation again", second_gen == first_gen + 1);
        expect("old fire generation is stale", !s.is_current(first_gen));
        expect("new generation is current", s.is_current(second_gen));
        expect("still pending after reschedule", s.pending());
    }

    // Multiple cancel-then-reschedule cycles keep generations monotonically
    // increasing and never let a stale generation number come back around.
    {
        ZoomReconnectSchedule s;
        uint64_t last_gen = 0;
        for (int i = 0; i < 5; ++i) {
            const uint64_t gen = s.schedule();
            expect("generation strictly increases", gen > last_gen);
            last_gen = gen;
            s.reset();
            expect("reset always clears pending", !s.pending());
        }
    }

    return g_failures == 0 ? 0 : 1;
}
