#include "zoom-iso-recorder.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QStandardPaths>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
#include <algorithm>
#include <cstring>

static QString default_iso_dir()
{
    const QString docs = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    const QString base = docs.isEmpty() ? QDir::homePath() : docs;
    return QDir(base).absoluteFilePath("CoreVideo ISOs");
}

static QString sanitized(const std::string &value, const QString &fallback)
{
    QString out = QString::fromStdString(value).trimmed();
    if (out.isEmpty()) out = fallback;
    for (QChar &ch : out) {
        if (!ch.isLetterOrNumber() && ch != '-' && ch != '_' && ch != '.')
            ch = '_';
    }
    while (out.contains("__")) out.replace("__", "_");
    return out.left(80);
}

static const char *assignment_label(AssignmentMode mode)
{
    switch (mode) {
    case AssignmentMode::ActiveSpeaker: return "active_speaker";
    case AssignmentMode::SpotlightIndex: return "spotlight";
    case AssignmentMode::ScreenShare: return "screen_share";
    case AssignmentMode::Participant:
    default: return "participant";
    }
}

ZoomIsoRecorder &ZoomIsoRecorder::instance()
{
    static ZoomIsoRecorder inst;
    return inst;
}

ZoomIsoRecorder::~ZoomIsoRecorder()
{
    stop();
}

bool ZoomIsoRecorder::WavFile::open(const QString &path,
                                    uint32_t rate,
                                    uint16_t channel_count)
{
    close();
#if defined(_WIN32)
    file = _wfopen(reinterpret_cast<const wchar_t *>(path.utf16()), L"wb");
#else
    file = fopen(path.toUtf8().constData(), "wb");
#endif
    if (!file) return false;
    sample_rate = rate;
    channels = std::max<uint16_t>(channel_count, 1);
    data_bytes = 0;

    const uint16_t audio_format = 1;
    const uint16_t bits_per_sample = 16;
    const uint32_t byte_rate = sample_rate * channels * bits_per_sample / 8;
    const uint16_t block_align = channels * bits_per_sample / 8;
    const uint32_t riff_size = 36;
    const uint32_t data_size = 0;

    fwrite("RIFF", 1, 4, file);
    fwrite(&riff_size, 4, 1, file);
    fwrite("WAVEfmt ", 1, 8, file);
    const uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, file);
    fwrite(&audio_format, 2, 1, file);
    fwrite(&channels, 2, 1, file);
    fwrite(&sample_rate, 4, 1, file);
    fwrite(&byte_rate, 4, 1, file);
    fwrite(&block_align, 2, 1, file);
    fwrite(&bits_per_sample, 2, 1, file);
    fwrite("data", 1, 4, file);
    fwrite(&data_size, 4, 1, file);
    return true;
}

void ZoomIsoRecorder::WavFile::write(const uint8_t *pcm, uint32_t byte_len)
{
    if (!file || !pcm || byte_len == 0) return;
    fwrite(pcm, 1, byte_len, file);
    data_bytes += byte_len;
}

void ZoomIsoRecorder::WavFile::close()
{
    if (!file) return;
    const uint32_t riff_size = 36 + data_bytes;
    fseek(file, 4, SEEK_SET);
    fwrite(&riff_size, 4, 1, file);
    fseek(file, 40, SEEK_SET);
    fwrite(&data_bytes, 4, 1, file);
    fclose(file);
    file = nullptr;
    data_bytes = 0;
}

bool ZoomIsoRecorder::start(const ZoomIsoRecordConfig &config,
                            std::string *error)
{
    ZoomIsoRecordConfig normalized = config;
    if (normalized.output_dir.empty())
        normalized.output_dir = default_iso_dir().toStdString();
    if (normalized.ffmpeg_path.empty())
        normalized.ffmpeg_path = "ffmpeg";
    const QString ffmpegProgram = QString::fromStdString(normalized.ffmpeg_path);
    const QFileInfo ffmpegInfo(ffmpegProgram);
    if ((ffmpegInfo.isRelative() &&
         QStandardPaths::findExecutable(ffmpegProgram).isEmpty()) ||
        (!ffmpegInfo.isRelative() && !ffmpegInfo.exists())) {
        if (error) {
            *error = "FFmpeg was not found on PATH. Set ffmpeg_path to a "
                     "valid ffmpeg executable.";
        }
        return false;
    }

    QDir dir(QString::fromStdString(normalized.output_dir));
    if (!dir.exists() && !dir.mkpath(".")) {
        if (error) *error = "Could not create ISO recording directory.";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mtx);
        for (auto &entry : m_sessions)
            close_session(entry.second);
        m_sessions.clear();
        m_config = normalized;
        m_started_program_recording = false;
        m_active.store(true, std::memory_order_release);
    }

    if (normalized.record_program && !obs_frontend_recording_active()) {
        obs_frontend_recording_start();
        std::lock_guard<std::mutex> lock(m_mtx);
        m_started_program_recording = true;
    }

    blog(LOG_INFO, "[obs-zoom-plugin] ISO recording started: dir=%s ffmpeg=%s",
         normalized.output_dir.c_str(), normalized.ffmpeg_path.c_str());
    return true;
}

