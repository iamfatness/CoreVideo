#include "iso-track-writer.h"
#include "iso-av-mux.h"
#include "iso-frame-conform.h"
#ifdef _WIN32
#include "iso-d3d-conformer.h"
#else
#include <media-io/video-scaler.h>
#endif
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <media-io/audio-resampler.h>

IsoTrackWriter::IsoTrackWriter(Config config)
    : m_config(std::move(config)), m_thread([this] { run(); })
{
}
IsoTrackWriter::~IsoTrackWriter()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_abort = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}
void IsoTrackWriter::fail(const std::string &error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_status.error.empty())
        m_status.error = error;
}
IsoTrackWriter::Status IsoTrackWriter::status() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}
void IsoTrackWriter::close(uint64_t end_ns)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closing = true;
        m_end_ns = end_ns;
    }
    m_cv.notify_all();
}
bool IsoTrackWriter::wait_finished(int timeout_ms)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [&] { return m_status.done; });
}
bool IsoTrackWriter::video(uint32_t w, uint32_t h, const uint8_t *y, const uint8_t *u,
                           const uint8_t *v, uint32_t sy, uint32_t suv, uint64_t ns)
{
    if (!y || !u || !v || w < 2 || h < 2 || w > 4096 || h > 2160 || ((w | h) & 1) || sy < w ||
        suv < w / 2)
        return false;
    Video frame{w, h, ns, std::vector<uint8_t>(size_t(w) * h * 3 / 2)};
    auto *dst = frame.pixels.data();
    for (uint32_t row = 0; row < h; ++row) {
        std::memcpy(dst, y + size_t(row) * sy, w);
        dst += w;
    }
    for (auto *plane : {u, v})
        for (uint32_t row = 0; row < h / 2; ++row) {
            std::memcpy(dst, plane + size_t(row) * suv, w / 2);
            dst += w / 2;
        }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closing || m_status.done || !m_status.error.empty())
        return false;
    // Coalesce delivery bursts; the independent output clock never loses time.
    // Keep one second through GPU/encoder startup and delivery bursts. Shrinking
    // this after the first output tick discards still-pending startup frames.
    const size_t limit = 32;
    while (m_video.size() >= limit) {
        m_video.pop_front();
        ++m_status.coalesced_video;
    }
    m_video.push_back(std::move(frame));
    m_cv.notify_all();
    return true;
}
bool IsoTrackWriter::audio(const uint8_t *pcm, uint32_t bytes, uint32_t rate, uint16_t channels,
                           uint64_t ns)
{
    if (!pcm || !bytes || (channels != 1 && channels != 2) || rate < 8000 || rate > 192000 ||
        bytes % (channels * 2) || bytes > rate * channels * 2 / 2)
        return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closing || m_status.done || !m_status.error.empty())
        return false;
    if (m_audio.size() >= 256) {
        m_status.error = "ISO audio input backlog exceeded its bound";
        return false;
    }
    m_audio.push_back({rate, channels, ns, {pcm, pcm + bytes}});
    m_cv.notify_all();
    return true;
}
void IsoTrackWriter::run()
{
    IsoFfmpegPipe pipe;
    audio_resampler_t *resampler = nullptr;
#ifndef _WIN32
    video_scaler_t *scaler = nullptr;
#endif
    try {
        std::vector<std::string> args = {
            "-hide_banner", "-loglevel", "warning", "-n",   "-f",    "matroska", "-i",
            "pipe:0",       "-map",      "0:v:0",   "-map", "0:a:0", "-c:v",     m_config.encoder};
        if (m_config.encoder == "libx264")
            args.insert(args.end(), {"-preset", "veryfast", "-crf", "18", "-threads", "2"});
        else
            args.insert(args.end(), {"-b:v", "12M", "-maxrate", "20M", "-bufsize", "24M"});
        args.insert(args.end(), {"-r",           "30",
                                 "-fps_mode",    "cfr",
                                 "-g",           "60",
                                 "-bf",          "0",
                                 "-color_range", "pc",
                                 "-colorspace",  "bt709",
                                 "-c:a",         "aac",
                                 "-b:a",         "192k",
                                 "-ar",          "48000",
                                 "-ac",          "2",
                                 "-movflags",    "+frag_keyframe+empty_moov",
                                 m_config.path});
        std::string error;
        if (!pipe.start(m_config.ffmpeg, args, m_config.log_path, IsoFfmpegPipe::kMaximumPixelBytes,
                        &error))
            throw std::runtime_error(error);
        if (!pipe.try_queue(iso_av::header()))
            throw std::runtime_error("ISO transport header rejected");
        constexpr uint64_t latency = 150000000ULL;
        constexpr size_t ringFrames = 48000 * 4;
        std::vector<int16_t> ring(ringFrames * 2, 0);
        uint64_t index = 0, audioEnd = 0;
        uint32_t rate = 0;
        uint16_t channels = 0;
        std::vector<uint8_t> picture(1920 * 1080 * 3 / 2, 128);
        std::fill_n(picture.data(), 1920 * 1080, 0);
#ifdef _WIN32
        corevideo::modules::D3DIsoFrameConformer conformer;
#else
        uint32_t oldW = 0, oldH = 0;
#endif
        for (;;) {
            Video newest{};
            std::deque<Audio> audio;
            const uint64_t tickNs = m_config.epoch_ns + index * 1000000000ULL / 30;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (m_abort || !m_status.error.empty())
                    break;
                if (m_closing && tickNs >= m_end_ns)
                    break;
                if (!m_closing && m_config.now() < tickNs + latency) {
                    m_cv.wait_for(lock, std::chrono::milliseconds(5));
                    continue;
                }
                while (!m_video.empty() && m_video.front().ns <= tickNs) {
                    newest = std::move(m_video.front());
                    m_video.pop_front();
                }
                audio.swap(m_audio);
            }
            if (!newest.pixels.empty()) {
                if (newest.w == 1920 && newest.h == 1080)
                    picture = std::move(newest.pixels);
                else {
#ifdef _WIN32
                    if (!conformer.convert(newest.pixels, newest.w, newest.h, 1920, 1080, picture,
                                           error))
                        throw std::runtime_error(error);
#else
                    const auto fit = corevideo::modules::isoFitRect(newest.w, newest.h, 1920, 1080);
                    if (oldW != newest.w || oldH != newest.h) {
                        video_scaler_destroy(scaler);
                        scaler = nullptr;
                        video_scale_info src{VIDEO_FORMAT_I420, newest.w, newest.h,
                                             VIDEO_RANGE_FULL, VIDEO_CS_709};
                        video_scale_info dst{VIDEO_FORMAT_I420, uint32_t(fit.width),
                                             uint32_t(fit.height), VIDEO_RANGE_FULL, VIDEO_CS_709};
                        if (video_scaler_create(&scaler, &dst, &src, VIDEO_SCALE_BILINEAR) !=
                            VIDEO_SCALER_SUCCESS)
                            throw std::runtime_error("ISO scaler initialization failed");
                        oldW = newest.w;
                        oldH = newest.h;
                    }
                    std::fill(picture.begin(), picture.end(), 128);
                    std::fill_n(picture.data(), 1920 * 1080, 0);
                    uint8_t *out[4] = {picture.data() + fit.y * 1920 + fit.x,
                                       picture.data() + 1920 * 1080 + fit.y / 2 * 960 + fit.x / 2,
                                       picture.data() + 1920 * 1080 * 5 / 4 + fit.y / 2 * 960 +
                                           fit.x / 2,
                                       nullptr};
                    const uint8_t *in[4] = {
                        newest.pixels.data(), newest.pixels.data() + newest.w * newest.h,
                        newest.pixels.data() + newest.w * newest.h * 5 / 4, nullptr};
                    uint32_t os[4] = {1920, 960, 960, 0},
                             is[4] = {newest.w, newest.w / 2, newest.w / 2, 0};
                    if (!video_scaler_scale(scaler, out, os, in, is))
                        throw std::runtime_error("ISO scaling failed");
#endif
                }
                std::lock_guard<std::mutex> lock(m_mutex);
                m_status.input_width = newest.w;
                m_status.input_height = newest.h;
            }
            const uint64_t emitted = index * 1600;
            for (auto &packet : audio) {
                if (packet.rate != rate || packet.channels != channels) {
                    audio_resampler_destroy(resampler);
                    resample_info src{packet.rate, AUDIO_FORMAT_16BIT,
                                      packet.channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO};
                    resample_info dst{48000, AUDIO_FORMAT_16BIT, SPEAKERS_STEREO};
                    resampler = audio_resampler_create(&dst, &src);
                    if (!resampler)
                        throw std::runtime_error("ISO audio resampler initialization failed");
                    rate = packet.rate;
                    channels = packet.channels;
                }
                uint8_t *out[MAX_AV_PLANES] = {};
                const uint8_t *in[MAX_AV_PLANES] = {packet.pcm.data()};
                uint32_t frames = 0;
                uint64_t offset = 0;
                if (!audio_resampler_resample(resampler, out, &frames, &offset, in,
                                              uint32_t(packet.pcm.size() / (channels * 2))))
                    throw std::runtime_error("ISO audio resampling failed");
                const uint64_t ns = packet.ns > offset ? packet.ns - offset : 0;
                const uint64_t target =
                    ns > m_config.epoch_ns ? (ns - m_config.epoch_ns) * 48000 / 1000000000ULL : 0;
                uint64_t begin = audioEnd;
                // Arrival timestamps bunch together when the source drains an
                // audio batch. Never rewind and overwrite already placed PCM.
                if (target > begin + 960)
                    begin = target;
                auto *samples = reinterpret_cast<const int16_t *>(out[0]);
                uint64_t late = 0;
                for (uint32_t i = 0; i < frames; ++i) {
                    const auto pos = begin + i;
                    if (pos < emitted) {
                        ++late;
                        continue;
                    }
                    if (pos >= emitted + ringFrames)
                        throw std::runtime_error("ISO audio timestamp exceeds bounded timeline");
                    ring[(pos % ringFrames) * 2] = samples[i * 2];
                    ring[(pos % ringFrames) * 2 + 1] = samples[i * 2 + 1];
                }
                audioEnd = begin + frames;
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_status.audio_packets;
                m_status.late_audio_frames += late;
            }
            iso_av::Bytes pcm(1600 * 4);
            for (size_t i = 0; i < 1600; ++i)
                for (size_t c = 0; c < 2; ++c) {
                    auto &sample = ring[((emitted + i) % ringFrames) * 2 + c];
                    const uint16_t bits = uint16_t(sample);
                    pcm[i * 4 + c * 2] = uint8_t(bits);
                    pcm[i * 4 + c * 2 + 1] = uint8_t(bits >> 8);
                    sample = 0;
                }
            auto batch = iso_av::frame(index, picture, pcm);
            const auto queueStart = std::chrono::steady_clock::now();
            while (!pipe.try_queue(std::move(batch))) {
                if (!pipe.running() || pipe.queue_status().broken)
                    throw std::runtime_error("ISO encoder stopped: " + pipe.log_tail(2048));
                std::unique_lock<std::mutex> lock(m_mutex);
                if (m_abort || !m_status.error.empty())
                    throw std::runtime_error("ISO recording interrupted");
                if (std::chrono::steady_clock::now() - queueStart > std::chrono::seconds(3))
                    throw std::runtime_error("ISO encoder backlog exceeded three seconds");
                m_cv.wait_for(lock, std::chrono::milliseconds(5));
            }
            ++index;
            const auto queue = pipe.queue_status();
            std::lock_guard<std::mutex> lock(m_mutex);
            m_status.frames = index;
            m_status.written_frames = queue.written_frames ? queue.written_frames - 1 : 0;
            m_status.startup = queue.startup;
            m_status.queued_frames = queue.frames;
            m_status.peak_frames = queue.peak_frames;
        }
    } catch (const std::exception &e) {
        fail(e.what());
    } catch (...) {
        fail("Unexpected ISO writer failure");
    }
    audio_resampler_destroy(resampler);
#ifndef _WIN32
    video_scaler_destroy(scaler);
#endif
    pipe.close_stdin();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!pipe.wait_finished(50)) {
        bool abort;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            abort = m_abort;
        }
        if (abort || std::chrono::steady_clock::now() > deadline) {
            pipe.kill();
            fail("ISO encoder finalization timed out or was cancelled");
            break;
        }
    }
    if (pipe.exit_code() != 0)
        fail("ISO encoder failure: " + pipe.log_tail(2048));
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.done = true;
        const auto queue = pipe.queue_status();
        m_status.written_frames = queue.written_frames ? queue.written_frames - 1 : 0;
        m_status.queued_frames = 0;
    }
    m_cv.notify_all();
}
