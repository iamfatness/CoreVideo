#include "zoom-output-dialog.h"
#include "cv-style.h"
#include "zoom-engine-client.h"
#include "zoom-output-manager.h"
#include "zoom-output-profile.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

enum OutputColumns {
    ColumnPreview = 0,
    ColumnName,
    ColumnAssignment,
    ColumnResolution,
    ColumnSignal,
    ColumnAudio,
    ColumnIsolate,
    ColumnCount
};

// Fast I420 → RGB888 conversion for preview thumbnails.
// Samples every (step) pixels to produce a scaled-down image.
static QImage i420_to_qimage_scaled(uint32_t w, uint32_t h,
    const uint8_t *y_plane, const uint8_t *u_plane, const uint8_t *v_plane,
    uint32_t stride_y, uint32_t stride_uv,
    int out_w, int out_h)
{
    QImage img(out_w, out_h, QImage::Format_RGB888);
    const float x_scale = static_cast<float>(w) / out_w;
    const float y_scale = static_cast<float>(h) / out_h;

    for (int dy = 0; dy < out_h; ++dy) {
        const uint32_t sy  = static_cast<uint32_t>(dy * y_scale);
        const uint32_t scy = sy / 2;
        uint8_t *row = img.scanLine(dy);
        for (int dx = 0; dx < out_w; ++dx) {
            const uint32_t sx  = static_cast<uint32_t>(dx * x_scale);
            const uint32_t scx = sx / 2;
            const int Y = y_plane[sy  * stride_y  + sx];
            const int U = u_plane[scy * stride_uv + scx] - 128;
            const int V = v_plane[scy * stride_uv + scx] - 128;
            row[dx * 3 + 0] = static_cast<uint8_t>(std::clamp(Y + (int)(1.402f  * V), 0, 255));
            row[dx * 3 + 1] = static_cast<uint8_t>(std::clamp(Y - (int)(0.344f  * U) - (int)(0.714f * V), 0, 255));
            row[dx * 3 + 2] = static_cast<uint8_t>(std::clamp(Y + (int)(1.772f  * U), 0, 255));
        }
    }
    return img;
}

static QString participant_label(const ParticipantInfo &p)
{
    QString label = p.display_name.empty()
        ? QString("ID %1").arg(p.user_id)
        : QString::fromStdString(p.display_name);
    if (p.is_talking) label += " *";
    if (p.has_video) label += " [video]";
    return label;
}

static QString signal_text(const ZoomOutputInfo &output)
{
    if (output.observed_width == 0 || output.observed_height == 0)
        return QStringLiteral("No signal");
    const QString prefix = output_signal_below_requested(output)
        ? QStringLiteral("! ")
        : QString();
    return QString("%1%2x%3\n%4 fps")
        .arg(prefix)
        .arg(output.observed_width)
        .arg(output.observed_height)
        .arg(output.observed_fps, 0, 'f', 1);
}

static QString signal_tooltip(const ZoomOutputInfo &output)
{
    if (output.observed_width == 0 || output.observed_height == 0)
        return QStringLiteral("No live video frame has been received.");
    QString text = QString("Requested %1x%2. Receiving %3x%4 at %5 fps.")
        .arg(video_resolution_width(output.video_resolution))
        .arg(video_resolution_height(output.video_resolution))
        .arg(output.observed_width)
        .arg(output.observed_height)
        .arg(output.observed_fps, 0, 'f', 2);
    if (output_signal_below_requested(output))
        text += QStringLiteral(" CoreVideo is using the best available Zoom feed.");
    return text;
}