void ZoomIsoRecorder::stop()
{
    if (!m_active.exchange(false, std::memory_order_acq_rel)) return;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        for (auto &entry : m_sessions)
            close_session(entry.second);
        m_sessions.clear();
        if (m_started_program_recording && obs_frontend_recording_active())
            obs_frontend_recording_stop();
        m_started_program_recording = false;
    }
    blog(LOG_INFO, "[obs-zoom-plugin] ISO recording stopped");
}

QJsonArray ZoomIsoRecorder::status_json() const
{
    QJsonArray arr;
    std::lock_guard<std::mutex> lock(m_mtx);
    for (const auto &entry : m_sessions) {
        const Session &s = entry.second;
        QJsonObject obj;
        obj["source_uuid"] = QString::fromStdString(s.source_uuid);
        obj["source"] = QString::fromStdString(s.source_name);
        obj["display_name"] = QString::fromStdString(s.display_name);
        obj["assignment"] = assignment_label(s.assignment);
        obj["configured_participant_id"] =
            static_cast<double>(s.configured_participant_id);
        obj["resolved_participant_id"] =
            static_cast<double>(s.resolved_participant_id);
        obj["width"] = static_cast<int>(s.width);
        obj["height"] = static_cast<int>(s.height);
        obj["video_frames"] = static_cast<int>(s.video_frames);
        obj["audio_chunks"] = static_cast<int>(s.audio_chunks);
        obj["video_path"] = s.video_path;
        obj["audio_path"] = s.audio_path;
        arr.append(obj);
    }
    return arr;
}

void ZoomIsoRecorder::on_output_updated(const ZoomOutputInfo &info)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (info.source_uuid.empty()) return;
    m_outputs[info.source_uuid] = info;
    if (!should_record(info, info.participant_id))
        close_session_locked(info.source_uuid);
}

void ZoomIsoRecorder::on_output_removed(const std::string &source_uuid)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    close_session_locked(source_uuid);
    m_outputs.erase(source_uuid);
}

bool ZoomIsoRecorder::should_record(const ZoomOutputInfo &info,
                                    uint32_t resolved_participant_id) const
{
    if (!m_active.load(std::memory_order_acquire)) return false;
    if (info.source_uuid.empty()) return false;
    if (info.assignment == AssignmentMode::ScreenShare) return false;
    if (resolved_participant_id == 0 &&
        info.assignment != AssignmentMode::ActiveSpeaker &&
        info.assignment != AssignmentMode::SpotlightIndex)
        return false;
    return true;
}

ZoomIsoRecorder::Session &
ZoomIsoRecorder::ensure_session_locked(const ZoomOutputInfo &info,
                                       uint32_t resolved_participant_id,
                                       uint32_t width,
                                       uint32_t height,
                                       uint64_t timestamp_ns)
{
    auto it = m_sessions.find(info.source_uuid);
    const bool needs_new = it == m_sessions.end() ||
        it->second.resolved_participant_id != resolved_participant_id ||
        it->second.width != width ||
        it->second.height != height;
    if (!needs_new) return it->second;

    if (it != m_sessions.end()) {
        close_session(it->second);
        m_sessions.erase(it);
    }

    Session session;
    session.source_uuid = info.source_uuid;
    session.source_name = info.source_name;
    session.display_name = info.display_name.empty()
        ? info.source_name : info.display_name;
    session.assignment = info.assignment;
    session.configured_participant_id = info.participant_id;
    session.resolved_participant_id = resolved_participant_id;
    session.width = width;
    session.height = height;
    session.started_ns = timestamp_ns;

    QDir root(QString::fromStdString(m_config.output_dir));
    const QString stamp = QDateTime::currentDateTimeUtc()
        .toString("yyyyMMdd-HHmmss-zzz");
    const QString name = sanitized(session.display_name,
        QStringLiteral("source"));
    const QString participant =
        resolved_participant_id == 0
            ? QStringLiteral("unresolved")
            : QString::number(resolved_participant_id);
    const QString base = QString("%1_%2_%3_%4")
        .arg(stamp, name, QString::fromLatin1(assignment_label(info.assignment)),
             participant);
    session.base_path = root.absoluteFilePath(base);
    session.video_path = session.base_path + ".mp4";
    session.audio_path = session.base_path + ".wav";

    session.ffmpeg = std::make_unique<QProcess>();
    session.ffmpeg->setProgram(QString::fromStdString(m_config.ffmpeg_path));
    session.ffmpeg->setArguments({
        "-hide_banner", "-loglevel", "warning", "-y",
        "-f", "rawvideo",
        "-pix_fmt", "yuv420p",
        "-s", QString("%1x%2").arg(width).arg(height),
        "-r", "30",
        "-i", "pipe:0",
        "-an",
        "-c:v", "libx264",
        "-preset", "veryfast",
        "-crf", "18",
        "-movflags", "+faststart",
        session.video_path,
    });
    session.ffmpeg->setProcessChannelMode(QProcess::MergedChannels);
    session.ffmpeg->start(QIODevice::WriteOnly);
    if (!session.ffmpeg->waitForStarted(2000)) {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] ISO ffmpeg failed to start for %s: %s",
             session.source_name.c_str(),
             session.ffmpeg->errorString().toUtf8().constData());
    }

    blog(LOG_INFO,
         "[obs-zoom-plugin] ISO session started: source=%s participant=%u video=%s audio=%s",
         session.source_name.c_str(), session.resolved_participant_id,
         session.video_path.toUtf8().constData(),
         session.audio_path.toUtf8().constData());

    auto inserted = m_sessions.emplace(info.source_uuid, std::move(session));
    return inserted.first->second;
}

