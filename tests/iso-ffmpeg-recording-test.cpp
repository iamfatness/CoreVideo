// Optional real-codec test. Run in an empty output directory:
// CoreVideoIsoRecordingTest <ffmpeg> [encoder=libx264] [tracks=1]
// Creates two consecutive 10-second fragmented MP4 takes, with a half-second
// input gap. Probe/decode the output files independently after this test.
#include "iso-ffmpeg-pipe.h"
#include "iso-video-pacer.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    const std::string encoder = argc > 2 ? argv[2] : "libx264";
    const int tracks = argc > 3 ? std::stoi(argv[3]) : 1;
    if (tracks < 1 || tracks > 8) return 2;
    constexpr size_t bytes = 1920 * 1080 * 3 / 2;
    int failures = 0;
    for (int take = 0; take < 2; ++take) {
        std::vector<std::unique_ptr<IsoFfmpegPipe>> pipes;
        std::vector<uint64_t> next(tracks, 0), accepted(tracks, 0);
        for (int track = 0; track < tracks; ++track) {
            const auto name = "take-" + std::to_string(take) + "-track-" + std::to_string(track);
            auto pipe = std::make_unique<IsoFfmpegPipe>();
            std::vector<std::string> args = {
                "-hide_banner", "-loglevel", "warning", "-n", "-f", "rawvideo",
                "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "30", "-i", "pipe:0",
                "-an", "-c:v", encoder};
            if (encoder == "libx264")
                args.insert(args.end(), {"-preset", "veryfast", "-crf", "18"});
            else
                args.insert(args.end(), {"-b:v", "12M", "-maxrate", "20M", "-bufsize", "24M"});
            args.insert(args.end(), {"-movflags", "+frag_keyframe+empty_moov", name + ".mp4"});
            std::string error;
            if (!pipe->start(argv[1], args, name + ".log",
                             bytes * IsoFfmpegPipe::kStartupFrames, &error)) {
                std::cerr << error << '\n';
                return 1;
            }
            pipes.push_back(std::move(pipe));
        }
        const auto start = std::chrono::steady_clock::now();
        for (int tick = 0; tick < 300; ++tick) {
            const uint64_t timestamp = tick * kIsoVideoFramePeriodNs;
            std::this_thread::sleep_until(start + std::chrono::nanoseconds(timestamp));
            if (tick >= 90 && tick < 105) continue;
            for (int track = 0; track < tracks; ++track) {
                auto candidate = next[track];
                const auto due = iso_video_frames_due(candidate, timestamp);
                std::vector<uint8_t> pixels(bytes, 128);
                // Alternating luma patterns exercise real encode work.
                for (size_t row = 0; row < 1080; ++row)
                    std::fill_n(pixels.data() + row * 1920, 1920,
                                static_cast<uint8_t>(16 + ((row + tick * 4 + track * 16) % 220)));
                if (!pipes[track]->try_queue(std::move(pixels), due)) {
                    ++failures;
                    std::cerr << "rejected take=" << take << " track=" << track << " tick=" << tick << '\n';
                    // Same fail-closed policy as the recorder: do not keep
                    // generating a timeline that silently skips missing time.
                    for (auto &pipe : pipes) pipe->close_stdin();
                    return 1;
                }
                next[track] = candidate;
                accepted[track] += due;
            }
        }
        for (auto &pipe : pipes) pipe->close_stdin();
        for (int track = 0; track < tracks; ++track) {
            auto &pipe = *pipes[track];
            if (!pipe.wait_finished(15000)) { pipe.kill(); ++failures; }
            const auto status = pipe.queue_status();
            if (pipe.exit_code() != 0 || accepted[track] != 300 || status.written_frames != 300)
                ++failures;
            std::cout << "take=" << take << " track=" << track
                      << " accepted=" << accepted[track] << " written=" << status.written_frames
                      << " peak=" << status.peak_frames << " exit=" << pipe.exit_code() << '\n';
        }
    }
    return failures ? 1 : 0;
}
