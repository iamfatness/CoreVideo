#include "zoom-iso-panel.h"
#include "cv-style.h"
#include "zoom-iso-recorder.h"
#include "zoom-output-manager.h"
#include "zoom-settings.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

static constexpr qint64 kIsoMinimumFreeBytes = 2ll * 1024ll * 1024ll * 1024ll;
static constexpr qint64 kIsoWarningFreeBytes = 10ll * 1024ll * 1024ll * 1024ll;

static QString default_iso_output_dir()
{
    const QString docs = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    const QString base = docs.isEmpty() ? QDir::homePath() : docs;
    return QDir(base).absoluteFilePath("CoreVideo ISOs");
}

static QString effective_output_dir(const QString &text)
{
    const QString trimmed = text.trimmed();
    return trimmed.isEmpty() ? default_iso_output_dir() : trimmed;
}

static bool ffmpeg_exists(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty())
        return false;
    const QFileInfo info(trimmed);
    if (info.isRelative())
        return !QStandardPaths::findExecutable(trimmed).isEmpty();
    return info.exists() && info.isFile();
}

static QTableWidgetItem *item(const QString &text)
{
    auto *cell = new QTableWidgetItem(text);
    cell->setFlags(cell->flags() & ~Qt::ItemIsEditable);
    return cell;
}

static QString session_table_key(const QJsonObject &session)
{
    const QString path = session.value("video_path").toString();
    if (!path.isEmpty())
        return QString("%1:%2").arg(
            session.value("completed").toBool() ? QStringLiteral("done")
                                                : QStringLiteral("live"),
            path);

    return QString("%1:%2:%3")
        .arg(session.value("completed").toBool() ? QStringLiteral("done")
                                                 : QStringLiteral("live"),
             session.value("source_uuid").toString())
        .arg(static_cast<int>(
            session.value("resolved_participant_id").toDouble()));
}

struct SessionTableViewState {
    int vertical_scroll = 0;
    int horizontal_scroll = 0;
    QString current_key;
    QSet<QString> selected_keys;
};

static SessionTableViewState capture_session_table_view_state(
    const QTableWidget *table)
{
    SessionTableViewState state;
    if (!table)
        return state;

    if (const QScrollBar *bar = table->verticalScrollBar())
        state.vertical_scroll = bar->value();
    if (const QScrollBar *bar = table->horizontalScrollBar())
        state.horizontal_scroll = bar->value();

    if (const QTableWidgetItem *current = table->item(table->currentRow(), 0))
        state.current_key = current->data(Qt::UserRole).toString();

    const auto selected_ranges = table->selectedRanges();
    for (const QTableWidgetSelectionRange &range : selected_ranges) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
            if (const QTableWidgetItem *row_item = table->item(row, 0)) {
                const QString key = row_item->data(Qt::UserRole).toString();
                if (!key.isEmpty())
                    state.selected_keys.insert(key);
            }
        }
    }

    return state;
}

static void restore_session_table_view_state(
    QTableWidget *table, const SessionTableViewState &state)
{
    if (!table)
        return;

    table->clearSelection();
    int current_row = -1;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QTableWidgetItem *row_item = table->item(row, 0);
        if (!row_item)
            continue;

        const QString key = row_item->data(Qt::UserRole).toString();
        if (!state.current_key.isEmpty() && key == state.current_key)
            current_row = row;
        if (state.selected_keys.contains(key))
            table->selectRow(row);
    }

    if (current_row >= 0)
        table->setCurrentCell(current_row, table->currentColumn() >= 0
            ? table->currentColumn()
            : 0);

    if (QScrollBar *bar = table->verticalScrollBar())
        bar->setValue(state.vertical_scroll);
    if (QScrollBar *bar = table->horizontalScrollBar())
        bar->setValue(state.horizontal_scroll);
}

static QString bytes_text(qint64 bytes)
{
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0)
        return QString("%1 GB").arg(gb, 0, 'f', 1);
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return QString("%1 MB").arg(mb, 0, 'f', 1);
}

