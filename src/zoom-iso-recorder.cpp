#include "zoom-iso-recorder.h"
#include "iso-encoder-plan.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStringList>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <set>

static constexpr qint64 kIsoMinimumFreeBytes = 2ll * 1024ll * 1024ll * 1024ll;
static constexpr qint64 kIsoWarningFreeBytes = 10ll * 1024ll * 1024ll * 1024ll;
static constexpr int kFfmpegOutputTailChars = 2048;
static constexpr size_t kMaxCompletedIsoSessions = 24;

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

static QString bytes_text(qint64 bytes)
{
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0)
        return QString("%1 GB").arg(gb, 0, 'f', 1);
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return QString("%1 MB").arg(mb, 0, 'f', 1);
}

static std::string normalized_video_encoder(const std::string &encoder);
static int count_obs_nvenc_encoders();
static bool is_hardware_encoder(const std::string &encoder);
static bool ffmpeg_encoder_available(const QString &ffmpeg_path,
                                     const std::string &encoder,
                                     std::string *error);
static QStringList ffmpeg_video_encoder_args(const std::string &encoder);

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

bool ZoomIsoRecorder::WavFile::write(const uint8_t *pcm, uint32_t byte_len,
                                     bool *out_disk_full)
{
    if (out_disk_full) *out_disk_full = false;
    if (!file || !pcm || byte_len == 0) return true;
    errno = 0;
    const size_t written = fwrite(pcm, 1, byte_len, file);
    data_bytes += static_cast<uint32_t>(written);
    if (written < byte_len) {
        write_failed = true;
        if (out_disk_full && (errno == ENOSPC || ferror(file)))
            *out_disk_full = (errno == ENOSPC);
        return false;
    }
    return true;
}

void ZoomIsoRecorder::WavFile::close()
{
    if (!file) return;
    // Finalize the RIFF/data size fields on every close path (including
    // when a write failed mid-stream) so the file stays valid and playable.
    // data_bytes reflects the bytes actually written, so a truncated file
    // still describes its real payload size.
    const uint32_t riff_size = 36 + data_bytes;
    if (fseek(file, 4, SEEK_SET) == 0)
        fwrite(&riff_size, 4, 1, file);
    if (fseek(file, 40, SEEK_SET) == 0)
        fwrite(&data_bytes, 4, 1, file);
    fclose(file);
    file = nullptr;
    data_bytes = 0;
    write_failed = false;
}

