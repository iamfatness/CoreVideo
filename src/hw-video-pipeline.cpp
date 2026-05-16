#include "hw-video-pipeline.h"
#ifdef COREVIDEO_HW_ACCEL

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <media-io/video-io.h>
#include <obs-module.h>
#include <cstring>

struct BackendSpec {
    int         dev_type;    // AVHWDeviceType
    HwAccelMode mode;
    const char *scale_str;   // filter(s) between hwupload and hwdownload
};

// Ordered by preference: fastest/most common first.
static const BackendSpec kBackends[] = {
    { AV_HWDEVICE_TYPE_CUDA,         HwAccelMode::Cuda,         "scale_cuda=format=nv12"  },
    { AV_HWDEVICE_TYPE_VAAPI,        HwAccelMode::Vaapi,        "scale_vaapi=format=nv12" },
    { AV_HWDEVICE_TYPE_VIDEOTOOLBOX, HwAccelMode::VideoToolbox, "scale_vt=format=nv12"    },
    { AV_HWDEVICE_TYPE_QSV,          HwAccelMode::Qsv,          "vpp_qsv=format=nv12"     },
};

static bool attach_hw_device_to_upload_filters(AVFilterGraph *graph,
                                               AVBufferRef *device_ctx)
{
    if (!graph || !device_ctx)
        return false;

    bool attached = false;
    for (unsigned i = 0; i < graph->nb_filters; ++i) {
        AVFilterContext *ctx = graph->filters[i];
        if (!ctx || !ctx->filter || std::strcmp(ctx->filter->name, "hwupload") != 0)
            continue;

        AVBufferRef *ref = av_buffer_ref(device_ctx);
        if (!ref)
            return false;

        av_buffer_unref(&ctx->hw_device_ctx);
        ctx->hw_device_ctx = ref;
        attached = true;
    }
    return attached;
}

// ── public interface ──────────────────────────────────────────────────────────

bool HwVideoPipeline::init(HwAccelMode mode)
{
    shutdown();
    if (mode == HwAccelMode::None)
        return false;

    if (mode == HwAccelMode::Auto) {
        for (const auto &b : kBackends) {
            if (try_init(b.dev_type, b.scale_str)) {
                m_active_mode = b.mode;
                blog(LOG_INFO, "[obs-zoom-plugin] HW accel: using %s", b.scale_str);
                return true;
            }
        }
        blog(LOG_WARNING, "[obs-zoom-plugin] HW accel: no backend available, falling back to CPU");
        return false;
    }

    for (const auto &b : kBackends) {
        if (b.mode == mode) {
            if (try_init(b.dev_type, b.scale_str)) {
                m_active_mode = mode;
                blog(LOG_INFO, "[obs-zoom-plugin] HW accel: using %s", b.scale_str);
                return true;
            }
            blog(LOG_WARNING, "[obs-zoom-plugin] HW accel: requested backend unavailable");
            return false;
        }
    }
    return false;
}

bool HwVideoPipeline::process(const uint8_t *y, const uint8_t *u, const uint8_t *v,
                               int w, int h, int ls_y, int ls_u, int ls_v,
                               obs_source_frame &out)
{
    if (!m_hw_device_ctx || m_broken)
        return false;

    if (w != m_last_w || h != m_last_h || !m_graph) {
        if (!build_filter_graph(w, h)) {
            m_broken = true;
            blog(LOG_ERROR, "[obs-zoom-plugin] HW accel: filter graph build failed (%dx%d)", w, h);
            return false;
        }
    }

    // Release the previous sink frame before acquiring a new one.
    av_frame_unref(m_sink_frame);

    // Prepare the I420 source frame using properly allocated (ref-counted) buffers.
    av_frame_unref(m_src_frame);
    m_src_frame->format = AV_PIX_FMT_YUV420P;
    m_src_frame->width  = w;
    m_src_frame->height = h;
    if (av_frame_get_buffer(m_src_frame, 0) < 0)
        return false;

    const uint8_t *src_data[4]  = { y, u, v, nullptr };
    const int      src_lsz[4]   = { ls_y, ls_u, ls_v, 0 };
    av_image_copy(m_src_frame->data, m_src_frame->linesize,
                  src_data, src_lsz, AV_PIX_FMT_YUV420P, w, h);

    // Push the frame to the filter graph (KEEP_REF leaves our ref intact).
    if (av_buffersrc_write_frame(m_buf_src, m_src_frame) < 0)
        return false;

    // Pull the converted NV12 frame from the filter graph.
    if (av_buffersink_get_frame(m_buf_sink, m_sink_frame) < 0)
        return false;

    // Fill the OBS frame. Data pointers are valid until the next process() call.
    out.format      = VIDEO_FORMAT_NV12;
    out.width       = static_cast<uint32_t>(m_sink_frame->width);
    out.height      = static_cast<uint32_t>(m_sink_frame->height);
    out.data[0]     = m_sink_frame->data[0];
    out.data[1]     = m_sink_frame->data[1];
    out.linesize[0] = static_cast<uint32_t>(m_sink_frame->linesize[0]);
    out.linesize[1] = static_cast<uint32_t>(m_sink_frame->linesize[1]);
    return true;
}

