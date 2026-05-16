#include "zoom-iso-panel.h"
#include "cv-style.h"
#include "zoom-iso-recorder.h"
#include "zoom-output-manager.h"
#include "zoom-settings.h"

#include <QAbstractItemView>
#include <QCheckBox>
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
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

static QString default_iso_output_dir()
{
    const QString docs = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    const QString base = docs.isEmpty() ? QDir::homePath() : docs;
    return QDir(base).absoluteFilePath("CoreVideo ISOs");
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
    auto *browse_ffmpeg = new QPushButton("Browse", config_group);
    m_test_btn = new QPushButton("Test", config_group);
    connect(browse_ffmpeg, &QPushButton::clicked,
            this, &ZoomIsoPanel::browse_ffmpeg);
    connect(m_test_btn, &QPushButton::clicked,
            this, &ZoomIsoPanel::test_ffmpeg);
    ffmpeg_row->addWidget(new QLabel("FFmpeg:", config_group));
    ffmpeg_row->addWidget(m_ffmpeg_path, 1);
    ffmpeg_row->addWidget(browse_ffmpeg);
    ffmpeg_row->addWidget(m_test_btn);
    config_layout->addLayout(ffmpeg_row);

    m_record_program = new QCheckBox("Also start/stop OBS program recording",
                                     config_group);
    m_record_program->setChecked(settings.iso_record_program);
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
    m_error = new QLabel(config_group);
    m_error->setObjectName("errorLabel");
    m_error->setWordWrap(true);
    m_error->setVisible(false);
    config_layout->addWidget(m_status);
    config_layout->addWidget(m_error);
    layout->addWidget(config_group);

    auto *sessions_group = new QGroupBox("Active ISO Sessions", this);
    auto *sessions_layout = new QVBoxLayout(sessions_group);
    m_sessions = new QTableWidget(sessions_group);
    m_sessions->setColumnCount(7);
    m_sessions->setHorizontalHeaderLabels({
        "Source", "Participant", "Resolution", "Video Frames",
        "Audio Chunks", "Video File", "Audio File"
    });
    m_sessions->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_sessions->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_sessions->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_sessions->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_sessions->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_sessions->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_sessions->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
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
    if (ffmpeg_exists(m_ffmpeg_path->text())) {
        set_error(QString());
        QMessageBox::information(this, "FFmpeg", "FFmpeg was found.");
    } else {
        set_error("FFmpeg was not found. Use a full path or make sure ffmpeg is on PATH.");
    }
}

void ZoomIsoPanel::start_recording()
{
    persist_settings();
    set_error(QString());

    ZoomIsoRecordConfig config;
    config.output_dir = m_output_dir->text().trimmed().toStdString();
    config.ffmpeg_path = m_ffmpeg_path->text().trimmed().toStdString();
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
    const QJsonArray sessions = ZoomIsoRecorder::instance().status_json();

    m_start_btn->setEnabled(!active);
    m_stop_btn->setEnabled(active);
    m_output_dir->setEnabled(!active);
    m_ffmpeg_path->setEnabled(!active);
    m_test_btn->setEnabled(!active);
    m_record_program->setEnabled(!active);

    m_status->setText(active
        ? QString("Recording - %1 active session%2")
              .arg(sessions.size())
              .arg(sessions.size() == 1 ? "" : "s")
        : QStringLiteral("Idle"));

    m_sessions->setRowCount(sessions.size());
    for (int row = 0; row < sessions.size(); ++row) {
        const QJsonObject s = sessions.at(row).toObject();
        const QString resolution = QString("%1x%2")
            .arg(s.value("width").toInt())
            .arg(s.value("height").toInt());
        QString participant = s.value("display_name").toString();
        const int participant_id =
            static_cast<int>(s.value("resolved_participant_id").toDouble());
        if (participant.isEmpty())
            participant = participant_id > 0
                ? QString("ID %1").arg(participant_id)
                : s.value("assignment").toString();

        m_sessions->setItem(row, 0, item(s.value("source").toString()));
        m_sessions->setItem(row, 1, item(participant));
        m_sessions->setItem(row, 2, item(resolution));
        m_sessions->setItem(row, 3, item(QString::number(s.value("video_frames").toInt())));
        m_sessions->setItem(row, 4, item(QString::number(s.value("audio_chunks").toInt())));
        m_sessions->setItem(row, 5, item(s.value("video_path").toString()));
        m_sessions->setItem(row, 6, item(s.value("audio_path").toString()));
    }
}

void ZoomIsoPanel::persist_settings() const
{
    if (m_shutting_down)
        return;
    ZoomPluginSettings settings = ZoomPluginSettings::load();
    settings.iso_output_dir = m_output_dir->text().trimmed().toStdString();
    settings.iso_ffmpeg_path = m_ffmpeg_path->text().trimmed().toStdString();
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