bool ZoomIsoRecorder::start(const ZoomIsoRecordConfig &config,
                            std::string *error)
{
    ZoomIsoRecordConfig normalized = config;
    if (normalized.output_dir.empty())
        normalized.output_dir = default_iso_dir().toStdString();
    if (normalized.ffmpeg_path.empty())
        normalized.ffmpeg_path = "ffmpeg";
    const std::string requested_encoder =
        normalized_video_encoder(normalized.video_encoder);
    normalized.video_encoder =
        requested_encoder;
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
    std::string encoder_error;
    QString status_warning;
    // Probe which hardware encoder families this FFmpeg build offers; the
    // per-session placement logic (iso-encoder-plan.h) uses this to spread
    // sessions across NVENC/QSV/AMF/x264 within hardware session budgets.
    IsoEncoderAvailability avail;
    avail.nvenc = ffmpeg_encoder_available(ffmpegProgram, "h264_nvenc", nullptr);
    avail.qsv = ffmpeg_encoder_available(ffmpegProgram, "h264_qsv", nullptr);
    avail.amf = ffmpeg_encoder_available(ffmpegProgram, "h264_amf", nullptr);
    if (normalized.video_encoder == "auto") {
        blog(LOG_INFO,
             "[obs-zoom-plugin] ISO encoder placement: automatic "
             "(nvenc=%d qsv=%d amf=%d, NVENC session limit %d, "
             "OBS NVENC encoders active %d)",
             avail.nvenc, avail.qsv, avail.amf,
             iso_nvenc_default_session_limit(), count_obs_nvenc_encoders());
    } else if (!ffmpeg_encoder_available(ffmpegProgram,
                                         normalized.video_encoder,
                                         &encoder_error)) {
        if (is_hardware_encoder(normalized.video_encoder) &&
            ffmpeg_encoder_available(ffmpegProgram, "libx264", nullptr)) {
            status_warning = QString("Requested hardware encoder '%1' was not "
                                     "available in FFmpeg; falling back to CPU "
                                     "libx264 for this ISO run.")
                                 .arg(QString::fromStdString(normalized.video_encoder));
            blog(LOG_WARNING, "[obs-zoom-plugin] %s",
                 status_warning.toUtf8().constData());
            normalized.video_encoder = "libx264";
        } else {
            if (error)
                *error = encoder_error;
            return false;
        }
    }

    QDir dir(QString::fromStdString(normalized.output_dir));
    if (!dir.exists() && !dir.mkpath(".")) {
        if (error) *error = "Could not create ISO recording directory.";
        return false;
    }
    const QStorageInfo storage(dir.absolutePath());
    if (storage.isValid() && storage.isReady()) {
        const qint64 available = storage.bytesAvailable();
        if (available < kIsoMinimumFreeBytes) {
            if (error) {
                *error = QString("Only %1 is free in the ISO output folder. "
                                 "Free at least %2 before starting ISO recording.")
                             .arg(bytes_text(available),
                                  bytes_text(kIsoMinimumFreeBytes))
                             .toStdString();
            }
            return false;
        }
        if (available < kIsoWarningFreeBytes) {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] ISO recording starting with low disk space: available=%s dir=%s",
                 bytes_text(available).toUtf8().constData(),
                 normalized.output_dir.c_str());
        }
    } else {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] ISO recording could not validate disk space for dir=%s",
             normalized.output_dir.c_str());
    }

    {
        std::lock_guard<std::mutex> lock(m_mtx);
        for (auto &entry : m_sessions)
            begin_finishing_locked(std::move(entry.second));
        m_sessions.clear();
        reap_finishing_locked();
        m_config = normalized;
        m_requested_video_encoder = requested_encoder;
        m_encoder_avail = avail;
        m_nvenc_session_limit = iso_nvenc_default_session_limit();
        m_encoder_demotions.clear();
        m_status_warning = status_warning;
        m_started_program_recording = false;
        m_completed_sessions.clear();
        m_active.store(true, std::memory_order_release);
    }

    if (normalized.record_program && !obs_frontend_recording_active()) {
        obs_frontend_recording_start();
        std::lock_guard<std::mutex> lock(m_mtx);
        m_started_program_recording = true;
    }

    blog(LOG_INFO,
         "[obs-zoom-plugin] ISO recording started: dir=%s ffmpeg=%s encoder=%s",
         normalized.output_dir.c_str(), normalized.ffmpeg_path.c_str(),
         normalized.video_encoder.c_str());
    return true;
}

