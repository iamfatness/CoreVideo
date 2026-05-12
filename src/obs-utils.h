#pragma once
#include <obs-module.h>
#include <cstdint>
void obs_push_video_frame(obs_source_t *source, const uint8_t *bgra_data, uint32_t width, uint32_t height);
