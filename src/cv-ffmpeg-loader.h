#pragma once

// Decides where the plugin's FFmpeg runtime (avfilter/avutil) comes from and,
// on Windows, installs the delay-load resolver that enforces the decision.
// Must run before the first HwVideoPipeline use. See cv-ffmpeg-loader.cpp.
enum class CvFfmpegRuntime {
    SystemLinked,  // non-Windows: normal dynamic linking, nothing to resolve
    ProcessCopies, // our cv*-named runtime is already resident in the process
    PrivateCopies, // loading our bundled runtime from corevideo-ffmpeg/
    Unavailable,   // no usable runtime: hardware acceleration must stay off
};

CvFfmpegRuntime cv_ffmpeg_loader_init();

// True once init() found a usable runtime. HwVideoPipeline::init() checks this
// before touching any FFmpeg symbol so a missing runtime degrades to the CPU
// path instead of raising a delay-load exception.
bool cv_ffmpeg_available();

const char *cv_ffmpeg_runtime_describe();