void HwVideoPipeline::shutdown()
{
    teardown_graph();
    if (m_hw_device_ctx) {
        av_buffer_unref(&m_hw_device_ctx);
        m_hw_device_ctx = nullptr;
    }
    m_active_mode = HwAccelMode::None;
    m_broken      = false;
}

// ── private helpers ───────────────────────────────────────────────────────────

bool HwVideoPipeline::try_init(int dev_type, const char *scale_str)
{
    AVBufferRef *hw_ctx = nullptr;
    if (av_hwdevice_ctx_create(&hw_ctx, static_cast<AVHWDeviceType>(dev_type),
                               nullptr, nullptr, 0) < 0)
        return false;
    m_hw_device_ctx = hw_ctx;
    m_scale_str     = scale_str;
    return true;
}

bool HwVideoPipeline::build_filter_graph(int w, int h)
{
    teardown_graph();

    char buf_args[256];
    snprintf(buf_args, sizeof(buf_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/1000000000:frame_rate=60/1",
             w, h, static_cast<int>(AV_PIX_FMT_YUV420P));

    m_graph = avfilter_graph_alloc();
    if (!m_graph)
        return false;

    const AVFilter *buf_src_flt  = avfilter_get_by_name("buffer");
    const AVFilter *buf_sink_flt = avfilter_get_by_name("buffersink");

    if (avfilter_graph_create_filter(&m_buf_src, buf_src_flt, "in",
                                     buf_args, nullptr, m_graph) < 0 ||
        avfilter_graph_create_filter(&m_buf_sink, buf_sink_flt, "out",
                                     nullptr, nullptr, m_graph) < 0) {
        teardown_graph();
        return false;
    }

    // Constrain sink output to NV12 only.
    const enum AVPixelFormat sink_fmts[] = { AV_PIX_FMT_NV12, AV_PIX_FMT_NONE };
    if (av_opt_set_int_list(m_buf_sink, "pix_fmts", sink_fmts,
                            AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN) < 0) {
        teardown_graph();
        return false;
    }

    // Build chain: hwupload → <backend scale/convert> → hwdownload → format=nv12
    const std::string chain = std::string("hwupload,") + m_scale_str + ",hwdownload,format=nv12";

    // `outputs` = what the parsed chain's open input connects FROM (buffersrc output).
    // `inputs`  = what the parsed chain's open output connects TO (buffersink input).
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    if (!outputs || !inputs) {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        teardown_graph();
        return false;
    }

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = m_buf_src;
    outputs->pad_idx    = 0;
    outputs->next       = nullptr;

    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = m_buf_sink;
    inputs->pad_idx     = 0;
    inputs->next        = nullptr;

    int ret = avfilter_graph_parse_ptr(m_graph, chain.c_str(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret < 0) {
        teardown_graph();
        return false;
    }

    if (!attach_hw_device_to_upload_filters(m_graph, m_hw_device_ctx)) {
        blog(LOG_WARNING, "[obs-zoom-plugin] HW accel: hwupload filter did not accept a hardware device");
        teardown_graph();
        return false;
    }

    if (avfilter_graph_config(m_graph, nullptr) < 0) {
        teardown_graph();
        return false;
    }

    m_src_frame  = av_frame_alloc();
    m_sink_frame = av_frame_alloc();
    if (!m_src_frame || !m_sink_frame) {
        teardown_graph();
        return false;
    }

    m_last_w = w;
    m_last_h = h;
    return true;
}

void HwVideoPipeline::teardown_graph()
{
    if (m_src_frame)  { av_frame_free(&m_src_frame);  m_src_frame  = nullptr; }
    if (m_sink_frame) { av_frame_free(&m_sink_frame); m_sink_frame = nullptr; }
    // avfilter_graph_free frees all filter contexts inside the graph.
    if (m_graph)      { avfilter_graph_free(&m_graph); m_graph = nullptr; }
    m_buf_src  = nullptr;
    m_buf_sink = nullptr;
    m_last_w   = 0;
    m_last_h   = 0;
}

#endif  // COREVIDEO_HW_ACCEL