static QString duration_text_from_seconds(qint64 seconds)
{
    if (seconds < 0)
        return QStringLiteral("unknown");
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    if (hours > 0)
        return QString("%1h %2m").arg(hours).arg(minutes);
    return QString("%1m").arg(minutes);
}

static QString age_text(double age_ms)
{
    if (age_ms < 0.0)
        return QStringLiteral("never");
    if (age_ms < 1000.0)
        return QString("%1 ms").arg(static_cast<int>(age_ms));
    return QString("%1.%2s")
        .arg(static_cast<int>(age_ms) / 1000)
        .arg((static_cast<int>(age_ms) / 100) % 10);
}

static QStorageInfo storage_for_output_dir(const QString &path)
{
    QFileInfo info(effective_output_dir(path));
    while (!info.exists() && !info.absolutePath().isEmpty() &&
           info.absolutePath() != info.absoluteFilePath()) {
        info = QFileInfo(info.absolutePath());
    }
    return QStorageInfo(info.absoluteFilePath());
}

static int encoder_index(QComboBox *combo, const std::string &encoder)
{
    if (!combo)
        return 0;
    const int idx = combo->findData(QString::fromStdString(encoder));
    return idx >= 0 ? idx : 0;
}

static bool ffmpeg_has_encoder(const QString &path, const QString &encoder)
{
    QProcess probe;
    probe.setProgram(path.trimmed());
    probe.setArguments({"-hide_banner", "-encoders"});
    probe.setProcessChannelMode(QProcess::MergedChannels);
    probe.start(QIODevice::ReadOnly);
    if (!probe.waitForStarted(2000))
        return false;
    if (!probe.waitForFinished(5000))
        probe.kill();
    return QString::fromUtf8(probe.readAll()).contains(encoder);
}

static QString encoder_guidance_text(const QString &encoder)
{
    if (encoder == QStringLiteral("libx264")) {
        return QStringLiteral(
            "CPU x264 avoids GPU encoder session limits but can be heavy with 8 ISO feeds plus program streaming. Use veryfast/fast CPU presets and monitor OBS CPU load.");
    }
    if (encoder == QStringLiteral("h264_nvenc")) {
        return QStringLiteral(
            "NVENC lowers CPU load, but each ISO feed uses an encoder session. 8 ISO feeds plus OBS program output may exceed GeForce session limits; use CPU for one path if sessions fail.");
    }
    if (encoder == QStringLiteral("h264_qsv")) {
        return QStringLiteral(
            "Intel Quick Sync lowers CPU load and is useful for ISO feeds when available. Watch GPU media-engine usage and fall back to CPU x264 if FFmpeg reports encoder failures.");
    }
    if (encoder == QStringLiteral("h264_amf")) {
        return QStringLiteral(
            "AMD AMF lowers CPU load when available. Watch GPU encoder utilization and fall back to CPU x264 if FFmpeg reports encoder failures.");
    }
    return QStringLiteral("Unknown encoder. Test FFmpeg before recording.");
}

static bool is_hardware_encoder(const QString &encoder)
{
    return encoder == QStringLiteral("h264_nvenc") ||
        encoder == QStringLiteral("h264_qsv") ||
        encoder == QStringLiteral("h264_amf");
}

static bool is_iso_eligible_output(const ZoomOutputInfo &output)
{
    if (output.assignment == AssignmentMode::ScreenShare)
        return false;
    if (output.assignment == AssignmentMode::ActiveSpeaker ||
        output.assignment == AssignmentMode::SpotlightIndex)
        return true;
    return output.participant_id != 0;
}

static qint64 estimated_output_bytes_per_second(const ZoomOutputInfo &output)
{
    const int requested_height = video_resolution_height(output.video_resolution);
    const uint32_t observed_height = output.observed_height;
    const int height = static_cast<int>(observed_height > 0
        ? observed_height
        : static_cast<uint32_t>(requested_height));
    if (height >= 1000)
        return 1100ll * 1024ll;
    if (height >= 700)
        return 700ll * 1024ll;
    return 350ll * 1024ll;
}