void ZoomIsoRecorder::stop()
{
    if (!m_active.exchange(false, std::memory_order_acq_rel)) return;

    // Take the sessions out and finish them WITHOUT holding m_mtx. The old
    // close-in-place loop waited up to 5 s per session under the lock; with
    // 8 sessions the engine-IPC dispatch thread (which needs m_mtx to write
    // frames) blocked for ~40 s, the engine's pipe backed up until its
    // heartbeat write stalled, and the plugin declared the engine dead and
    // tore the meeting down (2026-08-08 incident).
    std::vector<Session> finishing;
    bool stop_program = false;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        finishing.reserve(m_sessions.size() + m_finishing.size());
        for (auto &entry : m_sessions)
            finishing.push_back(std::move(entry.second));
        m_sessions.clear();
        for (auto &pending : m_finishing)
            finishing.push_back(std::move(pending));
        m_finishing.clear();
        stop_program = m_started_program_recording &&
                       obs_frontend_recording_active();
        m_started_program_recording = false;
    }
    if (stop_program)
        obs_frontend_recording_stop();

    // Phase 1: signal EOF to every encoder at once so they all finalize
    // their MP4s in parallel instead of serially.
    for (Session &session : finishing) {
        session.wav.close();
        if (session.ffmpeg) {
            refresh_ffmpeg_status_locked(session);
            session.ffmpeg->close_stdin();
        }
    }

    // Phase 2: wait against one shared deadline; anything still running at
    // the end gets terminated with an honest status (a killed FFmpeg leaves
    // the MP4 without its final index).
    QElapsedTimer deadline;
    deadline.start();
    constexpr qint64 kShutdownBudgetMs = 15000;
    for (Session &session : finishing) {
        if (!session.ffmpeg)
            continue;
        qint64 remaining = kShutdownBudgetMs - deadline.elapsed();
        if (remaining < 100)
            remaining = 100;
        if (!session.ffmpeg->wait_finished(static_cast<int>(remaining))) {
            mark_ffmpeg_failure_locked(
                session,
                QString("FFmpeg did not exit within the %1s shutdown budget "
                        "and was terminated — the recording may be missing "
                        "its final index (moov).")
                    .arg(kShutdownBudgetMs / 1000));
            session.ffmpeg->kill();
            if (!session.ffmpeg->wait_finished(2000) &&
                session.ffmpeg->running()) {
                blog(LOG_WARNING,
                     "[obs-zoom-plugin] ISO ffmpeg for %s did not exit after "
                     "kill(); process may not be reaped (pid=%lld)",
                     session.source_name.c_str(), session.ffmpeg->pid());
            }
        }
        refresh_ffmpeg_status_locked(session);
        blog(LOG_INFO,
             "[obs-zoom-plugin] ISO session closed: source=%s participant=%u frames=%u audio_chunks=%u",
             session.source_name.c_str(), session.resolved_participant_id,
             session.video_frames, session.audio_chunks);
    }

    {
        std::lock_guard<std::mutex> lock(m_mtx);
        for (Session &session : finishing) {
            QJsonObject completed =
                session_status_json_locked(session, true);
            completed["completed_at"] =
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            m_completed_sessions.insert(m_completed_sessions.begin(),
                                        completed);
        }
        if (m_completed_sessions.size() > kMaxCompletedIsoSessions)
            m_completed_sessions.resize(kMaxCompletedIsoSessions);
    }
    blog(LOG_INFO, "[obs-zoom-plugin] ISO recording stopped");
}

QJsonArray ZoomIsoRecorder::status_json()
{
    QJsonArray arr;
    std::lock_guard<std::mutex> lock(m_mtx);
    reap_finishing_locked();
    sweep_unresolved_locked();
    for (auto &entry : m_sessions) {
        arr.append(session_status_json_locked(entry.second, false));
    }
    for (auto &finishing : m_finishing)
        arr.append(session_status_json_locked(finishing, false));
    for (const QJsonObject &completed : m_completed_sessions)
        arr.append(completed);
    return arr;
}

QJsonObject ZoomIsoRecorder::status_overview()
{
    QJsonObject obj;
    std::lock_guard<std::mutex> lock(m_mtx);
    obj["active"] = m_active.load(std::memory_order_acquire);
    obj["output_dir"] = QString::fromStdString(m_config.output_dir);
    obj["ffmpeg_path"] = QString::fromStdString(m_config.ffmpeg_path);
    obj["requested_video_encoder"] =
        QString::fromStdString(m_requested_video_encoder.empty()
            ? m_config.video_encoder
            : m_requested_video_encoder);
    obj["video_encoder"] = QString::fromStdString(m_config.video_encoder);
    obj["encoder_fallback"] =
        !m_requested_video_encoder.empty() &&
        m_requested_video_encoder != m_config.video_encoder;
    obj["hardware_encoder"] = is_hardware_encoder(m_config.video_encoder);
    obj["record_program"] = m_config.record_program;
    obj["program_recording_started_by_corevideo"] = m_started_program_recording;
    obj["session_count"] = static_cast<int>(m_sessions.size());
    obj["completed_session_count"] = static_cast<int>(m_completed_sessions.size());
    obj["warning"] = m_status_warning;

    const QDir dir(QString::fromStdString(m_config.output_dir));
    const QStorageInfo storage(dir.absolutePath());
    if (storage.isValid() && storage.isReady()) {
        obj["disk_available_bytes"] = static_cast<double>(storage.bytesAvailable());
        obj["disk_warning"] = storage.bytesAvailable() < kIsoWarningFreeBytes;
    } else {
        obj["disk_available_bytes"] = -1.0;
        obj["disk_warning"] = true;
    }
    return obj;
}

