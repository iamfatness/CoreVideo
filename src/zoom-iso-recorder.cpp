#include "zoom-iso-recorder.h"
#include "iso-encoder-plan.h"
#include "iso-provider-policy.h"
#include "iso-feed-selection.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStringList>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <set>
#include <util/platform.h>

static constexpr qint64 kIsoMinimumFreeBytes = 2ll * 1024ll * 1024ll * 1024ll;
static constexpr qint64 kIsoWarningFreeBytes = 10ll * 1024ll * 1024ll * 1024ll;

static QString default_iso_dir()
{
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString base = docs.isEmpty() ? QDir::homePath() : docs;
    return QDir(base).absoluteFilePath("CoreVideo ISOs");
}

static QString sanitized(const std::string &value, const QString &fallback)
{
    QString out = QString::fromStdString(value).trimmed();
    if (out.isEmpty())
        out = fallback;
    for (QChar &ch : out) {
        if (!ch.isLetterOrNumber() && ch != '-' && ch != '_' && ch != '.')
            ch = '_';
    }
    while (out.contains("__"))
        out.replace("__", "_");
    return out.left(80);
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
static bool ffmpeg_encoder_available(const QString &ffmpeg_path, const std::string &encoder,
                                     std::string *error);

ZoomIsoRecorder &ZoomIsoRecorder::instance()
{
    static ZoomIsoRecorder inst;
    return inst;
}

ZoomIsoRecorder::~ZoomIsoRecorder() { stop(); }

bool ZoomIsoRecorder::start(const ZoomIsoRecordConfig &config, std::string *error)
{
    if (m_active.load() || m_stopping.load()) {
        if (error)
            *error = "ISO recording is already active or finishing.";
        return false;
    }
    ZoomIsoRecordConfig normalized = config;
    if (normalized.selected_source_uuids.empty()) {
        if (error) *error = "Select at least one feed in the ISO Recorder before starting.";
        return false;
    }
    if (normalized.output_dir.empty())
    {
        normalized.output_dir = default_iso_dir().toStdString();
    }
    const auto outputs = ZoomOutputManager::instance().outputs();
    if (!std::any_of(outputs.begin(), outputs.end(), [&](const auto &info) {
        return iso_feed_selected(normalized.selected_source_uuids, info.source_uuid,
            info.assignment != AssignmentMode::Participant || info.participant_id != 0,
            info.audience_audio || info.assignment == AssignmentMode::ScreenShare);
    })) {
        if (error) *error = "None of the selected ISO feeds is currently routed. Assign a participant first.";
        return false;
    }
    if (normalized.ffmpeg_path.empty())
        normalized.ffmpeg_path = "ffmpeg";
    const std::string requested_encoder = normalized_video_encoder(normalized.video_encoder);
    normalized.video_encoder = requested_encoder;
    const QString ffmpegProgram = QString::fromStdString(normalized.ffmpeg_path);
    const QFileInfo ffmpegInfo(ffmpegProgram);
    if ((ffmpegInfo.isRelative() && QStandardPaths::findExecutable(ffmpegProgram).isEmpty()) ||
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
             avail.nvenc, avail.qsv, avail.amf, iso_nvenc_default_session_limit(),
             count_obs_nvenc_encoders());
    } else if (!ffmpeg_encoder_available(ffmpegProgram, normalized.video_encoder, &encoder_error)) {
        if (is_hardware_encoder(normalized.video_encoder) &&
            ffmpeg_encoder_available(ffmpegProgram, "libx264", nullptr)) {
            status_warning = QString("Requested hardware encoder '%1' was not "
                                     "available in FFmpeg; falling back to CPU "
                                     "libx264 for this ISO run.")
                                 .arg(QString::fromStdString(normalized.video_encoder));
            blog(LOG_WARNING, "[obs-zoom-plugin] %s", status_warning.toUtf8().constData());
            normalized.video_encoder = "libx264";
        } else {
            if (error)
                *error = encoder_error;
            return false;
        }
    }

    QDir dir(QString::fromStdString(normalized.output_dir));
    if (!dir.exists() && !dir.mkpath(".")) {
        if (error)
            *error = "Could not create ISO recording directory.";
        return false;
    }
    const QStorageInfo storage(dir.absolutePath());
    if (storage.isValid() && storage.isReady()) {
        const qint64 available = storage.bytesAvailable();
        if (available < kIsoMinimumFreeBytes) {
            if (error) {
                *error = QString("Only %1 is free in the ISO output folder. "
                                 "Free at least %2 before starting ISO recording.")
                             .arg(bytes_text(available), bytes_text(kIsoMinimumFreeBytes))
                             .toStdString();
            }
            return false;
        }
        if (available < kIsoWarningFreeBytes) {
            blog(
                LOG_WARNING,
                "[obs-zoom-plugin] ISO recording starting with low disk space: available=%s dir=%s",
                bytes_text(available).toUtf8().constData(), normalized.output_dir.c_str());
        }
    } else {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] ISO recording could not validate disk space for dir=%s",
             normalized.output_dir.c_str());
    }

    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_sessions.clear();
        m_config = normalized;
        m_requested_video_encoder = requested_encoder;
        m_encoder_avail = avail;
        m_nvenc_session_limit = iso_nvenc_default_session_limit();
        m_status_warning = status_warning;
        m_started_program_recording = false;
        m_completed_sessions.clear();
        m_epoch_ns = os_gettime_ns();
        m_active.store(true, std::memory_order_release);
        // Arm selected feeds only. A writer is opened on their first actual
        // media callback, never merely because an OBS source exists.
    }

    if (normalized.record_program && !obs_frontend_recording_active()) {
        obs_frontend_recording_start();
        std::lock_guard<std::mutex> lock(m_mtx);
        m_started_program_recording = true;
    }

    blog(LOG_INFO, "[obs-zoom-plugin] ISO recording started: dir=%s ffmpeg=%s encoder=%s",
         normalized.output_dir.c_str(), normalized.ffmpeg_path.c_str(),
         normalized.video_encoder.c_str());
    return true;
}