ZoomOutputDialog::ZoomOutputDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Zoom Output Manager");
    setMinimumSize(1080, 720);
    resize(1180, 760);

    // ── Profile toolbar ───────────────────────────────────────────────────────
    m_profile_combo = new QComboBox(this);
    m_profile_combo->setMinimumWidth(180);
    m_profile_combo->setPlaceholderText("— select profile —");

    auto *save_btn   = new QPushButton("Save Profile",   this);
    auto *load_btn   = new QPushButton("Load Profile",   this);
    auto *delete_btn = new QPushButton("Delete Profile", this);

    connect(save_btn,   &QPushButton::clicked, this, [this]() { save_profile(); });
    connect(load_btn,   &QPushButton::clicked, this, [this]() { load_profile(); });
    connect(delete_btn, &QPushButton::clicked, this, [this]() { delete_profile(); });

    auto *profile_row = new QHBoxLayout();
    profile_row->addWidget(new QLabel("Profile:", this));
    profile_row->addWidget(m_profile_combo, 1);
    profile_row->addWidget(save_btn);
    profile_row->addWidget(load_btn);
    profile_row->addWidget(delete_btn);
    profile_row->addStretch();

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText("Filter participants");
    connect(m_filter, &QLineEdit::textChanged, this,
            [this]() { refresh_participants(); });

    m_participant_table = new QTableWidget(this);
    m_participant_table->setColumnCount(5);
    m_participant_table->setHorizontalHeaderLabels({
        "Participant", "ID", "Video", "Audio", "Talking"
    });
    m_participant_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_participant_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_participant_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_participant_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_participant_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_participant_table->verticalHeader()->setVisible(false);
    m_participant_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_participant_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_participant_table->setMinimumHeight(170);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels({
        "Preview", "Output", "Assignment", "Requested", "Signal", "Audio", "Isolated audio"
    });
    m_table->horizontalHeader()->setSectionResizeMode(ColumnPreview,    QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(ColumnName,       QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColumnAssignment, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColumnResolution, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColumnSignal,     QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColumnAudio,      QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColumnIsolate,    QHeaderView::ResizeToContents);
    m_table->setColumnWidth(ColumnPreview, 162);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setMinimumHeight(360);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply |
                                         QDialogButtonBox::Close, this);
    auto *refresh_button = buttons->addButton("Refresh", QDialogButtonBox::ActionRole);

    if (auto *apply_btn = buttons->button(QDialogButtonBox::Apply))
        apply_btn->setProperty("role", "primary");

    connect(refresh_button, &QPushButton::clicked, this, [this]() { refresh(); });
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, [this]() { apply(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(profile_row);
    layout->addWidget(new QLabel("Participants", this));
    layout->addWidget(m_filter);
    layout->addWidget(m_participant_table);
    layout->addWidget(new QLabel("Outputs", this));
    layout->addWidget(m_table);
    layout->addWidget(buttons);

    setStyleSheet(cv_stylesheet());

    QPointer<ZoomOutputDialog> self(this);
    auto alive = m_alive;
    ZoomEngineClient::instance().add_roster_callback(this, [self, alive]() {
        if (!alive->load(std::memory_order_acquire) || !self) return;
        QMetaObject::invokeMethod(self, [self, alive]() {
            if (!alive->load(std::memory_order_acquire) || !self) return;
            self->refresh();
        }, Qt::QueuedConnection);
    });

    refresh();
    refresh_profiles();
}

ZoomOutputDialog::~ZoomOutputDialog()
{
    m_alive->store(false, std::memory_order_release);
    ZoomEngineClient::instance().remove_roster_callback(this);
    ZoomOutputManager::instance().clear_all_preview_cbs();
}

void ZoomOutputDialog::refresh()
{
    refresh_participants();

    // Clear any existing preview callbacks before rebuilding rows.
    ZoomOutputManager::instance().clear_all_preview_cbs();

    const auto outputs = ZoomOutputManager::instance().outputs();
    const std::vector<ParticipantInfo> roster = ZoomEngineClient::instance().roster();

    m_table->setRowCount(static_cast<int>(outputs.size()));
    m_table->setRowHeight(0, 92); // apply uniform row height once
    for (int row = 0; row < static_cast<int>(outputs.size()); ++row) {
        m_table->setRowHeight(row, 92);
        const auto &output = outputs[row];

        // Preview thumbnail label
        auto *preview_label = new QLabel(m_table);
        preview_label->setFixedSize(160, 90);
        preview_label->setAlignment(Qt::AlignCenter);
        preview_label->setStyleSheet("background: #1a1a1a;");
        preview_label->setText("No video");
        m_table->setCellWidget(row, ColumnPreview, preview_label);

        // Register live preview callback for this source.
        // Capture preview_label via QPointer so the queued lambda is safe if
        // the table row (and label) is destroyed before the callback fires.
        auto alive = m_alive;
        const std::string src_name = output.source_name;
        QPointer<QLabel> preview_ptr(preview_label);
        ZoomOutputManager::instance().set_preview_cb(src_name,
            [preview_ptr, alive](uint32_t w, uint32_t h,
                const uint8_t *y, const uint8_t *u, const uint8_t *v,
                uint32_t stride_y, uint32_t stride_uv) {
                if (!alive->load(std::memory_order_acquire)) return;
                QImage img = i420_to_qimage_scaled(w, h, y, u, v,
                    stride_y, stride_uv, 160, 90);
                // Post to main thread; QPointer::operator bool() is safe there.
                QMetaObject::invokeMethod(qApp,
                    [preview_ptr, img, alive]() {
                        if (!alive->load(std::memory_order_acquire) || !preview_ptr) return;
                        preview_ptr->setPixmap(QPixmap::fromImage(img));
                        preview_ptr->setText({});
                    }, Qt::QueuedConnection);
            });

        auto *name_item = new QTableWidgetItem(
            output.display_name.empty()
            ? QString::fromStdString(output.source_name)
            : QString::fromStdString(output.display_name));
        name_item->setFlags(name_item->flags() & ~Qt::ItemIsEditable);
        name_item->setData(Qt::UserRole, QString::fromStdString(output.source_name));
        m_table->setItem(row, ColumnName, name_item);

        auto *assignment = new QComboBox(m_table);
        assignment->addItem("Active speaker", "active");
        assignment->addItem("None", "user:0");
        for (const auto &p : roster)
            assignment->addItem(participant_label(p), QString("user:%1").arg(p.user_id));
        const QString current_assignment = output.active_speaker
            ? "active"
            : QString("user:%1").arg(output.participant_id);
        const int assignment_index = assignment->findData(current_assignment);
        if (assignment_index >= 0) assignment->setCurrentIndex(assignment_index);
        m_table->setCellWidget(row, ColumnAssignment, assignment);

        auto *audio = new QComboBox(m_table);
        audio->addItem("Mono", static_cast<int>(AudioChannelMode::Mono));
        audio->addItem("Stereo", static_cast<int>(AudioChannelMode::Stereo));
        audio->setCurrentIndex(output.audio_mode == AudioChannelMode::Stereo ? 1 : 0);

        auto *resolution = new QComboBox(m_table);
        resolution->addItem("360p", static_cast<int>(VideoResolution::P360));
        resolution->addItem("720p", static_cast<int>(VideoResolution::P720));
        resolution->addItem("1080p", static_cast<int>(VideoResolution::P1080));
        resolution->setCurrentIndex(
            output.video_resolution == VideoResolution::P360 ? 0 :
            output.video_resolution == VideoResolution::P1080 ? 2 : 1);
        m_table->setCellWidget(row, ColumnResolution, resolution);

        auto *signal = new QLabel(m_table);
        signal->setAlignment(Qt::AlignCenter);
        signal->setText(signal_text(output));
        signal->setToolTip(signal_tooltip(output));
        if (output_signal_below_requested(output))
            signal->setStyleSheet("color: #f0b429; font-weight: 700;");
        m_table->setCellWidget(row, ColumnSignal, signal);

        m_table->setCellWidget(row, ColumnAudio, audio);

        auto *isolate = new QCheckBox(m_table);
        isolate->setChecked(output.isolate_audio);
        isolate->setToolTip("Use the assigned participant's isolated audio instead of the meeting mix.");
        m_table->setCellWidget(row, ColumnIsolate, isolate);
    }
}

void ZoomOutputDialog::refresh_participants()
{
    const QString filter = m_filter ? m_filter->text().trimmed() : QString{};
    const std::vector<ParticipantInfo> roster = ZoomEngineClient::instance().roster();

    int row = 0;
    m_participant_table->setRowCount(0);
    for (const auto &p : roster) {
        const QString name = p.display_name.empty()
            ? QString("ID %1").arg(p.user_id)
            : QString::fromStdString(p.display_name);
        const QString id = QString::number(p.user_id);
        if (!filter.isEmpty() &&
            !name.contains(filter, Qt::CaseInsensitive) &&
            !id.contains(filter)) {
            continue;
        }

        m_participant_table->insertRow(row);
        m_participant_table->setItem(row, 0, new QTableWidgetItem(name));
        m_participant_table->setItem(row, 1, new QTableWidgetItem(id));

        auto *video_item = new QTableWidgetItem(p.has_video ? "● On" : "Off");
        video_item->setForeground(p.has_video ? QColor("#22cc44") : QColor("#666666"));
        m_participant_table->setItem(row, 2, video_item);

        auto *audio_item = new QTableWidgetItem(p.is_muted ? "Muted" : "● Open");
        audio_item->setForeground(p.is_muted ? QColor("#f0a000") : QColor("#22cc44"));
        m_participant_table->setItem(row, 3, audio_item);

        auto *talking_item = new QTableWidgetItem(p.is_talking ? "●" : "");
        talking_item->setForeground(QColor("#22cc44"));
        talking_item->setTextAlignment(Qt::AlignCenter);
        m_participant_table->setItem(row, 4, talking_item);
        ++row;
    }
}

void ZoomOutputDialog::refresh_profiles()
{
    const QString current = m_profile_combo->currentText();
    m_profile_combo->clear();
    for (const auto &name : ZoomOutputProfile::list())
        m_profile_combo->addItem(QString::fromStdString(name));
    // Restore previous selection if it still exists
    const int idx = m_profile_combo->findText(current);
    if (idx >= 0) m_profile_combo->setCurrentIndex(idx);
}

void ZoomOutputDialog::save_profile()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Save Profile",
        "Profile name:", QLineEdit::Normal,
        m_profile_combo->currentText(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    // Collect current table state as ZoomOutputInfo list
    std::vector<ZoomOutputInfo> outputs;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *name_item  = m_table->item(row, ColumnName);
        auto *assignment = qobject_cast<QComboBox *>(m_table->cellWidget(row, ColumnAssignment));
        auto *resolution = qobject_cast<QComboBox *>(m_table->cellWidget(row, ColumnResolution));
        auto *audio      = qobject_cast<QComboBox *>(m_table->cellWidget(row, ColumnAudio));
        auto *isolate    = qobject_cast<QCheckBox *>(m_table->cellWidget(row, ColumnIsolate));
        if (!name_item || !assignment || !resolution || !audio || !isolate) continue;

        ZoomOutputInfo o;
        o.source_name    = name_item->data(Qt::UserRole).toString().toStdString();
        const QString ad = assignment->currentData().toString();
        o.active_speaker = (ad == "active");
        o.participant_id = ad.startsWith("user:") ? ad.mid(5).toUInt() : 0;
        o.isolate_audio  = isolate->isChecked();
        o.audio_mode     = static_cast<AudioChannelMode>(audio->currentData().toInt());
        o.video_resolution =
            static_cast<VideoResolution>(resolution->currentData().toInt());
        outputs.push_back(std::move(o));
    }

    if (ZoomOutputProfile::save(name.trimmed().toStdString(), outputs)) {
        refresh_profiles();
        m_profile_combo->setCurrentText(name.trimmed());
    } else {
        QMessageBox::warning(this, "Save Profile", "Failed to save profile.");
    }
}