void ZoomIsoRecorder::on_output_updated(const ZoomOutputInfo &info)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (info.source_uuid.empty()) return;
    m_outputs[info.source_uuid] = info;
    auto it = m_sessions.find(info.source_uuid);
    if (it == m_sessions.end())
        return;
    if (should_record(info, info.participant_id)) {
        it->second.unresolved_since_ns = 0;
        return;
    }
    if (info.assignment == AssignmentMode::ScreenShare) {
        // Assignment changed away from a recordable mode — final.
        close_session_locked(info.source_uuid);
        return;
    }
    // Unresolved participant: often transient (engine reconnect, camera
    // toggle). Start the grace clock instead of finalizing the file;
    // sweep_unresolved_locked() closes it if this persists.
    if (it->second.unresolved_since_ns == 0)
        it->second.unresolved_since_ns = os_gettime_ns();
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
        // Resolution or participant change: finalize the old segment
        // without blocking (we are on the frame-dispatch thread).
        begin_finishing_locked(std::move(it->second));
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
    session.requested_video_encoder = m_requested_video_encoder.empty()
        ? normalized_video_encoder(m_config.video_encoder)
        : m_requested_video_encoder;
    // Encoder placement: hardware sessions are a shared, finite budget
    // (OBS's own outputs included), so each ISO session is placed
    // individually — NVENC while the budget lasts, then QSV/AMF/x264.
    // A session that already failed once on a hardware encoder was demoted
    // and stays demoted for this run.
    const auto demoted = m_encoder_demotions.find(session.source_uuid);
    if (demoted != m_encoder_demotions.end()) {
        session.video_encoder = demoted->second;
    } else {
        int nvenc_in_use_iso = 0;
        for (const auto &other : m_sessions) {
            if (other.second.video_encoder == "h264_nvenc" &&
                other.second.ffmpeg && other.second.ffmpeg->running())
                ++nvenc_in_use_iso;
        }
        const int remaining = m_nvenc_session_limit -
                              count_obs_nvenc_encoders() - nvenc_in_use_iso;
        session.video_encoder = iso_choose_session_encoder(
            normalized_video_encoder(m_config.video_encoder), remaining,
            m_encoder_avail);
    }
    session.encoder_fallback =
        session.requested_video_encoder != session.video_encoder;

    session.ffmpeg = std::make_unique<IsoFfmpegPipe>();
    std::vector<std::string> args = {
        "-hide_banner", "-loglevel", "warning", "-y",
        "-f", "rawvideo",
        "-pix_fmt", "yuv420p",
        "-s", std::to_string(width) + "x" + std::to_string(height),
        "-r", "30",
        "-i", "pipe:0",
        "-an",
    };
    for (const QString &a : ffmpeg_video_encoder_args(session.video_encoder))
        args.push_back(a.toStdString());
    // Fragmented MP4: the file is playable up to the last written
    // fragment even if FFmpeg is killed or the machine loses power
    // (+faststart wrote the index only during finalize — and rewrote
    // the whole file, which made shutdown slow on synced folders and
    // left killed recordings unplayable).
    args.push_back("-movflags");
    args.push_back("+frag_keyframe+empty_moov");
    args.push_back(session.video_path.toStdString());

    // Queue bound = 4 whole frames, the same realtime backpressure budget
    // the drop path enforces; the writer thread does blocking pipe writes
    // beneath it. FFmpeg's stdout+stderr go to a sidecar log so nothing has
    // to drain them and the tail survives any exit.
    const size_t frame_cap =
        static_cast<size_t>(width) * height * 3 / 2 * 4;
    std::string start_error;
    if (!session.ffmpeg->start(
            m_config.ffmpeg_path, args,
            (session.base_path + ".ffmpeg.log").toStdString(),
            frame_cap, &start_error)) {
        session.ffmpeg_error = QString::fromStdString(start_error);
        session.ffmpeg_error_logged = true;
        blog(LOG_WARNING,
             "[obs-zoom-plugin] ISO ffmpeg failed to start for %s: %s",
             session.source_name.c_str(), start_error.c_str());
    } else {
        session.ffmpeg_started_ns = os_gettime_ns();
    }

    blog(LOG_INFO,
         "[obs-zoom-plugin] ISO session started: source=%s participant=%u encoder=%s video=%s audio=%s",
         session.source_name.c_str(), session.resolved_participant_id,
         session.video_encoder.c_str(),
         session.video_path.toUtf8().constData(),
         session.audio_path.toUtf8().constData());

    auto inserted = m_sessions.emplace(info.source_uuid, std::move(session));
    return inserted.first->second;
}

