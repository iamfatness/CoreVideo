#import "engine-talkback-pump-macos.h"
#import "engine-talkback.h"
#import <dispatch/dispatch.h>

// 50ms to match the Windows idle turn's own granularity (main.cpp's
// ipc_read_line_with_message_pump() wakes on a 50ms MsgWaitForMultipleObjects
// timeout). The interval is NOT the pacing: Law 2's ~600ms floor is enforced
// INSIDE nomination_tick(), which spends at most one membership call per
// turn -- ticking faster than the floor is harmless, ticking slower would
// silently stretch a 24-invite ladder (see nomination_tick()'s own header
// comment in engine-talkback.h). mic_tick()'s 2s re-assert interval is the
// same shape: cheap to call every turn, paced by its own deadline inside.
//
// tick() RIDES THE SAME TIMER. That is a deliberate divergence from Windows,
// not an oversight -- see task-3-report.md for the full reasoning, recorded
// here because it is the single biggest structural decision this task made
// beyond what the brief's own draft showed.
//
// On Windows, tick() -- the Milestone 1 probe's own ladder -- is driven by a
// DEDICATED std::thread that main.cpp spawns only when probe() returns true,
// specifically so CreateChannel/destroy calls can happen off the command
// loop while that loop keeps reading IPC (see probe()'s return-value
// contract in engine-talkback.h, and tick()'s own header comment on why that
// thread is "one hard-won exception" to the command-loop-thread rule).
//
// That model does not port to this file. The single constraint this whole
// port is built under is that EVERY talkback SDK call happens on the main
// queue, because the reader thread must never touch the SDK and because
// Cocoa frameworks are not thread-safe in general -- every other SDK
// interaction in main-macos.mm (join, roster, raw media subscribe/teardown,
// even the heartbeat ping) is deliberately confined to the main queue for
// exactly that reason (see that file's own header comment and the heartbeat
// thread's comment). Spawning a background std::thread that calls into
// ZoomSDKTalkbackController would be the FIRST exception to that rule in
// this engine, on a platform where nothing has ever needed one before.
// Folding tick() into this same 50ms main-queue timer keeps the rule intact
// everywhere instead of adding a second, narrower one just for talkback.
//
// This is cheap, not just safe: tick()'s own first action is
// drain_stray_channels(), a single mutex-guarded emptiness check, and none of
// its phase branches (AwaitingChannel/AwaitingInvite/Sending/Destroying)
// matches while the ladder is Idle/Done -- so an idle probe costs one
// lock/compare per turn, the same order of cost nomination_tick() already
// documents for itself.
//
// A useful side effect, not the reason for the change: because the main
// queue runs one block at a time, every talkback entry point -- the seven
// IPC command handlers, tick(), nomination_tick(), mic_tick() -- is now
// serialized on a single thread. The cross-thread Begin/Add/Execute
// exclusion reasoning tick()'s own header comment builds (the four-fact
// chain proving the probe's driving thread and the command-loop thread
// cannot destroy the same channels at once) is built for Windows' two-thread
// model; here there is only one thread to interleave on, so that reasoning
// is satisfied structurally rather than by the chain of facts Windows needs.
// It does not relax any invariant those comments protect -- it just holds
// for a simpler reason.
static dispatch_source_t g_pump = nil;

void talkback_pump_start(EngineTalkback *tb)
{
    if (g_pump) return;
    g_pump = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                    dispatch_get_main_queue());
    dispatch_source_set_timer(g_pump, dispatch_time(DISPATCH_TIME_NOW, 0),
                              50ull * NSEC_PER_MSEC, 10ull * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(g_pump, ^{
        tb->nomination_tick();
        tb->mic_tick();
        tb->tick();
    });
    dispatch_resume(g_pump);
}

void talkback_pump_stop(void)
{
    if (!g_pump) return;
    dispatch_source_cancel(g_pump);
    g_pump = nil;
}