void ZoomOutputDialog::load_profile()
{
    const std::string name = m_profile_combo->currentText().toStdString();
    if (name.empty()) return;

    const auto profile = ZoomOutputProfile::load(name);
    if (profile.empty()) {
        QMessageBox::warning(this, "Load Profile", "Profile is empty or could not be read.");
        return;
    }

    // Apply each entry in the profile to the matching output row
    for (const auto &o : profile) {
        for (int row = 0; row < m_table->rowCount(); ++row) {
            auto *name_item = m_table->item(row, ColumnName);
            if (!name_item) continue;
            if (name_item->data(Qt::UserRole).toString().toStdString() != o.source_name)
                continue;

            auto *assignment = qobject_cast<QComboBox *>(m_table->cellWidget(row, ColumnAssignment));
            auto *resolution = qobject_cast<QComboBox *>(m_table->cellWidget(row, ColumnResolution));
            auto *audio      = qobject_cast<QComboBox *>(m_table->cellWidget(row, ColumnAudio));
            auto *isolate    = qobject_cast<QCheckBox *>(m_table->cellWidget(row, ColumnIsolate));
            if (!assignment || !resolution || !audio || !isolate) continue;

            const QString ad = o.active_speaker
                ? "active"
                : QString("user:%1").arg(o.participant_id);
            const int idx = assignment->findData(ad);
            if (idx >= 0) assignment->setCurrentIndex(idx);
            const int res_idx = resolution->findData(
                static_cast<int>(o.video_resolution));
            if (res_idx >= 0) resolution->setCurrentIndex(res_idx);
            audio->setCurrentIndex(o.audio_mode == AudioChannelMode::Stereo ? 1 : 0);
            isolate->setChecked(o.isolate_audio);
            break;
        }
    }
}

