#pragma once

#include "zoom-output-manager.h"
#include "zoom-types.h"
#include <QJsonArray>
#include <QProcess>
#include <QString>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct ZoomIsoRecordConfig {
    std::string output_dir;
    std::string ffmpeg_path = "ffmpeg";
    bool record_program = true;
};

class ZoomIsoRecorder {
public:
    static ZoomIsoRecorder &instance();

    bool start(const ZoomIsoRecordConfig &config, std::string *error = nullptr);
    void stop();
    bool active() const { return m_active.load(std::memory_order_acquire); }
    QJsonArray status_json() const;

    void on_output_updated(const ZoomOutputInfo &info);
    void on_output_removed(const std::string &source_uuid);

    void record_video_frame(const ZoomOutputInfo &info,
                            uint32_t resolved_participant_id,
                            uint32_t width,
                            uint32_t height,
                            const uint8_t *y,
                            const uint8_t *u,
                            const uint8_t *v,
                            uint32_t stride_y,
                            uint32_t stride_uv,
                            uint64_t timestamp_ns);
    void record_audio_frame(const ZoomOutputInfo &info,
                            uint32_t resolved_participant_id,
                            const uint8_t *pcm,
                            uint32_t byte_len,
                            uint32_t sample_rate,
                            uint16_t channels,
                            uint64_t timestamp_ns);

private:
    ZoomIsoRecorder() = default;
    ~ZoomIsoRecorder();

    struct WavFile {
        bool open(const QString &path, uint32_t sample_rate, uint16_t channels);
        void write(const uint8_t *pcm, uint32_t byte_len);
        void close();
        FILE *file = nullptr;
        uint32_t data_bytes = 0;
        uint32_t sample_rate = 0;
        uint16_t channels = 0;
    };

    struct Session {
        std::string source_uuid;
        std::string source_name;
        std::string display_name;
        AssignmentMode assignment = AssignmentMode::Participant;
        uint32_t configured_participant_id = 0;
        uint32_t resolved_participant_id = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t video_frames = 0;
        uint32_t audio_chunks = 0;
        uint64_t started_ns = 0;
        QString base_path;
        QString video_path;
        QString audio_path;
        std::unique_ptr<QProcess> ffmpeg;
        WavFile wav;
    };

    Session &ensure_session_locked(const ZoomOutputInfo &info,
                                   uint32_t resolved_participant_id,
                                   uint32_t width,
                                   uint32_t height,
                                   uint64_t timestamp_ns);
    void close_session_locked(const std::string &source_uuid);
    void close_session(Session &session);
    bool should_record(const ZoomOutputInfo &info,
                       uint32_t resolved_participant_id) const;

    mutable std::mutex m_mtx;
    std::atomic<bool> m_active{false};
    bool m_started_program_recording = false;
    ZoomIsoRecordConfig m_config;
    std::unordered_map<std::string, ZoomOutputInfo> m_outputs;
    std::unordered_map<std::string, Session> m_sessions;
};