void ZoomIsoRecorder::stop()
{
    if (m_stopping.exchange(true))
        return;
    if (!m_active.exchange(false)) {
        m_stopping = false;
        return;
    }
    const uint64_t end_ns = os_gettime_ns();
    std::unordered_map<uint32_t, Session> sessions;
    bool stop_program = false;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        sessions.swap(m_sessions);
        stop_program = m_started_program_recording && obs_frontend_recording_active();
        m_started_program_recording = false;
    }
    if (stop_program)
        obs_frontend_recording_stop();
    for (auto &entry : sessions)
        entry.second.writer->close(end_ns);
    QElapsedTimer deadline;
    deadline.start();
    for (auto &entry : sessions) {
        auto &session = entry.second;
        session.writer->wait_finished(int(std::max<qint64>(0, 15000 - deadline.elapsed())));
        QJsonObject completed = session_status_locked(session, true, end_ns);
        if (!session.writer->status().done) {
            completed["ffmpeg_error"] = "ISO finalization exceeded the shared shutdown deadline";
            completed["session_health"] = "encoder_error";
        }
        // Destruction cancels any hung worker/process outside the recorder lock.
        session.writer.reset();
        std::lock_guard<std::mutex> lock(m_mtx);
        m_completed_sessions.push_back(completed);
    }
    m_stopping = false;
    blog(LOG_INFO,
         "[obs-zoom-plugin] ISO recording stopped (one muxed 1080p file per participant)");
}

bool ZoomIsoRecorder::should_record(const ZoomOutputInfo &info, uint32_t participant) const
{
    return m_active.load() && participant && iso_feed_selected(
        m_config.selected_source_uuids, info.source_uuid,
        info.assignment != AssignmentMode::Participant || info.participant_id != 0,
        info.audience_audio || info.assignment == AssignmentMode::ScreenShare);
}

ZoomIsoRecorder::Session &ZoomIsoRecorder::ensure_session_locked(const ZoomOutputInfo &info,
                                                                 uint32_t participant, uint64_t ns)
{
    auto existing = m_sessions.find(participant);
    if (existing != m_sessions.end())
        return existing->second;
    Session session;
    session.info = info;
    session.participant = participant;
    // Known participants begin at Record. Late arrivals have an explicit offset
    // rather than generating minutes of blank frames in a catch-up burst.
    session.epoch_ns = ns - m_epoch_ns < 500000000ULL ? m_epoch_ns : ns;
    const QString stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz");
    session.path = QDir(QString::fromStdString(m_config.output_dir))
                       .absoluteFilePath(
                           QString("%1_%2_participant_%3.mp4")
                               .arg(stamp, sanitized(info.display_name.empty() ? info.source_name
                                                                               : info.display_name,
                                                     "participant"))
                               .arg(participant));
    int in_use = 0;
    for (const auto &entry : m_sessions)
        if (entry.second.encoder == "h264_nvenc")
            ++in_use;
    const int reserve = m_config.record_program && !obs_frontend_recording_active() ? 1 : 0;
    session.encoder = iso_choose_session_encoder(
        m_config.video_encoder,
        m_nvenc_session_limit - count_obs_nvenc_encoders() - in_use - reserve, m_encoder_avail);
    IsoTrackWriter::Config config;
    config.ffmpeg = m_config.ffmpeg_path;
    config.encoder = session.encoder;
    config.path = session.path.toStdString();
    config.log_path = config.path + ".ffmpeg.log";
    config.epoch_ns = session.epoch_ns;
    config.now = [] { return os_gettime_ns(); };
    session.writer = std::make_unique<IsoTrackWriter>(std::move(config));
    blog(LOG_INFO,
         "[obs-zoom-plugin] ISO participant recording opened: participant=%u encoder=%s "
         "output=1920x1080 audio=AAC file=%s",
         participant, session.encoder.c_str(), session.path.toUtf8().constData());
    return m_sessions.emplace(participant, std::move(session)).first->second;
}

