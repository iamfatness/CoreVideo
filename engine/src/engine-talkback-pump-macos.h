#pragma once
// engine-talkback-pump-macos.h -- the main-queue pacing pump (macOS talkback
// port, Task 3, 2026-09-05). See engine-talkback-pump-macos.mm for the full
// rationale. Short version: Windows rides the command loop's own 50ms idle
// turn to drive EngineTalkback::nomination_tick()/mic_tick()/tick(); macOS's
// IPC reader runs on a separate thread and has no such turn at all (see
// main-macos.mm's own header comment: the main thread must stay in the Cocoa
// run loop so the SDK's delegate callbacks can be delivered) -- a
// dispatch_source timer on the main queue is the platform equivalent, and it
// is the only place any of these three calls may happen: every talkback SDK
// call in this engine must run on the main queue, and the reader thread must
// never touch the SDK.
class EngineTalkback;

// Starts the pump. Idempotent -- a second call while one is already running
// is a no-op. `tb` must outlive every call this makes into it; main-macos.mm
// constructs its EngineTalkback with static storage duration for exactly
// that reason (mirrors engine/src/main.cpp's own `static EngineTalkback
// talkback` and its comment).
void talkback_pump_start(EngineTalkback *tb);

// Stops the pump. Idempotent.
void talkback_pump_stop();