static qint64 estimated_iso_bytes_per_second(
    const std::vector<ZoomOutputInfo> &outputs, bool record_program)
{
    qint64 bytes_per_second = 0;
    // Accumulation is conditional (filtered by is_iso_eligible_output), so
    // std::accumulate would need an equivalent-or-more-convoluted lambda
    // here with no readability gain over the plain loop.
    for (const auto &output : outputs) {
        if (is_iso_eligible_output(output))
            // cppcheck-suppress useStlAlgorithm
            bytes_per_second += estimated_output_bytes_per_second(output);
    }
    if (record_program)
        bytes_per_second += 1100ll * 1024ll;
    return bytes_per_second;
}

ZoomIsoPanel::ZoomIsoPanel(QWidget *parent)
    : QWidget(parent)
{
    setMinimumWidth(560);

    const ZoomPluginSettings settings = ZoomPluginSettings::load();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *config_group = new QGroupBox("ISO Recording", this);
    auto *config_layout = new QVBoxLayout(config_group);
    config_layout->setSpacing(6);

    auto *folder_row = new QHBoxLayout;
    folder_row->setSpacing(6);
    m_output_dir = new QLineEdit(config_group);
    m_output_dir->setPlaceholderText(default_iso_output_dir());
    m_output_dir->setText(settings.iso_output_dir.empty()
        ? default_iso_output_dir()
        : QString::fromStdString(settings.iso_output_dir));
    auto *browse_folder = new QPushButton("Browse", config_group);
    connect(browse_folder, &QPushButton::clicked,
            this, &ZoomIsoPanel::browse_output_dir);
    folder_row->addWidget(new QLabel("Output folder:", config_group));
    folder_row->addWidget(m_output_dir, 1);
    folder_row->addWidget(browse_folder);
    config_layout->addLayout(folder_row);

    auto *ffmpeg_row = new QHBoxLayout;
    ffmpeg_row->setSpacing(6);
    m_ffmpeg_path = new QLineEdit(config_group);
    m_ffmpeg_path->setText(settings.iso_ffmpeg_path.empty()
        ? QStringLiteral("ffmpeg")
        : QString::fromStdString(settings.iso_ffmpeg_path));
    auto *browse_ffmpeg_button = new QPushButton("Browse", config_group);
    m_test_btn = new QPushButton("Test", config_group);
    connect(browse_ffmpeg_button, &QPushButton::clicked,
            this, &ZoomIsoPanel::browse_ffmpeg);
    connect(m_test_btn, &QPushButton::clicked,
            this, &ZoomIsoPanel::test_ffmpeg);
    ffmpeg_row->addWidget(new QLabel("FFmpeg:", config_group));
    ffmpeg_row->addWidget(m_ffmpeg_path, 1);
    ffmpeg_row->addWidget(browse_ffmpeg_button);
    ffmpeg_row->addWidget(m_test_btn);
    config_layout->addLayout(ffmpeg_row);

    auto *encoder_row = new QHBoxLayout;
    encoder_row->setSpacing(6);
    m_video_encoder = new QComboBox(config_group);
    m_video_encoder->addItem("CPU - x264 (safe fallback)", "libx264");
    m_video_encoder->addItem("NVIDIA NVENC - H.264", "h264_nvenc");
    m_video_encoder->addItem("Intel Quick Sync - H.264", "h264_qsv");
    m_video_encoder->addItem("AMD AMF - H.264", "h264_amf");
    m_video_encoder->setCurrentIndex(
        encoder_index(m_video_encoder, settings.iso_video_encoder));
    m_video_encoder->setToolTip(
        "Hardware encoders reduce CPU load but consume GPU encoder sessions. "
        "Use CPU x264 if the selected FFmpeg build or GPU does not support the hardware encoder.");
    connect(m_video_encoder, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this]() {
                refresh_encoder_guidance();
                refresh_capacity_guidance();
                persist_settings();
            });
    encoder_row->addWidget(new QLabel("Video encoder:", config_group));
    encoder_row->addWidget(m_video_encoder, 1);
    config_layout->addLayout(encoder_row);

    m_record_program = new QCheckBox("Also start/stop OBS program recording",
                                     config_group);
    m_record_program->setChecked(settings.iso_record_program);
    connect(m_record_program, &QCheckBox::toggled, this, [this]() {
        refresh_capacity_guidance();
        persist_settings();
    });
    config_layout->addWidget(m_record_program);

    auto *button_row = new QHBoxLayout;
    button_row->setSpacing(6);
    m_start_btn = new QPushButton("Start ISO Recording", config_group);
    m_stop_btn = new QPushButton("Stop ISO Recording", config_group);
    m_open_folder_btn = new QPushButton("Open Folder", config_group);
    m_start_btn->setProperty("role", "primary");
    m_stop_btn->setProperty("role", "danger");
    connect(m_start_btn, &QPushButton::clicked,
            this, &ZoomIsoPanel::start_recording);
    connect(m_stop_btn, &QPushButton::clicked,
            this, &ZoomIsoPanel::stop_recording);
    connect(m_open_folder_btn, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_output_dir->text()));
    });
    button_row->addWidget(m_start_btn);
    button_row->addWidget(m_stop_btn);
    button_row->addWidget(m_open_folder_btn);
    config_layout->addLayout(button_row);

    m_status = new QLabel("Idle", config_group);
    m_encoder_guidance = new QLabel(config_group);
    m_encoder_guidance->setWordWrap(true);
    m_capacity_guidance = new QLabel(config_group);
    m_capacity_guidance->setWordWrap(true);
    m_disk_status = new QLabel(config_group);
    m_error = new QLabel(config_group);
    m_error->setObjectName("errorLabel");
    m_error->setWordWrap(true);
    m_error->setVisible(false);
    config_layout->addWidget(m_status);
    config_layout->addWidget(m_encoder_guidance);
    config_layout->addWidget(m_capacity_guidance);
    config_layout->addWidget(m_disk_status);
    config_layout->addWidget(m_error);
    layout->addWidget(config_group);

    auto *sessions_group = new QGroupBox("Active ISO Sessions", this);
    auto *sessions_layout = new QVBoxLayout(sessions_group);
    m_sessions = new QTableWidget(sessions_group);
    m_sessions->setColumnCount(11);
    m_sessions->setHorizontalHeaderLabels({
        "Source", "Participant", "Status", "Encoder", "Duration", "Resolution",
        "Video Frames", "Audio Chunks", "Video Size", "Audio Size", "Files"
    });
    m_sessions->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_sessions->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_sessions->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_sessions->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_sessions->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_sessions->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_sessions->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_sessions->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
    m_sessions->horizontalHeader()->setSectionResizeMode(8, QHeaderView::ResizeToContents);
    m_sessions->horizontalHeader()->setSectionResizeMode(9, QHeaderView::ResizeToContents);
    m_sessions->horizontalHeader()->setSectionResizeMode(10, QHeaderView::Stretch);
    m_sessions->verticalHeader()->setVisible(false);
    m_sessions->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_sessions->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_sessions->setAlternatingRowColors(true);
    sessions_layout->addWidget(m_sessions);
    layout->addWidget(sessions_group, 1);

    m_refresh_timer = new QTimer(this);
    m_refresh_timer->setInterval(1000);
    connect(m_refresh_timer, &QTimer::timeout,
            this, &ZoomIsoPanel::refresh_status);
    m_refresh_timer->start();

    setStyleSheet(cv_stylesheet());
    refresh_encoder_guidance();
    refresh_capacity_guidance();
    refresh_status();
}

