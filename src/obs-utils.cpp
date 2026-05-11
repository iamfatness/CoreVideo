#include "obs-utils.h"
#include <util/platform.h>
#include <media-io/video-io.h>
void obs_push_video_frame(obs_source_t *source, const uint8_t *bgra_data, uint32_t width, uint32_t height)
{
    obs_source_frame frame = {};
    frame.width       = width;
    frame.height      = height;
    frame.format      = VIDEO_FORMAT_BGRA;
    frame.data[0]     = const_cast<uint8_t *>(bgra_data);
    frame.linesize[0] = width * 4;
    frame.timestamp   = os_gettime_ns();
    obs_source_output_video(source, &frame);
}