void ZoomIsoRecorder::close_session_locked(const std::string &source_uuid)
{
    auto it = m_sessions.find(source_uuid);
    if (it == m_sessions.end()) return;
    begin_finishing_locked(std::move(it->second));
    m_sessions.erase(it);
}

// Mid-recording closes must never wait on an encoder: they run on the
// frame-dispatch thread, and a blocked dispatch thread starves the engine's
// IPC pipe (the 2026-08-08 engine-"death" incidents). Signal EOF, park the
// session, and let reap_finishing_locked() finalize it from status polls.
void ZoomIsoRecorder::begin_finishing_locked(Session &&session)
{
    session.wav.close();
    if (session.ffmpeg) {
        refresh_ffmpeg_status_locked(session);
        session.ffmpeg->close_stdin();
    }
    session.finishing_since_ns = os_gettime_ns();
    m_finishing.push_back(std::move(session));
}

void ZoomIsoRecorder::reap_finishing_locked()
{
    constexpr uint64_t kFinishingKillNs = 15000000000ULL;
    const uint64_t now_ns = os_gettime_ns();
    for (auto it = m_finishing.begin(); it != m_finishing.end();) {
        Session &session = *it;
        bool done = true;
        if (session.ffmpeg && session.ffmpeg->running()) {
            const uint64_t age_ns =
                now_ns >= session.finishing_since_ns
                    ? now_ns - session.finishing_since_ns
                    : 0;
            if (age_ns > kFinishingKillNs) {
                mark_ffmpeg_failure_locked(
                    session,
                    QStringLiteral(
                        "FFmpeg did not exit within the shutdown budget "
                        "and was terminated — the recording may be missing "
                        "its final index (moov)."));
                session.ffmpeg->kill();
                session.ffmpeg->wait_finished(100);
            }
            done = !session.ffmpeg->running();
        }
        if (!done) {
            ++it;
            continue;
        }
        refresh_ffmpeg_status_locked(session);
        blog(LOG_INFO,
             "[obs-zoom-plugin] ISO session closed: source=%s participant=%u frames=%u audio_chunks=%u",
             session.source_name.c_str(), session.resolved_participant_id,
             session.video_frames, session.audio_chunks);
        QJsonObject completed = session_status_json_locked(session, true);
        completed["completed_at"] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        m_completed_sessions.insert(m_completed_sessions.begin(), completed);
        if (m_completed_sessions.size() > kMaxCompletedIsoSessions)
            m_completed_sessions.resize(kMaxCompletedIsoSessions);
        it = m_finishing.erase(it);
    }
}

// Closes sessions whose participant has stayed unresolved past the grace
// window. Transient unresolves (engine reconnect, camera toggles) must NOT
// finalize the file — that was fragmenting recordings into segments.
void ZoomIsoRecorder::sweep_unresolved_locked()
{
    constexpr uint64_t kUnresolvedGraceNs = 60000000000ULL;
    const uint64_t now_ns = os_gettime_ns();
    std::vector<std::string> to_close;
    for (auto &entry : m_sessions) {
        Session &session = entry.second;
        if (session.unresolved_since_ns == 0 ||
            now_ns < session.unresolved_since_ns)
            continue;
        if (now_ns - session.unresolved_since_ns > kUnresolvedGraceNs)
            to_close.push_back(entry.first);
    }
    for (const std::string &uuid : to_close) {
        blog(LOG_INFO,
             "[obs-zoom-plugin] ISO session %s unresolved for over 60s; "
             "finalizing its recording",
             uuid.c_str());
        close_session_locked(uuid);
    }
}


