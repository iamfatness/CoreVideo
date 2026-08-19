#pragma once

#include "zoom-output-manager.h"
#include "zoom-types.h"
#include "iso-encoder-plan.h"
#include "iso-ffmpeg-pipe.h"
#include <QJsonArray>
#include <QJsonObject>
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
    std::string video_encoder = "libx264";
    bool record_program = true;
};

class ZoomIsoRecorder {
public:
    static ZoomIsoRecorder &instance();

    bool start(const ZoomIsoRecordConfig &config, std::string *error = nullptr);
    void stop();
    bool active() const { return m_active.load(std::memory_order_acquire); }
    QJsonObject status_overview();
    QJsonArray status_json();

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
        // Returns false if a write failed; sets out_disk_full when the
        // failure was caused by the disk being full (ENOSPC).
        bool write(const uint8_t *pcm, uint32_t byte_len, bool *out_disk_full);
        void close();
        FILE *file = nullptr;
        uint32_t data_bytes = 0;
        uint32_t sample_rate = 0;
        uint16_t channels = 0;
        bool write_failed = false;
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
        uint64_t video_frames_dropped = 0;
        uint64_t last_drop_ns = 0;
        bool backlog_reported = false;
        uint64_t ffmpeg_started_ns = 0;
        uint64_t finishing_since_ns = 0;
        uint64_t unresolved_since_ns = 0;
        uint32_t audio_chunks = 0;
        uint64_t started_ns = 0;
        uint64_t last_video_ns = 0;
        uint64_t last_audio_ns = 0;
        QString base_path;
        QString video_path;
        QString audio_path;
        QString ffmpeg_error;
        QString ffmpeg_output_tail;
        int ffmpeg_exit_code = -1;
        QString ffmpeg_exit_status;
        bool ffmpeg_error_logged = false;
        bool disk_full = false;
        std::string requested_video_encoder;
        std::string video_encoder;
        bool encoder_fallback = false;
        // Raw-pipe process feed — QProcess is banned here: its Windows stdin
        // chaining needs the owner thread's Qt event loop, and every caller
        // of record_video_frame() is a plain std::thread (measured live
        // 2026-08-18: exactly 5 frames per session, then a permanent
        // backlog). See iso-ffmpeg-pipe.h.
        std::unique_ptr<IsoFfmpegPipe> ffmpeg;
        WavFile wav;
    };

    Session &ensure_session_locked(const ZoomOutputInfo &info,
                                   uint32_t resolved_participant_id,
                                   uint32_t width,
                                   uint32_t height,
                                   uint64_t timestamp_ns);
    void close_session_locked(const std::string &source_uuid);
    // Non-blocking close for mid-recording paths (frame-dispatch thread must
    // never wait on an encoder): signals EOF and parks the session on
    // m_finishing; reap_finishing_locked() finalizes it from status polls.
    void begin_finishing_locked(Session &&session);
    void reap_finishing_locked();
    void sweep_unresolved_locked();
    QJsonObject session_status_json_locked(Session &session, bool completed);
    void refresh_ffmpeg_status_locked(Session &session);
    void mark_ffmpeg_failure_locked(Session &session, const QString &message);
    void mark_disk_full_locked(Session &session);
    // If the session was flagged disk-full, close it cleanly (finalizing the
    // WAV header and reaping FFmpeg). No-op otherwise. Note: this may
    // invalidate any Session reference for source_uuid.
    void close_session_on_disk_full_locked(const std::string &source_uuid);
    bool should_record(const ZoomOutputInfo &info,
                       uint32_t resolved_participant_id) const;

    IsoEncoderAvailability m_encoder_avail;
    int m_nvenc_session_limit = 8;
    // source_uuid -> encoder to use after a startup failure demoted it.
    std::unordered_map<std::string, std::string> m_encoder_demotions;

    mutable std::mutex m_mtx;
    std::atomic<bool> m_active{false};
    bool m_started_program_recording = false;
    ZoomIsoRecordConfig m_config;
    std::string m_requested_video_encoder;
    QString m_status_warning;
    std::unordered_map<std::string, ZoomOutputInfo> m_outputs;
    std::unordered_map<std::string, Session> m_sessions;
    std::vector<Session> m_finishing;
    std::vector<QJsonObject> m_completed_sessions;
};
