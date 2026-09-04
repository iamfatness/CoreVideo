// src/talkback-cue.cpp
// Plays the talkback open/close audio cue on the Windows default playback
// device.
//
// WHY PlaySound(SND_MEMORY | SND_ASYNC), not WASAPI/IMMDeviceEnumerator or a
// third-party mixer: it targets the system default output with no device
// enumeration, needs no COM initialisation, and SND_ASYNC returns as soon as
// playback starts. The no-COM property specifically matters here: this file
// does its playback on a dedicated worker thread (see CueWorker below) that
// this plugin never calls CoInitialize on, and WASAPI requires COM on
// whatever thread touches it. PlaySound needs neither, and pulls in no new
// third-party dependency -- it's part of winmm, which ships with Windows.
//
// WHY THIS FILE NEVER TOUCHES libobs AUDIO: see talkback-isolation-test.cpp
// (the tap's version) and its sibling for this file, both enforced by a
// build-time source scan, not just this comment -- a later "route the cue
// through OBS so it lands on the monitor mix" change must fail the build,
// not quietly put beeps on air.
//
// WHAT HAPPENS WHEN A CUE IS REQUESTED WHILE ONE IS PLAYING: REPLACE, not
// drop or queue -- see CueWorker below for the mechanism (a single worker
// thread and a one-slot "latest wins" mailbox). REPLACE is the right choice
// regardless of mechanism: a CLOSE requested while OPEN is still sounding (a
// very quick key tap) must still be audible -- silently dropping it would
// leave the operator believing they're still keyed when they are not, which
// is the exact failure this feature exists to prevent.
//
// REVIEW-ROUND FIX -- ONE WORKER THREAD, NOT ONE THREAD PER CUE. The
// original version of this file spawned an independent detached
// std::thread per talkback_play_cue() call and relied on PlaySound's own
// "a new async call stops the previous one" behaviour for REPLACE. That has
// two real bugs, both from the SAME root cause (no ordering between
// independent threads):
//
//   1. ORDERING is not actually guaranteed. Two unsynchronized threads
//      racing PlaySoundA calls can have their OS calls land in either
//      order regardless of which talkback_play_cue() call happened first
//      in wall-clock time -- that is true of two independently-scheduled
//      threads regardless of what thread requested each one. A CLOSE
//      requested right after an OPEN (a normal "keyed, then immediately
//      released" sequence) could have its PlaySoundA call reach the OS
//      BEFORE the OPEN's, so OPEN plays last. The operator would hear
//      "you're live" after they are in fact cut -- precisely the failure
//      the close cue exists to prevent. (All current callers -- evaluate()
//      and key_off(), the latter itself called from both the timer tick
//      and zoom-control-server.cpp's request handling -- in fact run
//      serialized on the single Qt main-thread event loop today, so this
//      race does not depend on THEM being concurrent; it only needs two
//      independently-spawned std::threads racing the OS scheduler, which
//      is what talkback_play_cue() itself created on every call.)
//   2. SHUTDOWN was unbounded. A detached thread's lifetime was bounded by
//      nothing: TalkbackController::stop() -> key_off() -> a detached
//      thread starts sleeping for up to ~230ms -> stop() returns
//      immediately -> shutdown_corevideo() finishes -> obs_module_unload()
//      returns to OBS's loader, which calls FreeLibrary. If the DLL
//      unmapped while that thread was still executing code that lives in
//      it, that's a crash that takes the whole OBS process down. See
//      talkback_cue_shutdown()'s doc comment in talkback-cue.h.
//
// A single long-lived worker fixes both: cues are played in the order the
// ONE worker thread picks them up (ordering becomes structural, not a race
// between independent OS calls), and there is exactly one thread to join at
// shutdown (talkback_cue_shutdown(), called from TalkbackController::stop()
// after key_off() -- see that function's doc comment).
#include "talkback-cue.h"
#include "talkback-tone.h"

#if defined(WIN32)