QJsonObject ZoomIsoRecorder::session_status_json_locked(Session &s,
                                                        bool completed)
{
    refresh_ffmpeg_status_locked(s);
    QJsonObject obj;
    obj["completed"] = completed;
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
    obj["video_frames_dropped"] =
        static_cast<double>(s.video_frames_dropped);
    obj["audio_chunks"] = static_cast<int>(s.audio_chunks);
    const uint64_t now_ns = os_gettime_ns();
    obj["elapsed_ms"] = s.started_ns > 0 && now_ns >= s.started_ns
        ? static_cast<double>((now_ns - s.started_ns) / 1000000ULL)
        : 0.0;
    obj["last_video_age_ms"] = s.last_video_ns > 0 && now_ns >= s.last_video_ns
        ? static_cast<double>((now_ns - s.last_video_ns) / 1000000ULL)
        : -1.0;
    obj["last_audio_age_ms"] = s.last_audio_ns > 0 && now_ns >= s.last_audio_ns
        ? static_cast<double>((now_ns - s.last_audio_ns) / 1000000ULL)
        : -1.0;
    const bool ffmpeg_running =
        !completed && s.ffmpeg && s.ffmpeg->running();
    obj["ffmpeg_running"] = ffmpeg_running;
    obj["ffmpeg_error"] = s.ffmpeg_error;
    obj["disk_full"] = s.disk_full;
    obj["ffmpeg_exit_code"] = s.ffmpeg_exit_code;
    obj["ffmpeg_exit_status"] = s.ffmpeg_exit_status;
    obj["ffmpeg_output_tail"] = s.ffmpeg_output_tail;
    obj["requested_video_encoder"] =
        QString::fromStdString(s.requested_video_encoder);
    obj["video_encoder"] = QString::fromStdString(s.video_encoder);
    obj["encoder_fallback"] = s.encoder_fallback;
    const uint64_t health_now_ns = os_gettime_ns();
    const bool dropping_recently = s.last_drop_ns > 0 &&
        health_now_ns >= s.last_drop_ns &&
        health_now_ns - s.last_drop_ns < 5000000000ULL;
    QString session_health = QStringLiteral("recording");
    if (s.disk_full) {
        session_health = QStringLiteral("disk_full");
    } else if (completed) {
        session_health = QStringLiteral("completed");
    } else if (!s.ffmpeg_error.isEmpty()) {
        session_health = QStringLiteral("encoder_error");
    } else if (!ffmpeg_running) {
        session_health = QStringLiteral("encoder_stopped");
    } else if (dropping_recently) {
        session_health = QStringLiteral("encoder_behind");
    } else if (s.video_frames == 0) {
        session_health = QStringLiteral("waiting_for_video");
    } else if (s.audio_chunks == 0) {
        session_health = QStringLiteral("no_audio_yet");
    }
    obj["session_health"] = session_health;
    obj["video_bytes"] = QFileInfo(s.video_path).exists()
        ? static_cast<double>(QFileInfo(s.video_path).size())
        : 0.0;
    obj["audio_bytes"] = QFileInfo(s.audio_path).exists()
        ? static_cast<double>(QFileInfo(s.audio_path).size())
        : 0.0;
    obj["video_path"] = s.video_path;
    obj["audio_path"] = s.audio_path;
    return obj;
}

void ZoomIsoRecorder::refresh_ffmpeg_status_locked(Session &session)
{
    if (!session.ffmpeg)
        return;

    // The child's stdout+stderr live in its sidecar log file; the tail is
    // re-read whole rather than accumulated, so it stays available after
    // any kind of exit (the merged-pipe readAll() was empty once the
    // process was gone).
    const std::string tail =
        session.ffmpeg->log_tail(kFfmpegOutputTailChars);
    if (!tail.empty())
        session.ffmpeg_output_tail =
            QString::fromUtf8(tail.data(),
                              static_cast<int>(tail.size())).trimmed();

    if (session.ffmpeg->running())
        return;

    session.ffmpeg_exit_code = session.ffmpeg->exit_code();
    session.ffmpeg_exit_status = session.ffmpeg->crashed()
        ? QStringLiteral("crashed")
        : QStringLiteral("normal");

    if (!session.ffmpeg_error.isEmpty())
        return;

    if (session.ffmpeg->crashed()) {
        mark_ffmpeg_failure_locked(session, QStringLiteral("FFmpeg crashed."));
    } else if (session.ffmpeg->exit_code() != 0) {
        mark_ffmpeg_failure_locked(
            session,
            QString("FFmpeg exited with code %1.")
                .arg(session.ffmpeg->exit_code()));
    }
}

