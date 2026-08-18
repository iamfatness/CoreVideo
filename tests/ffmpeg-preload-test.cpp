// Pins the invariant that cv_ffmpeg_loader_init() leaves the avfilter runtime
// RESIDENT, not merely locatable. The delay-load hook fires on the first
// avfilter call, which happens in HwVideoPipeline::build_filter_graph() on the
// first video frame — under the source's m_mtx on the media dispatch path,
// where on_engine_audio() also needs m_mtx. A cold load of cvfilter-11.dll
// (which drags in the whole codec family) measured ~1.06 s on 2026-08-18,
// 13x the 8-slot audio ring's ~80 ms of slack: every source with audio flowing
// lost 106 slots (~1 s of audible dropout) at the first join of each OBS run.
// Preloading at init keeps LoadLibrary off the frame path entirely.
//
// This test compiles the real loader (blog stubbed below) next to the staged
// corevideo-ffmpeg/ runtime, so plugin_directory() resolves to this test
// executable's directory the same way it resolves to the plugin's.
#include <windows.h>
#include <cstdarg>
#include <cstdio>

#include "cv-ffmpeg-loader.h"

// libobs is not linked in test executables; the loader only uses blog().
extern "C" void blog(int log_level, const char *format, ...)
{
    (void)log_level;
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

#ifndef COREVIDEO_AVFILTER_DLL
#define COREVIDEO_AVFILTER_DLL "cvfilter-11.dll"
#endif

static int fail(const char *msg)
{
    std::fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

int main()
{
    if (GetModuleHandleA(COREVIDEO_AVFILTER_DLL))
        return fail("avfilter resident before init — test cannot prove "
                    "init() did the load");

    const CvFfmpegRuntime runtime = cv_ffmpeg_loader_init();
    if (runtime != CvFfmpegRuntime::PrivateCopies)
        return fail("expected the staged corevideo-ffmpeg/ runtime to be "
                    "found (is the runtime staged next to this test?)");

    if (!GetModuleHandleA(COREVIDEO_AVFILTER_DLL))
        return fail("avfilter not resident after init — the first video "
                    "frame would pay the LoadLibrary on the media dispatch "
                    "path under m_mtx, starving the audio ring");

    std::printf("PASS: avfilter runtime is resident after loader init\n");
    return 0;
}