void ZoomIsoRecorder::record_video_frame(const ZoomOutputInfo &info, uint32_t participant,
                                         uint32_t w, uint32_t h, const uint8_t *y, const uint8_t *u,
                                         const uint8_t *v, uint32_t sy, uint32_t suv, uint64_t ns)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (!should_record(info, participant))
        return;
    auto &session = ensure_session_locked(info, participant, ns);
    if (iso_take_provider(session.video_owner, session.video_ns, info.source_uuid, ns))
        session.writer->video(w, h, y, u, v, sy, suv, ns);
}
void ZoomIsoRecorder::record_audio_frame(const ZoomOutputInfo &info, uint32_t participant,
                                         const uint8_t *pcm, uint32_t bytes, uint32_t rate,
                                         uint16_t channels, uint64_t ns)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (!should_record(info, participant))
        return;
    auto &session = ensure_session_locked(info, participant, ns);
    if (iso_take_provider(session.audio_owner, session.audio_ns, info.source_uuid, ns))
        session.writer->audio(pcm, bytes, rate, channels, ns);
}
void ZoomIsoRecorder::on_output_updated(const ZoomOutputInfo &info)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_outputs[info.source_uuid] = info;
}
void ZoomIsoRecorder::on_output_removed(const std::string &source)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_outputs.erase(source);
    for (auto &entry : m_sessions) {
        if (entry.second.video_owner == source)
            entry.second.video_owner.clear();
        if (entry.second.audio_owner == source)
            entry.second.audio_owner.clear();
    }
    // Keep the participant's file and clock alive until Record/Stop ends.
}
QJsonObject ZoomIsoRecorder::session_status_locked(Session &session, bool completed,
                                                   uint64_t end_ns)
{
    const auto status = session.writer->status();
    if (!status.error.empty() && !session.error_logged) {
        blog(LOG_ERROR, "[obs-zoom-plugin] ISO participant %u failed: %s", session.participant,
             status.error.c_str());
        session.error_logged = true;
    }
    const uint64_t now = end_ns ? end_ns : os_gettime_ns();
    QJsonObject obj;
    obj["source_uuid"] = QString("participant_%1").arg(session.participant);
    obj["source"] = QString::fromStdString(session.info.source_name);
    obj["display_name"] = QString::fromStdString(
        session.info.display_name.empty() ? session.info.source_name : session.info.display_name);
    obj["resolved_participant_id"] = double(session.participant);
    obj["configured_participant_id"] = double(session.participant);
    obj["assignment"] = "participant";
    obj["width"] = 1920;
    obj["height"] = 1080;
    obj["input_width"] = int(status.input_width);
    obj["input_height"] = int(status.input_height);
    obj["start_offset_ms"] = double((session.epoch_ns - m_epoch_ns) / 1000000);
    obj["elapsed_ms"] = double((now - session.epoch_ns) / 1000000);
    obj["last_video_age_ms"] = session.video_ns ? double((now - session.video_ns) / 1000000) : -1;
    obj["last_audio_age_ms"] = session.audio_ns ? double((now - session.audio_ns) / 1000000) : -1;
    obj["video_frames"] = double(status.frames);
    obj["written_frames"] = double(status.written_frames);
    obj["audio_chunks"] = double(status.audio_packets);
    obj["coalesced_video_frames"] = double(status.coalesced_video);
    obj["late_audio_frames"] = double(status.late_audio_frames);
    obj["video_frames_dropped"] = 0;
    obj["queued_frames"] = double(status.queued_frames);
    obj["queue_duration_ms"] = double(status.queued_frames) * 1000 / 30;
    obj["peak_queued_frames"] = double(status.peak_frames);
    obj["startup_buffering"] = status.startup;
    obj["media_stopped"] = !status.error.empty();
    obj["ffmpeg_error"] = QString::fromStdString(status.error);
    obj["ffmpeg_running"] = !status.done;
    obj["completed"] = completed;
    obj["video_encoder"] = QString::fromStdString(session.encoder);
    obj["requested_video_encoder"] = QString::fromStdString(m_requested_video_encoder);
    obj["encoder_fallback"] =
        m_requested_video_encoder != "auto" && session.encoder != m_requested_video_encoder;
    obj["session_health"] = !status.error.empty() ? "encoder_error"
                            : completed           ? "completed"
                                                  : "recording";
    obj["video_path"] = session.path;
    obj["audio_path"] = "";
    obj["audio_muxed"] = true;
    obj["video_bytes"] = double(QFileInfo(session.path).size());
    obj["audio_bytes"] = 0;
    return obj;
}
QJsonArray ZoomIsoRecorder::status_json()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    QJsonArray result;
    for (auto &entry : m_sessions)
        result.append(session_status_locked(entry.second, false));
    for (const auto &entry : m_completed_sessions)
        result.append(entry);
    return result;
}
QJsonObject ZoomIsoRecorder::status_overview()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    QJsonObject obj;
    obj["active"] = m_active.load();
    obj["finishing"] = m_stopping.load();
    QJsonArray selected_sources;
    for (const auto &uuid : m_config.selected_source_uuids)
        selected_sources.append(QString::fromStdString(uuid));
    obj["selected_source_uuids"] = selected_sources;
    obj["output_dir"] = QString::fromStdString(m_config.output_dir);
    obj["session_count"] = int(m_sessions.size());
    obj["completed_session_count"] = int(m_completed_sessions.size());
    QString warning = m_status_warning;
    for (const auto &entry : m_sessions)
        if (!entry.second.writer->status().error.empty()) {
            warning = "One or more participant recordings failed. Inspect the track errors.";
            break;
        }
    for (const auto &entry : m_completed_sessions)
        if (!entry.value("ffmpeg_error").toString().isEmpty()) {
            warning = "One or more participant recordings failed. Inspect the track errors.";
            break;
        }
    obj["warning"] = warning;
    obj["record_program"] = m_config.record_program;
    obj["video_encoder"] = QString::fromStdString(m_config.video_encoder);
    obj["requested_video_encoder"] = QString::fromStdString(m_requested_video_encoder);
    obj["ffmpeg_path"] = QString::fromStdString(m_config.ffmpeg_path);
    const QStorageInfo storage(QString::fromStdString(m_config.output_dir));
    obj["disk_available_bytes"] = double(storage.bytesAvailable());
    obj["disk_warning"] = storage.bytesAvailable() < kIsoWarningFreeBytes;
    return obj;
}