// WIN32_LEAN_AND_MEAN is already defined project-wide for this target (see
// CMakeLists.txt's target_compile_definitions(obs-zoom-plugin ...)) --
// redefining it here just produces a harmless-but-noisy macro-redefinition
// warning under MSVC.
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// Matches the probe tone's sample rate (talkback-tone.h / engine-talkback);
// no reason to differ, and matching keeps the ear's reference point (the
// probe's tone quality) the same one the cue reuses.
constexpr uint32_t kSampleRate = 48000;
// Below 1.0 so talkback_tone_fill()'s int16 conversion never wraps on
// rounding -- see that function's own doc comment.
constexpr double kAmplitude = 0.5;

struct CueSpec {
    double   freq_hz;
    uint32_t duration_ms;
};

// OPEN: ~880 Hz / ~120 ms. CLOSE: ~440 Hz / ~180 ms. High-then-low is the
// broadcast convention (open = attention rising, close = stand-down) and
// stays distinguishable from program/monitor audio bleeding into a busy
// headset -- see the spec this file implements.
CueSpec spec_for(TalkbackCue cue)
{
    switch (cue) {
    case TalkbackCue::Open:
        return {880.0, 120};
    case TalkbackCue::Close:
        return {440.0, 180};
    case TalkbackCue::None:
        break;
    }
    return {0.0, 0};
}

// Minimal mono 16-bit PCM WAV header. PlaySound(SND_MEMORY) wants a real WAV
// image in memory, not a raw sample buffer -- it parses the RIFF/fmt/data
// chunks itself.
#pragma pack(push, 1)
struct WavHeader {
    char     riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t riff_size = 0;
    char     wave[4] = {'W', 'A', 'V', 'E'};
    char     fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1; // PCM
    uint16_t num_channels = 1;
    uint32_t sample_rate = kSampleRate;
    uint32_t byte_rate = kSampleRate * sizeof(int16_t);
    uint16_t block_align = sizeof(int16_t);
    uint16_t bits_per_sample = 16;
    char     data[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size = 0;
};
#pragma pack(pop)

// Builds the cue's WAV image, generated with the EXISTING
// talkback_tone_fill() the Milestone 1 probe tone uses (src/talkback-tone.h)
// -- this file must not grow a second tone generator; see that header's own
// doc comment for why a generated sine beats a shipped asset, which applies
// identically here.
std::vector<uint8_t> build_wav(TalkbackCue cue)
{
    const CueSpec spec = spec_for(cue);
    const std::size_t sample_count =
        static_cast<std::size_t>(kSampleRate) * spec.duration_ms / 1000;

    std::vector<int16_t> samples(sample_count);
    talkback_tone_fill(samples.data(), sample_count, /*start_index=*/0,
                        kSampleRate, spec.freq_hz, kAmplitude);

    const uint32_t data_bytes =
        static_cast<uint32_t>(sample_count * sizeof(int16_t));

    WavHeader header;
    header.data_size = data_bytes;
    header.riff_size = static_cast<uint32_t>(sizeof(WavHeader) - 8 + data_bytes);

    std::vector<uint8_t> wav(sizeof(WavHeader) + data_bytes);
    std::memcpy(wav.data(), &header, sizeof(WavHeader));
    if (data_bytes > 0)
        std::memcpy(wav.data() + sizeof(WavHeader), samples.data(), data_bytes);
    return wav;
}

// One long-lived worker thread and a one-slot "latest wins" mailbox. See the
// file-header comment for why this replaced one-detached-thread-per-cue.
//
// LIFECYCLE:
//   START: the SINGLETON (CueWorker::instance()) is created lazily on first
//          access, thread-safe init same as TalkbackController::instance().
//          The WORKER THREAD is a separate, later step: request() starts it
//          lazily too, under m_mtx, the first time a real cue is requested
//          -- see m_started. Constructing the singleton alone spawns no
//          thread, so a session that never plays a cue never has one to
//          join at shutdown.
//   WAKE:  the worker blocks on m_cv until either a cue is pending or
//          shutdown has been requested; request() sets the pending slot
//          (overwriting whatever was already there -- REPLACE) and notifies.
//   EXIT:  only via shutdown(), which sets m_shutting_down, notifies, then
//          waits (bounded) for the worker to drain any last-pending cue and
//          return. See shutdown()'s own comment for the bound.
class CueWorker {
public:
    static CueWorker &instance()
    {
        static CueWorker w;
        return w;
    }