void ZoomIsoRecorder::mark_ffmpeg_failure_locked(Session &session,
                                                const QString &message)
{
    if (session.ffmpeg_error.isEmpty())
        session.ffmpeg_error = message;
    if (!session.ffmpeg || session.ffmpeg_error_logged)
        return;

    blog(LOG_WARNING,
         "[obs-zoom-plugin] ISO ffmpeg failure for %s: %s%s%s",
         session.source_name.c_str(),
         session.ffmpeg_error.toUtf8().constData(),
         session.ffmpeg_output_tail.isEmpty() ? "" : " output=",
         session.ffmpeg_output_tail.toUtf8().constData());
    session.ffmpeg_error_logged = true;
}

void ZoomIsoRecorder::mark_disk_full_locked(Session &session)
{
    session.disk_full = true;
    mark_ffmpeg_failure_locked(
        session,
        QStringLiteral("No space left on device - free disk space."));
}

void ZoomIsoRecorder::close_session_on_disk_full_locked(
    const std::string &source_uuid)
{
    auto it = m_sessions.find(source_uuid);
    if (it == m_sessions.end() || !it->second.disk_full)
        return;
    // Disk is full: stop this session cleanly so the partial files are
    // finalized rather than left growing against a full volume.
    close_session_locked(source_uuid);
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
    session.unresolved_since_ns = 0; // frames flowing == resolved
    refresh_ffmpeg_status_locked(session);
    if (!session.ffmpeg || !session.ffmpeg->running()) {
        // Startup-time death of a hardware encoder usually means the
        // session-budget estimate was wrong for this GPU (driver caps
        // vary). Demote this source one tier and recreate on the next
        // frame instead of surfacing a dead "Encoder error" row.
        //
        // Deliberately NOT gated on "has this uuid ever been demoted
        // before": that was the bug. iso_demote_encoder() is a CHAIN
        // (nvenc -> qsv -> amf -> libx264), but the old guard let a uuid
        // take exactly one hop for the rest of the run -- on a box with a
        // real GPU but no working QSV or AMF runtime (live 2026-08-19:
        // "Error creating a MFX session" / "DLL amfrt64.dll failed to
        // open"), a source demoted nvenc->qsv failed qsv too, could not
        // demote again, and sat at video_frames=2 permanently instead of
        // ever reaching libx264 (CPU, no hardware dependency, the one
        // tier guaranteed to work). `session.video_encoder != "libx264"`
        // is the only guard the chain needs: it is what iso_demote_encoder
        // eventually returns for every input, so re-arming this on every
        // startup failure still terminates, in at most 3 hops.
        const uint64_t now_ns = os_gettime_ns();
        if (session.video_encoder != "libx264" &&
            session.ffmpeg_started_ns != 0 &&
            now_ns - session.ffmpeg_started_ns < 10000000000ULL) {
            const std::string next =
                iso_demote_encoder(session.video_encoder, m_encoder_avail);
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] ISO encoder %s failed at startup for "
                 "%s; retrying with %s",
                 session.video_encoder.c_str(),
                 session.source_name.c_str(), next.c_str());
            const std::string uuid = session.source_uuid;
            m_encoder_demotions[uuid] = next;
            close_session_locked(uuid);
            return;
        }
        if (session.ffmpeg_error.isEmpty())
            mark_ffmpeg_failure_locked(
                session, QStringLiteral("FFmpeg is not running."));
        return;
    }

    // One contiguous I420 frame, then a single bounded queue attempt.
    // Whole-frame granularity is load-bearing: a partially written frame
    // would shift every following frame's bytes and shear the video. The
    // pipe's writer thread does the blocking writes; a stalled encoder
    // shows up here as try_queue() == false (drop), never as RAM growth or
    // a wedge (the QProcess feed stalled after ~5 frames — see
    // iso-ffmpeg-pipe.h).
    const size_t frame_bytes =
        static_cast<size_t>(width) * height * 3 / 2;
    std::vector<uint8_t> frame(frame_bytes);
    uint8_t *dst = frame.data();
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(dst, y + row * stride_y, width);
        dst += width;
    }
    for (uint32_t row = 0; row < height / 2; ++row) {
        std::memcpy(dst, u + row * stride_uv, width / 2);
        dst += width / 2;
    }
    for (uint32_t row = 0; row < height / 2; ++row) {
        std::memcpy(dst, v + row * stride_uv, width / 2);
        dst += width / 2;
    }

    if (!session.ffmpeg->try_queue(std::move(frame))) {
        ++session.video_frames_dropped;
        session.last_drop_ns = timestamp_ns;
        if (!session.backlog_reported) {
            session.backlog_reported = true;
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] ISO encoder for %s is falling behind "
                 "(queue %zu bytes) — dropping frames to protect memory "
                 "and the meeting. Hardware encoder session limit?",
                 session.source_name.c_str(),
                 session.ffmpeg->queued_bytes());
        }
        return;
    }
    if (session.backlog_reported) {
        session.backlog_reported = false;
        blog(LOG_INFO,
             "[obs-zoom-plugin] ISO encoder for %s caught up after dropping "
             "%llu frames",
             session.source_name.c_str(),
             static_cast<unsigned long long>(session.video_frames_dropped));
    }
    ++session.video_frames;
    session.last_video_ns = timestamp_ns;
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
        bool disk_full = false;
        if (!session.wav.write(pcm, byte_len, &disk_full)) {
            if (disk_full) {
                mark_disk_full_locked(session);
            } else if (session.ffmpeg_error.isEmpty()) {
                mark_ffmpeg_failure_locked(
                    session,
                    QStringLiteral("WAV write failed."));
            }
            // Finalize and close the WAV so it stays valid/playable, and
            // stop the rest of this session cleanly. Copy the key first:
            // close_session_locked erases the Session (and its source_uuid).
            const std::string source_uuid = session.source_uuid;
            close_session_locked(source_uuid);
            return;
        }
        ++session.audio_chunks;
        session.last_audio_ns = timestamp_ns;
    }
}

