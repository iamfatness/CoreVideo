#pragma once

#include "iso-encoder-plan.h"
#include "iso-track-writer.h"
#include "zoom-output-manager.h"
#include "zoom-types.h"
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
#include <unordered_set>
#include <vector>

struct ZoomIsoRecordConfig {
    std::string output_dir;
    std::string ffmpeg_path = "ffmpeg";
    std::string video_encoder = "libx264";
    bool record_program = true;
    std::vector<std::string> selected_source_uuids;
};

class ZoomIsoRecorder
{
  public:
    static ZoomIsoRecorder &instance();

    bool start(const ZoomIsoRecordConfig &config, std::string *error = nullptr);
    void stop();
    bool active() const { return m_active.load(std::memory_order_acquire); }
    QJsonObject status_overview();
    QJsonArray status_json();

    void on_output_updated(const ZoomOutputInfo &info);
    void on_output_removed(const std::string &source_uuid);

    void record_video_frame(const ZoomOutputInfo &info, uint32_t resolved_participant_id,
                            uint32_t width, uint32_t height, const uint8_t *y, const uint8_t *u,
                            const uint8_t *v, uint32_t stride_y, uint32_t stride_uv,
                            uint64_t timestamp_ns);
    void record_audio_frame(const ZoomOutputInfo &info, uint32_t resolved_participant_id,
                            const uint8_t *pcm, uint32_t byte_len, uint32_t sample_rate,
                            uint16_t channels, uint64_t timestamp_ns);

  private:
    ZoomIsoRecorder() = default;
    ~ZoomIsoRecorder();

    struct Session {
        ZoomOutputInfo info;
        uint32_t participant = 0;
        std::string video_owner, audio_owner, encoder;
        uint64_t video_ns = 0, audio_ns = 0, epoch_ns = 0;
        QString path;
        bool error_logged = false;
        std::unique_ptr<IsoTrackWriter> writer;
    };
    Session &ensure_session_locked(const ZoomOutputInfo &info, uint32_t participant, uint64_t ns);
    QJsonObject session_status_locked(Session &session, bool completed, uint64_t end_ns = 0);
    bool should_record(const ZoomOutputInfo &info, uint32_t participant) const;
    IsoEncoderAvailability m_encoder_avail;
    int m_nvenc_session_limit = 8;
    mutable std::mutex m_mtx;
    std::atomic<bool> m_active{false}, m_stopping{false};
    bool m_started_program_recording = false;
    uint64_t m_epoch_ns = 0;
    ZoomIsoRecordConfig m_config;
    std::string m_requested_video_encoder;
    QString m_status_warning;
    std::unordered_map<std::string, ZoomOutputInfo> m_outputs;
    std::unordered_map<uint32_t, Session> m_sessions;
    std::vector<QJsonObject> m_completed_sessions;
};