    // Non-blocking: store the latest-requested cue and wake the worker.
    // Called from talkback_play_cue(), which evaluate() (Qt main thread,
    // every 25ms) and key_off() both call -- currently always on that same
    // Qt main-thread event loop (see the file-header comment) -- so this
    // function must never itself block.
    void request(TalkbackCue cue)
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        // Once shutdown has been requested, no new work is accepted -- the
        // worker is on its way out and nobody will pick this up.
        if (m_shutting_down) return;
        // Lazily start the worker on first real use, under THIS SAME
        // m_mtx -- see m_started's declaration and shutdown()'s mirrored
        // check for why that matters.
        if (!m_started) {
            m_thread = std::thread([this] { run(); });
            m_started = true;
        }
        m_pending = cue; // latest wins: overwrites any still-unplayed cue
        lock.unlock();
        m_cv.notify_one();
    }

    // Shutdown-only -- see talkback-cue.h's doc comment on
    // talkback_cue_shutdown() for the full rationale and the caller
    // contract. Signals the worker to exit, waits (BOUNDED) for it to
    // actually do so, then joins or detaches depending on which happened.
    void shutdown()
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        // REVIEW-ROUND FIX: "was a worker ever started" used to be tracked
        // by an external std::atomic<bool> (g_cue_worker_used), checked
        // BEFORE this function touched CueWorker at all, so that a session
        // that never played a cue could skip spinning one up just to join
        // it. That was a TOCTOU: it was correct only because every current
        // caller happens to run on one thread, a fact the comments nearby
        // used to overstate as already-untrue ("the control-server
        // thread"). If a future change ever did move a caller to its own
        // thread, shutdown() could observe the atomic as still-false and
        // return while a concurrent first-ever request() was lazily
        // starting a worker nobody would then join -- re-admitting the
        // DLL-unload crash this class exists to prevent, unconditionally
        // rather than only under a wedged driver.
        //
        // m_started, checked here under the SAME m_mtx request() uses to
        // set it, removes the TOCTOU entirely: "never started" and "start
        // now" are decided by the one lock, so there is no window in which
        // an outside observer can see stale state. Correctness no longer
        // depends on how many threads ever call into this file.
        if (!m_started || m_shutting_down)
            return; // never started, or shutdown() already ran
        m_shutting_down = true;
        lock.unlock();
        m_cv.notify_one();

        // BOUND: a real cue is at most ~180ms of tone plus this file's
        // ~50ms lifetime margin, so 2 seconds is generous headroom, not a
        // tight budget -- it exists only to stop a genuinely wedged
        // PlaySoundA (e.g. a hung audio driver) from hanging OBS shutdown
        // forever. If the bound IS hit, do not join -- joining would be the
        // same unbounded wait we're trying to avoid. Detach instead: this
        // re-admits the original detached-thread hazard (a DLL-unload race)
        // but ONLY in that pathological case, in exchange for bounded,
        // predictable shutdown in the overwhelmingly common one. There is
        // no third option that is both bounded and safe against a truly
        // hung system call.
        lock.lock();
        const bool exited = m_cv.wait_for(lock, std::chrono::seconds(2),
                                           [this] { return m_exited; });
        lock.unlock();

        if (exited)
            m_thread.join();
        else
            m_thread.detach();
    }

