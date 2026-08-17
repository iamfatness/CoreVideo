// tests/media-event-queue-test.cpp
// The coalescing rules of the reader-thread -> media-lane queue.
//
// The defect this exists for (2026-08-17, full-1080p pressure test): media
// callbacks ran inline on the pipe reader thread, so a video backlog delayed
// audio events by a measured 0.5-0.94s and starved the audio ring's
// edge-triggered wakeups into the writer's 2.5s keepalive (~92% audio loss).
// The queue is what makes a backlog impossible: media events are prompts, not
// payloads, so N events for one source must collapse to ONE dispatch carrying
// the latest parameters. Every rule asserted here is load-bearing for that.

#include "media-event-queue.h"

#include <iostream>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

int main()
{
    // --- First event for a uuid asks for a wakeup ---
    {
        MediaEventQueue q;
        check(q.push("a", {1, 2, 3, 4}),
              "the first pending event for a source must request a consumer "
              "wakeup");
        check(!q.empty(), "queue with one pending entry reported empty");
    }

    // --- Same-uuid events coalesce: latest parameters win, no second wakeup ---
    {
        MediaEventQueue q;
        q.push("a", {100, 1, 0, 0});
        check(!q.push("a", {200, 2, 0, 0}),
              "an event for an already-pending source must coalesce, not "
              "request a redundant wakeup -- one syscall per 10ms buffer is "
              "the exact load this queue exists to shed");
        const auto drained = q.drain();
        check(drained.size() == 1,
              "two events for one source must drain as one dispatch");
        check(drained[0].second.p1 == 200 && drained[0].second.p2 == 2,
              "a coalesced dispatch must carry the LATEST event's parameters "
              "-- the handler reads current SHM state, so stale parameters "
              "would re-check a generation the writer has moved past");
        check(q.coalesced() == 1, "coalesce counter must count absorptions");
    }

    // --- Distinct uuids never coalesce, and drain in first-pending order ---
    {
        MediaEventQueue q;
        q.push("b", {1, 0, 0, 0});
        q.push("a", {2, 0, 0, 0});
        q.push("b", {3, 0, 0, 0}); // coalesces onto b, must NOT reorder it
        const auto drained = q.drain();
        check(drained.size() == 2,
              "two sources pending must drain as two dispatches");
        check(drained[0].first == "b" && drained[1].first == "a",
              "drain must preserve first-pending order -- a source whose "
              "events keep coalescing must not push itself ahead of one that "
              "has waited longer");
        check(drained[0].second.p1 == 3,
              "coalescing must update the entry in place");
    }

    // --- Drain resets pending state; the next push is a fresh wakeup ---
    {
        MediaEventQueue q;
        q.push("a", {1, 0, 0, 0});
        (void)q.drain();
        check(q.empty(), "drained queue must be empty");
        check(q.push("a", {2, 0, 0, 0}),
              "after a drain, the same source's next event must request a "
              "wakeup again -- treating it as still-pending would silence the "
              "source until an unrelated event arrived");
    }

    // --- Coalesce counter survives drains (it is a lifetime effectiveness
    //     meter, not a per-batch one) ---
    {
        MediaEventQueue q;
        q.push("a", {1, 0, 0, 0});
        q.push("a", {2, 0, 0, 0});
        (void)q.drain();
        q.push("a", {3, 0, 0, 0});
        q.push("a", {4, 0, 0, 0});
        check(q.coalesced() == 2,
              "coalesced() must accumulate across drains");
    }

    if (failures == 0)
        std::cout << "media-event-queue: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
