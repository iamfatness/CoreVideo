#pragma once

// Owns one FFmpeg child process and the pipe that feeds its stdin.
//
// This exists because QProcess could not do the job from the threads that
// have to call it. The ISO recorder writes frames from the media dispatch
// lane — a plain std::thread with no Qt event loop — and QProcess's Windows
// stdin machinery chains buffered writes through signals delivered on the
// OWNING THREAD's event loop. With no loop running, the first in-flight
// chunk went out and nothing ever followed: measured live 2026-08-18,
// every 1080p ISO session wrote exactly 5 frames, backlogged forever
// (bytesToWrite() pinned above the drop threshold — parent-side, so no
// encoder and no ffmpeg build was ever at fault), produced 0-byte MP4s,
// and blew the 15 s shutdown budget at stop. The 2026-08-08 "ISO stop
// stalls the engine" and RAM-balloon incidents were this same defect
// misattributed to encoder oversubscription.
//
// Design rules this class must keep:
//  - No Qt anywhere. Callers hold locks on real-time threads; every public
//    method except wait_finished() and log_tail() is non-blocking.
//  - The writer thread does plain blocking WriteFile/write on the pipe; a
//    stalled child fills the bounded queue and the caller sees try_queue()
//    == false (one dropped frame), never unbounded memory, never a wedge.
//  - The child's stdout+stderr go to a LOG FILE, not a parent pipe: nothing
//    has to drain it, the child can never block on logging, and the tail is
//    still available for diagnostics after any kind of exit.

#include <cstdint>
#include <string>
#include <vector>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

class IsoFfmpegPipe {
public:
    IsoFfmpegPipe() = default;
    ~IsoFfmpegPipe();

    IsoFfmpegPipe(const IsoFfmpegPipe &) = delete;
    IsoFfmpegPipe &operator=(const IsoFfmpegPipe &) = delete;

    // Spawns the child and starts the writer thread. Synchronous: on true
    // the process exists; on false *error says why. max_queued_bytes bounds
    // the frame queue (try_queue() refuses past it).
    bool start(const std::string &program,
               const std::vector<std::string> &args,
               const std::string &log_path,
               size_t max_queued_bytes,
               std::string *error);

    // True while the child process is running.
    bool running() const;

    // Queues one buffer for the writer thread. Returns false — WITHOUT
    // queuing — when the queue is at its byte bound or the pipe is broken;
    // the caller counts that as one dropped frame. Never blocks.
    bool try_queue(std::vector<uint8_t> &&buf);

    size_t queued_bytes() const;

    // Signals EOF: the writer drains what is queued, then closes the
    // child's stdin so it can finalize its output. Non-blocking.
    void close_stdin();

    // Waits up to timeout_ms for the child to exit. Returns true if it has.
    bool wait_finished(int timeout_ms);

    // Forcibly terminates the child. wait_finished() afterwards is cheap.
    void kill();

    long long pid() const { return m_pid; }

    // Exit code once the child has exited; -1 while running/unknown.
    int exit_code() const;

    // True when the child was killed (by us) or died on a hard fault.
    bool crashed() const;

    // Last max_bytes of the child's log file (its stdout+stderr).
    std::string log_tail(size_t max_bytes) const;

private:
    void writer_loop();
    void close_stdin_handle();

    // Platform process/pipe state, opaque here.
    void *m_process = nullptr;   // HANDLE / unused on POSIX
    void *m_stdin_write = nullptr; // HANDLE / fd via intptr on POSIX
    long long m_pid = -1;

    std::string m_log_path;
    size_t m_max_queued_bytes = 0;

    mutable std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<std::vector<uint8_t>> m_queue;
    size_t m_queued_bytes = 0;
    bool m_eof_requested = false;
    bool m_stop = false;
    bool m_broken = false;

    std::thread m_writer;
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_killed{false};
    mutable std::atomic<int> m_exit_code{-1};
};