private:
    // No thread here -- it starts lazily, inside request(), the first time
    // a cue is actually requested. See m_started and request()'s comment.
    CueWorker() = default;

    // Not expected to run during normal operation -- shutdown() always
    // leaves m_thread not-joinable (joined or detached) before returning,
    // and TalkbackController::stop() always calls shutdown() before this
    // singleton could be destroyed at static-deinit time. Guard it anyway:
    // a joinable std::thread destructing calls std::terminate, and this
    // guard is the difference between "extremely unlikely" and "crash the
    // process" if some future caller forgets the shutdown() contract.
    ~CueWorker()
    {
        if (m_started && m_thread.joinable()) {
            {
                std::lock_guard<std::mutex> lock(m_mtx);
                m_shutting_down = true;
            }
            m_cv.notify_one();
            m_thread.join();
        }
    }

    void run()
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        for (;;) {
            m_cv.wait(lock, [this] {
                return m_pending != TalkbackCue::None || m_shutting_down;
            });
            // Exit only once there is truly nothing left to play -- a
            // shutdown() that arrives while a cue is still pending (e.g.
            // key_off()'s final CLOSE, requested just before
            // TalkbackController::stop() calls talkback_cue_shutdown())
            // must still get that cue played before the worker exits.
            if (m_shutting_down && m_pending == TalkbackCue::None) break;

            const TalkbackCue cue = m_pending;
            m_pending = TalkbackCue::None;
            lock.unlock();
            play(cue); // blocks THIS worker thread only, never the caller
            lock.lock();
        }
        m_exited = true;
        lock.unlock();
        m_cv.notify_one(); // wakes shutdown()'s bounded wait
    }

    // Builds the WAV and calls PlaySoundA, blocking this worker thread for
    // the cue's duration so the buffer stays alive for exactly as long as
    // playback can be reading it. This is the same buffer-lifetime
    // discipline the original per-cue-thread version used; only the thread
    // that does it changed.
    static void play(TalkbackCue cue)
    {
        const uint32_t duration_ms = spec_for(cue).duration_ms;
        const std::vector<uint8_t> wav = build_wav(cue);
        if (wav.empty()) return;

        // SND_MEMORY: `wav` IS the sound image, not a filename. SND_ASYNC:
        // returns as soon as playback starts -- REPLACE-on-overlap now
        // comes from this worker picking up the next mailbox entry in
        // order (see the class comment), not from relying on PlaySound's
        // own single-active-sound behaviour, but that behaviour is still
        // harmlessly in effect underneath. SND_NODEFAULT: if playback
        // can't start (e.g. no output device), stay silent rather than
        // fall back to Windows' own system sound, which would be a more
        // confusing signal than no cue.
        PlaySoundA(reinterpret_cast<LPCSTR>(wav.data()), nullptr,
                   SND_MEMORY | SND_ASYNC | SND_NODEFAULT);

        std::this_thread::sleep_for(
            std::chrono::milliseconds(duration_ms + 50));
    }

    std::mutex              m_mtx;
    std::condition_variable m_cv;
    TalkbackCue             m_pending = TalkbackCue::None;
    // Whether the worker thread has ever been started. Set exactly once,
    // by request() the first time a real cue is requested, under m_mtx --
    // the ONLY thing shutdown() consults to decide "nothing to join". No
    // external/unsynchronized flag exists any more; see shutdown()'s
    // comment for what that used to cost.
    bool                    m_started = false;
    bool                    m_shutting_down = false;
    bool                    m_exited = false;
    std::thread             m_thread;
};

} // namespace

void talkback_play_cue(TalkbackCue cue)
{
    if (cue == TalkbackCue::None) return;
    CueWorker::instance().request(cue);
}

void talkback_cue_shutdown()
{
    CueWorker::instance().shutdown();
}

#else // !WIN32

void talkback_play_cue(TalkbackCue)
{
    // No playback path on this platform yet. Silently doing nothing (rather
    // than failing to build) matches how the rest of this plugin treats
    // Windows-only pieces during the mac port -- see zoom-meeting.cpp's
    // `#if defined(WIN32)` guard for the same pattern.
}

void talkback_cue_shutdown()
{
    // Nothing to join -- no worker thread exists on this platform yet.
}

#endif