ZoomIsoPanel::~ZoomIsoPanel()
{
    if (!m_shutting_down)
        persist_settings();
}

void ZoomIsoPanel::prepare_shutdown()
{
    m_shutting_down = true;
    if (m_refresh_timer)
        m_refresh_timer->stop();
}

void ZoomIsoPanel::refresh_now()
{
    refresh_status();
}

void ZoomIsoPanel::browse_output_dir()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, "Select ISO output folder", m_output_dir->text());
    if (!dir.isEmpty()) {
        m_output_dir->setText(dir);
        persist_settings();
    }
}

void ZoomIsoPanel::browse_ffmpeg()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Select ffmpeg executable", QString(), "FFmpeg (ffmpeg.exe ffmpeg);;All files (*)");
    if (!path.isEmpty()) {
        m_ffmpeg_path->setText(path);
        persist_settings();
    }
}

void ZoomIsoPanel::test_ffmpeg()
{
    persist_settings();
    if (!ffmpeg_exists(m_ffmpeg_path->text())) {
        set_error("FFmpeg was not found. Use a full path or make sure ffmpeg is on PATH.");
        return;
    }

    const QString encoder = m_video_encoder->currentData().toString();
    if (!ffmpeg_has_encoder(m_ffmpeg_path->text(), encoder)) {
        set_error(QString("FFmpeg was found, but encoder '%1' is not available in this FFmpeg build.")
            .arg(encoder));
        return;
    }

    set_error(QString());
    QMessageBox::information(this, "FFmpeg",
        QString("FFmpeg was found and encoder '%1' is available.").arg(encoder));
}