void ZoomIsoRecorder::close_session_locked(const std::string &source_uuid)
{
    auto it = m_sessions.find(source_uuid);
    if (it == m_sessions.end()) return;
    close_session(it->second);
    m_sessions.erase(it);
}

void ZoomIsoRecorder::close_session(Session &session)
{
    session.wav.close();
    if (session.ffmpeg) {
        session.ffmpeg->closeWriteChannel();
        if (!session.ffmpeg->waitForFinished(5000))
            session.ffmpeg->kill();
    }
    blog(LOG_INFO,
         "[obs-zoom-plugin] ISO session closed: source=%s participant=%u frames=%u audio_chunks=%u",
         session.source_name.c_str(), session.resolved_participant_id,
         session.video_frames, session.audio_chunks);
}

void ZoomIsoRecorder::record_video_frame(const ZoomOutputInfo &info,
                                         uint32_t resolved_participant_id,
                                         uint32_t width,
                                         uint32_t height,
                                         const uint8_t *y,
                                         const uint8_t *u,
                                         const uint8_t *v,
                                         uint32_t stride_y,
                                         uint32_t stride_uv,
                                         uint64_t timestamp_ns)
{
    if (!y || !u || !v || width == 0 || height == 0) return;
    std::lock_guard<std::mutex> lock(m_mtx);
    if (!should_record(info, resolved_participant_id)) return;
    Session &session = ensure_session_locked(info, resolved_participant_id,
                                             width, height, timestamp_ns);
    if (!session.ffmpeg ||
        session.ffmpeg->state() != QProcess::Running)
        return;

    for (uint32_t row = 0; row < height; ++row)
        session.ffmpeg->write(reinterpret_cast<const char *>(y + row * stride_y),
                              width);
    for (uint32_t row = 0; row < height / 2; ++row)
        session.ffmpeg->write(reinterpret_cast<const char *>(u + row * stride_uv),
                              width / 2);
    for (uint32_t row = 0; row < height / 2; ++row)
        session.ffmpeg->write(reinterpret_cast<const char *>(v + row * stride_uv),
                              width / 2);
    ++session.video_frames;
}

void ZoomIsoRecorder::record_audio_frame(const ZoomOutputInfo &info,
                                         uint32_t resolved_participant_id,
                                         const uint8_t *pcm,
                                         uint32_t byte_len,
                                         uint32_t sample_rate,
                                         uint16_t channels,
                                         uint64_t timestamp_ns)
{
    if (!pcm || byte_len == 0 || sample_rate == 0) return;
    std::lock_guard<std::mutex> lock(m_mtx);
    if (!should_record(info, resolved_participant_id)) return;
    auto it = m_sessions.find(info.source_uuid);
    if (it == m_sessions.end() ||
        it->second.resolved_participant_id != resolved_participant_id) {
        return;
    }
    Session &session = it->second;
    if (!session.wav.file &&
        !session.wav.open(session.audio_path, sample_rate, channels)) {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] ISO WAV open failed: %s",
             session.audio_path.toUtf8().constData());
        return;
    }
    if (session.wav.sample_rate == sample_rate &&
        session.wav.channels == std::max<uint16_t>(channels, 1)) {
        session.wav.write(pcm, byte_len);
        ++session.audio_chunks;
    }
}
