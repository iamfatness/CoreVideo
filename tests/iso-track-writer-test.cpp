// Real worker integration test. Run in an empty folder with FFmpeg on argv[1].
#include "iso-track-writer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <thread>
#include <util/platform.h>

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    const int tracks = argc > 3 ? std::stoi(argv[3]) : 2;
    if (tracks < 1 || tracks > 8)
        return 2;
    const auto epoch = os_gettime_ns();
    IsoTrackWriter::Config silentConfig;
    silentConfig.ffmpeg = argv[1];
    silentConfig.encoder = "libx264";
    silentConfig.path = "silent.mp4";
    silentConfig.log_path = "silent.mp4.log";
    silentConfig.epoch_ns = epoch;
    silentConfig.now = [] { return os_gettime_ns(); };
    IsoTrackWriter silent(silentConfig);
    auto brokenConfig = silentConfig;
    brokenConfig.ffmpeg = "nonexistent-corevideo-test-encoder";
    brokenConfig.path = "broken.mp4";
    brokenConfig.log_path = "broken.mp4.log";
    IsoTrackWriter broken(brokenConfig);
    std::vector<std::unique_ptr<IsoTrackWriter>> writers;
    for (int i = 0; i < tracks; ++i) {
        IsoTrackWriter::Config c;
        c.ffmpeg = argv[1];
        c.encoder = argc > 2 ? argv[2] : "libx264";
        c.path = "participant-" + std::to_string(i) + ".mp4";
        c.log_path = c.path + ".log";
        c.epoch_ns = epoch;
        c.now = [] { return os_gettime_ns(); };
        writers.push_back(std::make_unique<IsoTrackWriter>(c));
    }
    const auto begin = std::chrono::steady_clock::now();
    for (int packet = 0; packet < 600; ++packet) {
        std::this_thread::sleep_until(begin + std::chrono::milliseconds(packet * 10));
        const uint64_t ns = epoch + uint64_t(packet) * 10000000;
        const int second = packet / 100;
        uint32_t w = second == 1 ? 1280 : second == 2 ? 1920 : 640;
        uint32_t h = second == 1 ? 720 : second == 2 ? 1080 : second == 3 ? 480 : 360;
        const bool flash = packet % 100 >= 20 && packet % 100 < 30;
        std::vector<uint8_t> pixels(size_t(w) * h * 3 / 2, 128);
        std::fill_n(pixels.data(), size_t(w) * h, flash ? 235 : 32);
        std::fill_n(pixels.data() + size_t(w) * h, size_t(w) * h / 4, 90);
        std::fill_n(pixels.data() + size_t(w) * h * 5 / 4, size_t(w) * h / 4, 180);
        const uint32_t rate = second < 3 ? 32000 : 48000;
        const uint16_t channels = second < 3 ? 1 : 2;
        std::vector<int16_t> audio(size_t(rate / 100) * channels);
        for (uint32_t frame = 0; frame < rate / 100; ++frame) {
            const auto sample = int16_t(
                flash ? std::sin(double(frame) * 2 * 3.141592653589793 * 1000 / rate) * 16000 : 0);
            for (uint16_t c = 0; c < channels; ++c)
                audio[frame * channels + c] = sample;
        }
        for (auto &writer : writers) {
            // Send variable-rate video; withhold a whole second for camera-off.
            if (packet % 3 == 0 && second != 4)
                writer->video(w, h, pixels.data(), pixels.data() + w * h,
                              pixels.data() + w * h * 5 / 4, w, w / 2, ns);
            // Audio callback silence is withheld too; output must still contain silence.
            if (flash)
                writer->audio(reinterpret_cast<const uint8_t *>(audio.data()),
                              uint32_t(audio.size() * 2), rate, channels,
                              epoch + uint64_t(packet / 10) * 100000000ULL +
                                  uint64_t(packet % 10) * 1000ULL);
        }
    }
    for (auto &writer : writers)
        writer->close(epoch + 6000000000ULL);
    silent.close(epoch + 6000000000ULL);
    int failures = 0;
    for (auto &writer : writers) {
        if (!writer->wait_finished(15000))
            ++failures;
        const auto s = writer->status();
        std::cout << "frames=" << s.frames << " peak=" << s.peak_frames
                  << " late_audio=" << s.late_audio_frames << " error=" << s.error << '\n';
        if (s.frames != 180 || !s.error.empty())
            ++failures;
    }
    if (!silent.wait_finished(15000) || silent.status().frames != 180 || !silent.status().error.empty())
        ++failures;
    if (!broken.wait_finished(1000) || broken.status().error.empty())
        ++failures;
    return failures ? 1 : 0;
}