void ZoomIsoPanel::start_recording()
{
    persist_settings();
    set_error(QString());

    const QStorageInfo storage = storage_for_output_dir(m_output_dir->text());
    if (storage.isValid() && storage.isReady()) {
        const qint64 available = storage.bytesAvailable();
        if (available < kIsoMinimumFreeBytes) {
            set_error(QString("Only %1 is free in the ISO output folder. "
                              "Free at least %2 before starting ISO recording.")
                          .arg(bytes_text(available),
                               bytes_text(kIsoMinimumFreeBytes)));
            return;
        }
        if (available < kIsoWarningFreeBytes) {
            const int choice = QMessageBox::warning(
                this, "Low Disk Space",
                QString("Only %1 is free in the ISO output folder. ISO files "
                        "can grow quickly. Continue recording?")
                    .arg(bytes_text(available)),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (choice != QMessageBox::Yes)
                return;
        }
        const auto outputs = ZoomOutputManager::instance().outputs();
        const qint64 estimated_bytes_per_second =
            estimated_iso_bytes_per_second(outputs, m_record_program->isChecked());
        if (estimated_bytes_per_second > 0 &&
            available / estimated_bytes_per_second < 30 * 60) {
            const int choice = QMessageBox::warning(
                this, "Limited Recording Runway",
                QString("Current assignments are estimated at %1/min with about %2 "
                        "of free-space runway. Continue recording?")
                    .arg(bytes_text(estimated_bytes_per_second * 60),
                         duration_text_from_seconds(
                             available / estimated_bytes_per_second)),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (choice != QMessageBox::Yes)
                return;
        }
    }

    const QString encoder = m_video_encoder->currentData().toString();
    const auto outputs = ZoomOutputManager::instance().outputs();
    const int eligible_outputs = static_cast<int>(std::count_if(
        outputs.begin(), outputs.end(), is_iso_eligible_output));
    const int encode_paths = eligible_outputs + (m_record_program->isChecked() ? 1 : 0);
    if (is_hardware_encoder(encoder) && encode_paths > 3) {
        const int choice = QMessageBox::warning(
            this, "Encoder Capacity",
            QString("This will use up to %1 hardware H.264 encode path(s): %2 ISO feed(s)%3. "
                    "Some GPUs limit concurrent encoder sessions. Continue recording?")
                .arg(encode_paths)
                .arg(eligible_outputs)
                .arg(m_record_program->isChecked()
                    ? QStringLiteral(" plus OBS program recording")
                    : QString()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice != QMessageBox::Yes)
            return;
    }

    ZoomIsoRecordConfig config;
    config.output_dir = m_output_dir->text().trimmed().toStdString();
    config.ffmpeg_path = m_ffmpeg_path->text().trimmed().toStdString();
    config.video_encoder =
        m_video_encoder->currentData().toString().toStdString();
    config.record_program = m_record_program->isChecked();

    std::string error;
    if (!ZoomIsoRecorder::instance().start(config, &error)) {
        set_error(QString::fromStdString(error));
        refresh_status();
        return;
    }

    for (const auto &output : ZoomOutputManager::instance().outputs())
        ZoomIsoRecorder::instance().on_output_updated(output);
    refresh_status();
}

void ZoomIsoPanel::stop_recording()
{
    ZoomIsoRecorder::instance().stop();
    refresh_status();
}

void ZoomIsoPanel::refresh_status()
{
    const bool active = ZoomIsoRecorder::instance().active();
    const QJsonObject recorder = ZoomIsoRecorder::instance().status_overview();
    const QJsonArray sessions = ZoomIsoRecorder::instance().status_json();

    m_start_btn->setEnabled(!active);
    m_stop_btn->setEnabled(active);
    m_output_dir->setEnabled(!active);
    m_ffmpeg_path->setEnabled(!active);
    m_video_encoder->setEnabled(!active);
    m_test_btn->setEnabled(!active);
    m_record_program->setEnabled(!active);

    QString status_text = active
        ? QString("Recording - %1 active session%2")
              .arg(recorder.value("session_count").toInt())
              .arg(recorder.value("session_count").toInt() == 1 ? "" : "s")
        : QStringLiteral("Idle");
    const int completed_count = recorder.value("completed_session_count").toInt();
    if (completed_count > 0) {
        status_text += QString(" - %1 completed session%2 retained")
            .arg(completed_count)
            .arg(completed_count == 1 ? "" : "s");
    }
    const QString recorder_warning = recorder.value("warning").toString();
    if (!recorder_warning.isEmpty())
        status_text += QString(" - %1").arg(recorder_warning);
    m_status->setText(status_text);
    refresh_capacity_guidance();

    const QStorageInfo storage = storage_for_output_dir(m_output_dir->text());
    if (storage.isValid() && storage.isReady()) {
        m_disk_status->setText(QString("Free space: %1")
            .arg(bytes_text(storage.bytesAvailable())));
        m_disk_status->setStyleSheet(storage.bytesAvailable() < kIsoWarningFreeBytes
            ? "color: #f0b429; font-weight: 700;"
            : QString());
    } else {
        m_disk_status->setText(QStringLiteral("Free space: unavailable"));
        m_disk_status->setStyleSheet("color: #f0b429; font-weight: 700;");
    }

    const SessionTableViewState table_state =
        capture_session_table_view_state(m_sessions);
    const QSignalBlocker table_signal_blocker(m_sessions);
    m_sessions->setUpdatesEnabled(false);
    m_sessions->setRowCount(sessions.size());
    for (int row = 0; row < sessions.size(); ++row) {
        const QJsonObject s = sessions.at(row).toObject();
        const QString row_key = session_table_key(s);
        const QString resolution = QString("%1x%2")
            .arg(s.value("width").toInt())
            .arg(s.value("height").toInt());
        const int elapsed_ms = static_cast<int>(s.value("elapsed_ms").toDouble());
        const QString duration = QString("%1:%2")
            .arg(elapsed_ms / 60000)
            .arg((elapsed_ms / 1000) % 60, 2, 10, QLatin1Char('0'));
        const bool ffmpeg_running = s.value("ffmpeg_running").toBool();
        const bool completed = s.value("completed").toBool();
        const QString ffmpeg_error = s.value("ffmpeg_error").toString();
        const QString ffmpeg_output = s.value("ffmpeg_output_tail").toString();
        const QString session_health = s.value("session_health").toString();
        const int video_frames = s.value("video_frames").toInt();
        const double frames_dropped =
            s.value("video_frames_dropped").toDouble(0.0);
        const int audio_chunks = s.value("audio_chunks").toInt();
        const double last_video_age_ms =
            s.value("last_video_age_ms").toDouble(-1.0);
        const double last_audio_age_ms =
            s.value("last_audio_age_ms").toDouble(-1.0);
        QString status = completed
            ? QStringLiteral("Completed")
            : (ffmpeg_running ? QStringLiteral("Recording") : QStringLiteral("Encoder stopped"));
        if (!ffmpeg_error.isEmpty())
            status = QStringLiteral("Encoder error");
        else if (session_health == QLatin1String("encoder_behind"))
            status = QString("Encoder falling behind (%1 dropped)")
                .arg(static_cast<qint64>(frames_dropped));
        else if (video_frames == 0)
            status = QString("Waiting for video (%1)")
                .arg(age_text(static_cast<double>(elapsed_ms)));
        else if (!completed && audio_chunks == 0)
            status += QString(" / no audio (%1)")
                .arg(age_text(static_cast<double>(elapsed_ms)));
        if (frames_dropped > 0 &&
            session_health != QLatin1String("encoder_behind"))
            status += QString(" (%1 dropped)")
                .arg(static_cast<qint64>(frames_dropped));
        QString participant = s.value("display_name").toString();
        const int participant_id =
            static_cast<int>(s.value("resolved_participant_id").toDouble());
        if (participant.isEmpty())
            participant = participant_id > 0
                ? QString("ID %1").arg(participant_id)
                : s.value("assignment").toString();

        auto *source_item = item(s.value("source").toString());
        source_item->setData(Qt::UserRole, row_key);
        m_sessions->setItem(row, 0, source_item);
        m_sessions->setItem(row, 1, item(participant));
        auto *status_item = item(status);
        QString tooltip = QString("Health: %1\nLast video: %2\nLast audio: %3")
            .arg(session_health.isEmpty()
                    ? QStringLiteral("unknown")
                    : session_health,
                 age_text(last_video_age_ms),
                 age_text(last_audio_age_ms));
        if (!ffmpeg_error.isEmpty()) {
            tooltip += QString("\n\nFFmpeg error:\n%1").arg(ffmpeg_error);
            if (!ffmpeg_output.isEmpty())
                tooltip += QString("\n\nFFmpeg output:\n%1").arg(ffmpeg_output);
        } else if (!ffmpeg_output.isEmpty()) {
            tooltip += QString("\n\nFFmpeg output:\n%1").arg(ffmpeg_output);
        }
        status_item->setToolTip(tooltip);
        if (completed)
            status_item->setForeground(QColor("#66d989"));
        else if (!ffmpeg_running || video_frames == 0)
            status_item->setForeground(QColor("#f0b429"));
        m_sessions->setItem(row, 2, status_item);
        QString encoder_text = s.value("video_encoder").toString();
        if (s.value("encoder_fallback").toBool()) {
            encoder_text = QString("%1 (fallback from %2)")
                .arg(encoder_text, s.value("requested_video_encoder").toString());
        }
        m_sessions->setItem(row, 3, item(encoder_text));
        m_sessions->setItem(row, 4, item(duration));
        m_sessions->setItem(row, 5, item(resolution));
        m_sessions->setItem(row, 6, item(QString::number(video_frames)));
        m_sessions->setItem(row, 7, item(QString::number(audio_chunks)));
        m_sessions->setItem(row, 8, item(bytes_text(
            static_cast<qint64>(s.value("video_bytes").toDouble()))));
        m_sessions->setItem(row, 9, item(bytes_text(
            static_cast<qint64>(s.value("audio_bytes").toDouble()))));
        auto *files = item(QString("%1\n%2")
            .arg(s.value("video_path").toString(),
                 s.value("audio_path").toString()));
        files->setToolTip(files->text());
        m_sessions->setItem(row, 10, files);
    }
    restore_session_table_view_state(m_sessions, table_state);
    m_sessions->setUpdatesEnabled(true);
}

void ZoomIsoPanel::refresh_encoder_guidance()
{
    if (!m_encoder_guidance || !m_video_encoder)
        return;
    const QString encoder = m_video_encoder->currentData().toString();
    m_encoder_guidance->setText(encoder_guidance_text(encoder));
    m_encoder_guidance->setStyleSheet(
        encoder == QStringLiteral("libx264")
            ? "color: #c0c0d8;"
            : "color: #f0b429; font-weight: 700;");
}

void ZoomIsoPanel::refresh_capacity_guidance()
{
    if (!m_capacity_guidance || !m_video_encoder || !m_record_program)
        return;

    const QString encoder = m_video_encoder->currentData().toString();
    const auto outputs = ZoomOutputManager::instance().outputs();
    const int eligible_outputs = static_cast<int>(std::count_if(
        outputs.begin(), outputs.end(), is_iso_eligible_output));
    const int encode_paths = eligible_outputs + (m_record_program->isChecked() ? 1 : 0);
    const qint64 estimated_bytes_per_second =
        estimated_iso_bytes_per_second(outputs, m_record_program->isChecked());
    const QStorageInfo storage = storage_for_output_dir(m_output_dir->text());
    const qint64 available = storage.isValid() && storage.isReady()
        ? storage.bytesAvailable()
        : -1;
    const qint64 runway_seconds = available > 0 && estimated_bytes_per_second > 0
        ? available / estimated_bytes_per_second
        : -1;

    QString text = QString("Estimated encode load: %1 ISO feed%2%3 (%4 H.264 path%5).")
        .arg(eligible_outputs)
        .arg(eligible_outputs == 1 ? "" : "s")
        .arg(m_record_program->isChecked()
            ? QStringLiteral(" plus OBS program recording")
            : QString())
        .arg(encode_paths)
        .arg(encode_paths == 1 ? "" : "s");
    if (estimated_bytes_per_second > 0) {
        text += QString(" Estimated disk rate: %1/min.")
            .arg(bytes_text(estimated_bytes_per_second * 60));
    }
    if (runway_seconds >= 0) {
        text += QString(" Estimated runway: %1.")
            .arg(duration_text_from_seconds(runway_seconds));
    }

    if (is_hardware_encoder(encoder) && encode_paths > 3) {
        text += QStringLiteral(
            " Hardware encoder session pressure is likely; switch one path to CPU x264 if FFmpeg or OBS reports encoder startup failures.");
        m_capacity_guidance->setStyleSheet("color: #f0b429; font-weight: 700;");
    } else if (encoder == QStringLiteral("libx264") && eligible_outputs >= 6) {
        text += QStringLiteral(
            " CPU load may be high with 6-8 ISO feeds; monitor OBS CPU and dropped frames.");
        m_capacity_guidance->setStyleSheet("color: #f0b429; font-weight: 700;");
    } else if (runway_seconds >= 0 && runway_seconds < 30 * 60) {
        text += QStringLiteral(
            " Free space is tight for multi-feed ISO recording; clear disk space before long captures.");
        m_capacity_guidance->setStyleSheet("color: #f0b429; font-weight: 700;");
    } else {
        m_capacity_guidance->setStyleSheet("color: #c0c0d8;");
    }

    m_capacity_guidance->setText(text);
}

void ZoomIsoPanel::persist_settings() const
{
    if (m_shutting_down)
        return;
    ZoomPluginSettings settings = ZoomPluginSettings::load();
    settings.iso_output_dir = m_output_dir->text().trimmed().toStdString();
    settings.iso_ffmpeg_path = m_ffmpeg_path->text().trimmed().toStdString();
    settings.iso_video_encoder =
        m_video_encoder->currentData().toString().toStdString();
    settings.iso_record_program = m_record_program->isChecked();
    settings.save();
}

void ZoomIsoPanel::set_error(const QString &message)
{
    m_error->setText(message);
    m_error->setVisible(!message.isEmpty());
    if (!message.isEmpty())
        QMessageBox::warning(this, "ISO Recording", message);
}