static std::string normalized_video_encoder(const std::string &encoder)
{
    if (encoder == "auto" || encoder == "h264_nvenc" || encoder == "h264_qsv" ||
        encoder == "h264_amf" || encoder == "libx264") {
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
                obs_encoder_t *enc = obs_output_get_video_encoder2(output, i);
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
    return encoder == "h264_nvenc" || encoder == "h264_qsv" || encoder == "h264_amf";
}

static bool ffmpeg_encoder_available(const QString &ffmpeg_path, const std::string &encoder,
                                     std::string *error)
{
    QProcess probe;
    probe.setProgram(ffmpeg_path);
    probe.setArguments({"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                        "color=size=1920x1080:rate=30", "-frames:v", "1", "-an", "-c:v",
                        QString::fromStdString(encoder), "-f", "null", "-"});
    probe.setProcessChannelMode(QProcess::MergedChannels);
    probe.start(QIODevice::ReadOnly);
    if (!probe.waitForStarted(2000)) {
        if (error) {
            *error = "FFmpeg failed to start while checking encoders: " +
                     probe.errorString().toStdString();
        }
        return false;
    }
    if (!probe.waitForFinished(10000)) {
        probe.kill();
        probe.waitForFinished(2000);
        if (error)
            *error = "Encoder startup probe timed out: " + encoder;
        return false;
    }

    if (probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0)
        return true;

    if (error) {
        *error = "FFmpeg encoder '" + encoder + "' is not available in the selected ffmpeg build.";
    }
    return false;
}
