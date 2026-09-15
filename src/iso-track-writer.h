#pragma once
#include "iso-ffmpeg-pipe.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

// One participant, one worker, one muxed MP4. Producers copy bounded inputs
// only; GPU scaling, resampling, muxing and process I/O live on this worker.
class IsoTrackWriter
{
  public:
    struct Config {
        std::string ffmpeg, encoder, path, log_path;
        uint64_t epoch_ns = 0;
        std::function<uint64_t()> now;
    };
    struct Status {
        uint64_t frames = 0, written_frames = 0, audio_packets = 0, coalesced_video = 0, late_audio_frames = 0;
        size_t queued_frames = 0, peak_frames = 0;
        uint32_t input_width = 0, input_height = 0;
        bool done = false, startup = true;
        std::string error;
    };
    explicit IsoTrackWriter(Config config);
    ~IsoTrackWriter();
    bool video(uint32_t w, uint32_t h, const uint8_t *y, const uint8_t *u, const uint8_t *v,
               uint32_t sy, uint32_t suv, uint64_t ns);
    bool audio(const uint8_t *pcm, uint32_t bytes, uint32_t rate, uint16_t channels, uint64_t ns);
    void close(uint64_t end_ns);
    bool wait_finished(int timeout_ms);
    Status status() const;

  private:
    struct Video {
        uint32_t w, h;
        uint64_t ns;
        std::vector<uint8_t> pixels;
    };
    struct Audio {
        uint32_t rate;
        uint16_t channels;
        uint64_t ns;
        std::vector<uint8_t> pcm;
    };
    void run();
    void fail(const std::string &error);
    Config m_config;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Video> m_video;
    std::deque<Audio> m_audio;
    Status m_status;
    bool m_closing = false, m_abort = false;
    uint64_t m_end_ns = 0;
    std::thread m_thread;
};
