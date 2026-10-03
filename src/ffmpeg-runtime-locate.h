#pragma once
// OBS/Qt glue for the managed FFmpeg (spec 2026-10-03): where it lives for
// this OBS profile, and which ffmpeg the ISO recorder should run. The dock,
// the control API and OSC all start ISO through ZoomIsoRecorder::start(),
// which calls cv_ffmpeg_resolve() -- so they cannot disagree.
#include "ffmpeg-runtime-plan.h"

#include <QString>
#include <string>

QString cv_ffmpeg_install_root();          // obs_module_config_path("ffmpeg"), created
QString cv_ffmpeg_managed_exe();           // "" when this platform has no pin
bool cv_ffmpeg_managed_installed();
FfmpegResolution cv_ffmpeg_resolve(const std::string &configured);