void ZoomOutputDialog::delete_profile()
{
    const std::string name = m_profile_combo->currentText().toStdString();
    if (name.empty()) return;
    if (QMessageBox::question(this, "Delete Profile",
            QString("Delete profile '%1'?").arg(QString::fromStdString(name)),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    ZoomOutputProfile::remove(name);
    refresh_profiles();
}

void ZoomOutputDialog::apply()
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *name_item = m_table->item(row, ColumnName);
        auto *assignment = qobject_cast<QComboBox *>(
            m_table->cellWidget(row, ColumnAssignment));
        auto *resolution = qobject_cast<QComboBox *>(
            m_table->cellWidget(row, ColumnResolution));
        auto *audio = qobject_cast<QComboBox *>(
            m_table->cellWidget(row, ColumnAudio));
        auto *isolate = qobject_cast<QCheckBox *>(
            m_table->cellWidget(row, ColumnIsolate));
        if (!name_item || !assignment || !resolution || !audio || !isolate) continue;

        const std::string source_name =
            name_item->data(Qt::UserRole).toString().toStdString();
        const QString assignment_data = assignment->currentData().toString();
        const bool active_speaker = assignment_data == "active";
        uint32_t participant_id = 0;
        if (assignment_data.startsWith("user:"))
            participant_id = assignment_data.mid(5).toUInt();
        const auto audio_mode = static_cast<AudioChannelMode>(
            audio->currentData().toInt());
        const auto video_resolution = static_cast<VideoResolution>(
            resolution->currentData().toInt());

        ZoomOutputManager::instance().configure_output(
            source_name, participant_id, active_speaker,
            isolate->isChecked(), audio_mode, video_resolution);
    }
    refresh();
}
