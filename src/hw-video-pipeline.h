#pragma once
#include <cstdint>
#include <string>

enum class HwAccelMode : int {
    None         = 0,
    Auto         = 1,
    Cuda         = 2,
    Vaapi        = 3,
    VideoToolbox = 4,
    Qsv          = 5,
};

struct obs_source_frame;

#ifdef COREVIDEO_HW_ACCEL

struct AVBufferRef;
struct AVFilterGraph;
struct AVFilterContext;
struct AVFrame;

class HwVideoPipeline {
public:
    HwVideoPipeline() = default;
    ~HwVideoPipeline() { shutdown(); }

    HwVideoPipeline(const HwVideoPipeline &) = delete;
    HwVideoPipeline &operator=(const HwVideoPipeline &) = delete;

    // Returns false if the requested mode (or any auto-detected backend) is unavailable.
    bool init(HwAccelMode mode);

    // Converts one I420 frame to NV12 via the hardware pipeline and fills `out`.
    // Returns false on any error; caller must fall back to CPU path.
    // `out.data` pointers remain valid until the next call to process() or shutdown().
    bool process(const uint8_t *y, const uint8_t *u, const uint8_t *v,
                 int w, int h, int ls_y, int ls_u, int ls_v,
                 obs_source_frame &out);

    void shutdown();
    HwAccelMode active_mode() const { return m_active_mode; }

private:
    // `dev_type` is AVHWDeviceType cast to int to avoid FFmpeg enum in header.
    bool try_init(int dev_type, const char *scale_str);
    bool build_filter_graph(int w, int h);
    void teardown_graph();

    std::string      m_scale_str;
    AVBufferRef     *m_hw_device_ctx = nullptr;
    AVFilterGraph   *m_graph         = nullptr;
    AVFilterContext *m_buf_src        = nullptr;
    AVFilterContext *m_buf_sink       = nullptr;
    AVFrame         *m_src_frame      = nullptr;
    AVFrame         *m_sink_frame     = nullptr;
    int              m_last_w         = 0;
    int              m_last_h         = 0;
    bool             m_broken         = false;
    HwAccelMode      m_active_mode    = HwAccelMode::None;
};

#else  // COREVIDEO_HW_ACCEL not defined — compile-out stub

class HwVideoPipeline {
public:
    bool init(HwAccelMode) { return false; }
    bool process(const uint8_t *, const uint8_t *, const uint8_t *,
                 int, int, int, int, int, obs_source_frame &) { return false; }
    void shutdown() {}
    HwAccelMode active_mode() const { return HwAccelMode::None; }
};

#endif  // COREVIDEO_HW_ACCEL