static std::string normalized_video_encoder(const std::string &encoder)
{
    if (encoder == "auto" || encoder == "h264_nvenc" ||
        encoder == "h264_qsv" || encoder == "h264_amf" ||
        encoder == "libx264") {
        return encoder;
    }
    return "auto";
}

// Number of distinct hardware NVENC encoders OBS itself has active right
// now (program recording, streaming, vertical canvas, replay buffer, ...).
// These share the GPU's session budget with our ISO FFmpeg children.
static int count_obs_nvenc_encoders()
{
    struct Census {
        std::set<obs_encoder_t *> seen;
    } census;
    obs_enum_outputs(
        [](void *param, obs_output_t *output) -> bool {
            auto *c = static_cast<Census *>(param);
            if (!obs_output_active(output))
                return true;
            for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; ++i) {
                obs_encoder_t *enc =
                    obs_output_get_video_encoder2(output, i);
                if (!enc)
                    continue;
                const char *id = obs_encoder_get_id(enc);
                if (id && strstr(id, "nvenc"))
                    c->seen.insert(enc);
            }
            return true;
        },
        &census);
    return static_cast<int>(census.seen.size());
}

static bool is_hardware_encoder(const std::string &encoder)
{
    return encoder == "h264_nvenc" || encoder == "h264_qsv" ||
        encoder == "h264_amf";
}

static bool ffmpeg_encoder_available(const QString &ffmpeg_path,
                                     const std::string &encoder,
                                     std::string *error)
{
    QProcess probe;
    probe.setProgram(ffmpeg_path);
    probe.setArguments({"-hide_banner", "-encoders"});
    probe.setProcessChannelMode(QProcess::MergedChannels);
    probe.start(QIODevice::ReadOnly);
    if (!probe.waitForStarted(2000)) {
        if (error) {
            *error = "FFmpeg failed to start while checking encoders: " +
                     probe.errorString().toStdString();
        }
        return false;
    }
    if (!probe.waitForFinished(5000))
        probe.kill();

    const QString encoders = QString::fromUtf8(probe.readAll());
    if (encoders.contains(QString::fromStdString(encoder)))
        return true;

    if (error) {
        *error = "FFmpeg encoder '" + encoder +
                 "' is not available in the selected ffmpeg build.";
    }
    return false;
}

static QStringList ffmpeg_video_encoder_args(const std::string &encoder)
{
    if (encoder == "libx264") {
        return {
            "-c:v", "libx264",
            "-preset", "veryfast",
            "-crf", "18",
        };
    }

    return {
        "-c:v", QString::fromStdString(encoder),
        "-b:v", "12M",
        "-maxrate", "20M",
        "-bufsize", "24M",
    };
}
