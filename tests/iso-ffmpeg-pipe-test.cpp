// Pins the invariants of IsoFfmpegPipe that the QProcess-based feed broke
// live on 2026-08-18 (every 1080p ISO session: exactly 5 frames written,
// 0-byte MP4s, 15 s shutdown kills — QProcess's stdin chaining needs the
// owner thread's Qt event loop, and the media dispatch lane has none):
//
//  1. SUSTAINED writes: many megabytes flow to a child that consumes
//     normally — far past any initial in-flight chunk or OS pipe buffer.
//  2. BOUNDED backpressure: a stalled child makes try_queue() refuse at the
//     byte bound; memory does not grow and nothing wedges.
//  3. Clean EOF: close_stdin() lets the child see end-of-input and exit.
//  4. kill() always wins: a child that never reads cannot hang stop or
//     destruction.
//
// The test is its own child (argv[1] selects a role) so it needs no
// external binaries.
#include "iso-ffmpeg-pipe.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

static int g_failures = 0;

static void check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

static std::string self_path()
{
#ifdef _WIN32
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return buf;
#elif defined(__APPLE__)
    // No /proc on Darwin -- readlink("/proc/self/exe", ...) silently
    // returned -1 here, self_path() returned "", and start("", ...)
    // still reported success (fork() succeeded; only the child's later
    // execvp("", ...) failed). The child exited immediately, closing its
    // end of the pipe, so every write after that failed fast -- but the
    // test's own retry loop doesn't distinguish "permanently broken" from
    // "transiently full" and burned all 2000 retries x 50 buffers before
    // CTest's timeout caught it (2026-08-19 macOS CI).
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return "";
    return buf;
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return "";
    buf[n] = '\0';
    return buf;
#endif
}

static void child_binary_stdin()
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
}

static int run_child(const std::string &mode)
{
    child_binary_stdin();
    if (mode == "--pipe-child-fast") {
        unsigned long long total = 0;
        char buf[65536];
        size_t got;
        while ((got = std::fread(buf, 1, sizeof(buf), stdin)) > 0)
            total += got;
        std::fprintf(stderr, "consumed %llu\n", total);
        return 7;
    }
    if (mode == "--pipe-child-slow") {
        char buf[4096];
        while (std::fread(buf, 1, sizeof(buf), stdin) > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return 0;
    }
    if (mode == "--pipe-child-sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        return 0;
    }
    std::fprintf(stderr, "unknown child mode %s\n", mode.c_str());
    return 99;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return run_child(argv[1]);

    const std::string self = self_path();
    check(!self.empty(), "self path resolves");
    if (self.empty()) {
        // Every remaining check spawns a child at this (empty) path: fork()
        // still reports success, only the child's exec fails, and the
        // dependent retry loops below cannot tell "transiently full" from
        // "permanently broken" -- exactly the mismatch that turned one
        // resolution failure into a full CTest timeout (2026-08-19 macOS
        // CI). Fail fast and loud instead of cascading.
        std::fprintf(stderr, "FAIL: cannot resolve this test binary's own " // flawfinder: ignore
                             "path; skipping the rest (see self_path())\n");
        return 1;
    }

    // ── 1+3: sustained writes and clean EOF ─────────────────────────────
    {
        IsoFfmpegPipe pipe;
        std::string err;
        check(pipe.start(self, {"--pipe-child-fast"},
                         "pipe-test-fast.log", 64 * 1024 * 1024, &err),
              "fast child starts");
        const size_t kBuf = 200 * 1024, kCount = 50;
        size_t queued = 0;
        for (size_t i = 0; i < kCount; ++i) {
            std::vector<uint8_t> buf(kBuf, static_cast<uint8_t>(i));
            // A normally-consuming child may briefly hit the bound; retry
            // rather than drop so the byte total is exact. Bail the moment
            // the child is provably gone instead of spinning out every
            // remaining retry against a pipe that can never drain again.
            for (int tries = 0; tries < 2000; ++tries) {
                if (pipe.try_queue(std::move(buf))) {
                    ++queued;
                    break;
                }
                if (!pipe.running()) {
                    std::fprintf(stderr, // flawfinder: ignore
                                 "FAIL: child exited mid-transfer after "
                                 "queuing %zu/%zu buffers\n", queued, kCount);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                buf.assign(kBuf, static_cast<uint8_t>(i));
            }
        }
        check(queued == kCount, "all buffers queued to a consuming child");
        pipe.close_stdin();
        check(pipe.wait_finished(15000), "fast child exits after EOF");
        check(pipe.exit_code() == 7, "fast child exit code visible");
        check(!pipe.crashed(), "clean exit is not a crash");
        const std::string tail = pipe.log_tail(4096);
        const std::string expect =
            "consumed " + std::to_string(kBuf * kCount);
        check(tail.find(expect) != std::string::npos,
              "child consumed every byte (sustained writes; log captured)");
    }

    // ── 2: bounded backpressure against a stalled child ─────────────────
    {
        IsoFfmpegPipe pipe;
        std::string err;
        check(pipe.start(self, {"--pipe-child-slow"},
                         "pipe-test-slow.log", 500 * 1024, &err),
              "slow child starts");
        bool refused = false;
        for (int i = 0; i < 60 && !refused; ++i) {
            std::vector<uint8_t> buf(100 * 1024, 0xAB);
            if (!pipe.try_queue(std::move(buf)))
                refused = true;
        }
        check(refused, "try_queue refuses at the byte bound (no RAM growth)");
        check(pipe.queued_bytes() <= 500 * 1024,
              "queued bytes stay within the bound");
        pipe.kill();
        check(pipe.wait_finished(3000), "killed slow child exits");
        check(pipe.crashed(), "kill() reports as crashed");
    }

    // ── 4: a child that never reads cannot wedge stop ───────────────────
    {
        const auto t0 = std::chrono::steady_clock::now();
        {
            IsoFfmpegPipe pipe;
            std::string err;
            check(pipe.start(self, {"--pipe-child-sleep"},
                             "pipe-test-sleep.log", 64 * 1024 * 1024, &err),
                  "sleeping child starts");
            for (int i = 0; i < 3; ++i) {
                std::vector<uint8_t> buf(256 * 1024, 0xCD);
                pipe.try_queue(std::move(buf));
            }
            pipe.close_stdin();
            check(!pipe.wait_finished(300),
                  "non-reading child is still alive after EOF request");
            pipe.kill();
            check(pipe.wait_finished(3000), "kill() defeats a wedged child");
        } // destructor must not hang either
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
        check(elapsed < 10000, "whole wedged-child cycle stays fast");
    }

    if (g_failures == 0)
        std::printf("PASS: iso-ffmpeg-pipe invariants hold\n");
    return g_failures == 0 ? 0 : 1;
}
