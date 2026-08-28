#include "zoom-dock.h"
#include "cv-style.h"
#include "cv-update-check.h"
#include "cv-widgets.h"
#include "obs-utils.h"
#include "speaker-director.h"
#include "join-watchdog.h"
#include "zoom-engine-client.h"
#include "zoom-join-decision.h"
#include "zoom-oauth.h"
#include "zoom-output-manager.h"
#include "zoom-output-dialog.h"
#include "zoom-reconnect.h"
#include "participant-filter.h"
#include "zoom-settings.h"
#include "zoom-settings-dialog.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMainWindow>
#include <QMetaObject>
#include <QMessageBox>
#include <QMimeData>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStringList>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

// -- Column layout for the output table ---------------------------------------
enum DockOutputColumns {
    DColPreview    = 0,
    DColName       = 1,
    DColAssignment = 2,
    DColRequested  = 3,
    DColSignal     = 4,
    DColAudio      = 5,
    DColIsolate    = 6,
    DColCount      = 7
};

static constexpr int kThumbW = 96;
static constexpr int kThumbH = 54;
static constexpr int kRowH   = 66;

static QString sidecar_executable_path()
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    wchar_t module_path[MAX_PATH] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&sidecar_executable_path),
                           &module) &&
        GetModuleFileNameW(module, module_path, MAX_PATH) > 0) {
        QString path = QString::fromWCharArray(module_path);
        const int slash = path.lastIndexOf(QRegularExpression("[\\\\/]"));
        if (slash >= 0) {
            const QString candidate =
                path.left(slash + 1) + QStringLiteral("CoreVideoSidecar.exe");
            if (QFileInfo::exists(candidate))
                return candidate;
        }
    }

    char *obs_path = obs_module_file("CoreVideoSidecar.exe");
    const QString candidate = obs_path
        ? QString::fromLocal8Bit(obs_path)
        : QStringLiteral("CoreVideoSidecar.exe");
    if (obs_path) bfree(obs_path);
    return candidate;
#else
    return QStringLiteral("CoreVideoSidecar");
#endif
}

// Fast I420 -> RGB888 for dock thumbnails (shared with output dialog logic)
static QImage i420_to_qimage_dock(uint32_t w, uint32_t h,
    const uint8_t *y, const uint8_t *u, const uint8_t *v,
    uint32_t sy, uint32_t suv)
{
    QImage img(kThumbW, kThumbH, QImage::Format_RGB888);
    const float xs = float(w) / kThumbW, ys = float(h) / kThumbH;
    for (int dy = 0; dy < kThumbH; ++dy) {
        const auto iy = uint32_t(dy * ys), icy = iy / 2;
        auto *row = img.scanLine(dy);
        for (int dx = 0; dx < kThumbW; ++dx) {
            const auto ix = uint32_t(dx * xs), icx = ix / 2;
            const int Y = y[iy * sy + ix];
            const int U = u[icy * suv + icx] - 128;
            const int V = v[icy * suv + icx] - 128;
            row[dx*3]   = uint8_t(std::clamp(Y + int(1.402f*V),                          0, 255));
            row[dx*3+1] = uint8_t(std::clamp(Y - int(0.344f*U) - int(0.714f*V),          0, 255));
            row[dx*3+2] = uint8_t(std::clamp(Y + int(1.772f*U),                          0, 255));
        }
    }
    return img;
}

// -- Draggable participant list ------------------------------------------------
// Each item carries participant user_id as Qt::UserRole.
// Dragging emits MIME type "application/x-cv-participant" with the user_id.
class CvParticipantList : public QListWidget {
public:
    explicit CvParticipantList(QWidget *p = nullptr) : QListWidget(p) {
        setDragEnabled(true);
        setDragDropMode(QAbstractItemView::DragOnly);
        setDefaultDropAction(Qt::CopyAction);
        setStyleSheet(
            "QListWidget { border: none; background: transparent; outline: none; }"
            "QListWidget::item { color: #c0c0d8; padding: 5px 8px; border-radius: 4px; }"
            "QListWidget::item:hover { background: #1a1a30; }"
            "QListWidget::item:selected { background: #1d3de8; color: #fff; }");
    }
protected:
    void startDrag(Qt::DropActions) override {
        auto *item = currentItem();
        if (!item) return;
        auto *drag = new QDrag(this);
        auto *data = new QMimeData;
        data->setData("application/x-cv-participant",
                      item->data(Qt::UserRole).toString().toUtf8());
        drag->setMimeData(data);
        drag->exec(Qt::CopyAction);
    }
};

// -- Drop-enabled output table -------------------------------------------------
// Accepts drops from CvParticipantList; sets the assignment combo on the
// row that was hovered when the participant was dropped.
class CvDropOutputTable : public QTableWidget {
public:
    explicit CvDropOutputTable(QWidget *p = nullptr) : QTableWidget(p) {
        setAcceptDrops(true);
        setDropIndicatorShown(true);
    }
protected:
    void dragEnterEvent(QDragEnterEvent *e) override {
        if (e->mimeData()->hasFormat("application/x-cv-participant"))
            e->acceptProposedAction();
        else
            QTableWidget::dragEnterEvent(e);
    }
    void dragMoveEvent(QDragMoveEvent *e) override {
        if (e->mimeData()->hasFormat("application/x-cv-participant"))
            e->acceptProposedAction();
        else
            QTableWidget::dragMoveEvent(e);
    }
    void dropEvent(QDropEvent *e) override {
        if (!e->mimeData()->hasFormat("application/x-cv-participant")) {
            QTableWidget::dropEvent(e);
            return;
        }
        const QString uid =
            QString::fromUtf8(e->mimeData()->data("application/x-cv-participant"));
        const int row = rowAt(e->position().toPoint().y());
        if (row >= 0) {
            auto *combo = qobject_cast<QComboBox *>(cellWidget(row, DColAssignment));
            if (combo) {
                const QString key = QString("user:%1").arg(uid);
                const int idx = combo->findData(key);
                if (idx >= 0) combo->setCurrentIndex(idx);
            }
        }
        e->acceptProposedAction();
    }
};

static const char *state_label_text(MeetingState s)
{
    switch (s) {
    case MeetingState::Idle:       return "Not connected";
    case MeetingState::Joining:    return "Joining...";
    case MeetingState::InMeeting:  return "In meeting";
    case MeetingState::Leaving:    return "Leaving...";
    case MeetingState::Recovering: return "Recovering...";
    case MeetingState::Failed:     return "Connection failed";
    }
    return "";
}

static QString participant_label(const ParticipantInfo &p)
{
    QString label = p.display_name.empty()
        ? QString("ID %1").arg(p.user_id)
        : QString::fromStdString(p.display_name);
    if (p.has_video) label += " [video]";
    return label;
}

static QString participant_roster_label(const ParticipantInfo &p)
{
    QString label = p.display_name.empty()
        ? QString("ID %1").arg(p.user_id)
        : QString::fromStdString(p.display_name);
    QStringList tags;
    tags << QString("ID %1").arg(p.user_id);
    tags << (p.has_video ? QStringLiteral("video") : QStringLiteral("no video"));
    tags << (p.is_muted ? QStringLiteral("muted") : QStringLiteral("audio"));
    if (p.is_talking) tags << QStringLiteral("talking");
    if (p.spotlight_index > 0) tags << QString("spotlight %1").arg(p.spotlight_index);
    if (p.is_sharing_screen) tags << QStringLiteral("sharing");
    return QString("%1  -  %2").arg(label, tags.join(" / "));
}

static QString participant_label_for_id(
    const std::vector<ParticipantInfo> &roster, uint32_t participant_id)
{
    if (participant_id == 0)
        return QStringLiteral("Select participant");
    for (const auto &p : roster) {
        if (p.user_id == participant_id)
            return participant_label(p);
    }
    return QString("ID %1 (not in roster)").arg(participant_id);
}

static bool combo_popup_open(const QComboBox *combo)
{
    return combo && combo->view() && combo->view()->isVisible();
}

// Renders the latest talkback_probe stage line (raw compact JSON from
// EngineTalkback::report(), see engine/src/engine-talkback.cpp) for the
// status label, so the operator gets "create_channel: code=0" instead of a
// JSON blob. The wire format is not contractual across stages -- each stage
// reports whichever field is relevant to it -- so this is deliberately
// defensive: anything that doesn't parse as an object with a "stage" field
// falls back to the raw text verbatim rather than showing nothing or
// crashing. An empty label would be worse than an ugly one; this diagnostic
// exists specifically so the operator does not have to go read the log.
static QString format_talkback_probe_status(const std::string &raw)
{
    if (raw.empty())
        return QStringLiteral("No probe run yet.");

    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(raw));
    if (!doc.isObject())
        return QString::fromStdString(raw);
    const QJsonObject obj = doc.object();
    const QString stage = obj.value("stage").toString();
    if (stage.isEmpty())
        return QString::fromStdString(raw);

    // Checked in rough "most diagnostic" order; the first one present wins.
    static const char *kFields[] = {
        "code", "error", "supported", "buffers", "ok", "phase"
    };
    for (const char *field : kFields) {
        if (!obj.contains(field))
            continue;
        const QJsonValue v = obj.value(field);
        QString rendered;
        if (v.isBool())
            rendered = v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        else if (v.isDouble())
            rendered = QString::number(v.toDouble(), 'f', 0);
        else if (v.isString())
            rendered = v.toString();
        else
            continue;
        return QString("%1: %2=%3").arg(stage, QString::fromLatin1(field), rendered);
    }
    return stage;
}

static bool combo_items_match(
    const QComboBox *combo,
    const std::vector<std::pair<QString, QVariant>> &items)
{
    if (!combo || combo->count() != static_cast<int>(items.size()))
        return false;
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemText(i) != items[static_cast<size_t>(i)].first ||
            combo->itemData(i) != items[static_cast<size_t>(i)].second)
            return false;
    }
    return true;
}

static void replace_combo_items(
    QComboBox *combo,
    const std::vector<std::pair<QString, QVariant>> &items,
    const QVariant &preferred)
{
    if (!combo)
        return;

    const QVariant current = combo->currentData();
    const QVariant target = preferred.isValid() ? preferred : current;
    const bool same_items = combo_items_match(combo, items);

    if (!same_items) {
        combo->blockSignals(true);
        combo->clear();
        for (const auto &item : items)
            combo->addItem(item.first, item.second);
        combo->blockSignals(false);
    }

    const int idx = combo->findData(target);
    if (idx >= 0 && idx != combo->currentIndex()) {
        combo->blockSignals(true);
        combo->setCurrentIndex(idx);
        combo->blockSignals(false);
    } else if (idx < 0 && combo->currentIndex() < 0 && combo->count() > 0) {
        combo->blockSignals(true);
        combo->setCurrentIndex(0);
        combo->blockSignals(false);
    }
}

static bool item_list_contains_data(
    const std::vector<std::pair<QString, QVariant>> &items, uint32_t participant_id)
{
    for (const auto &item : items) {
        if (item.second.toUInt() == participant_id)
            return true;
    }
    return false;
}

static QString screen_share_assignment_label(const std::vector<ParticipantInfo> &roster)
{
    for (const auto &p : roster) {
        if (!p.is_sharing_screen)
            continue;
        const QString name = p.display_name.empty()
            ? QString("ID %1").arg(p.user_id)
            : QString::fromStdString(p.display_name);
        return QString("Screen share - %1").arg(name);
    }
    return QStringLiteral("Screen share - unavailable");
}

static QString signal_label(const ZoomOutputInfo &output)
{
    if (output.health_reason == ZoomOutputHealthReason::RawMediaNotReady)
        return QStringLiteral("Raw media\nnot ready");
    if (output.health_reason == ZoomOutputHealthReason::DuplicateAssignment)
        return QStringLiteral("! Duplicate\nassignment");
    if (output.health_reason == ZoomOutputHealthReason::ParticipantMissing)
        return QStringLiteral("Participant\nmissing");
    if (output.health_reason == ZoomOutputHealthReason::ParticipantVideoOff)
        return QStringLiteral("Video off");
    if (output.health_reason == ZoomOutputHealthReason::ScreenShareUnavailable)
        return QStringLiteral("No screen\nshare");
    if (output.health_reason == ZoomOutputHealthReason::ActiveSpeakerUnavailable)
        return QStringLiteral("No active\nvideo speaker");
    if (output.health_reason == ZoomOutputHealthReason::SpotlightUnavailable)
        return QString("Spotlight %1\nunavailable").arg(output.spotlight_slot);
    if (output.observed_width == 0 || output.observed_height == 0)
        return QStringLiteral("Waiting");
    if (output.video_stale)
        return QString("! Stale\n%1.%2s")
            .arg(output.last_frame_age_ms / 1000)
            .arg((output.last_frame_age_ms / 100) % 10);
    const QString prefix = output_signal_below_requested(output)
        ? QStringLiteral("! ")
        : QString();
    if (output.observed_fps > 0.01) {
        return QString("%1%2x%3\n%4 fps")
            .arg(prefix)
            .arg(output.observed_width)
            .arg(output.observed_height)
            .arg(output.observed_fps, 0, 'f', 1);
    }
    return QString("%1%2x%3")
        .arg(prefix)
        .arg(output.observed_width)
        .arg(output.observed_height);
}

static QString signal_tooltip(const ZoomOutputInfo &output)
{
    if (output.health_reason == ZoomOutputHealthReason::RawMediaNotReady)
        return QStringLiteral("Zoom raw media is not active yet. Start the engine before expecting participant video or audio frames.");
    if (output.health_reason == ZoomOutputHealthReason::DuplicateAssignment)
        return QStringLiteral("This participant is assigned to more than one fixed output. CoreVideo will allow it, but duplicate fixed assignments waste subscriptions and can make quality negotiation less stable.");
    if (output.health_reason == ZoomOutputHealthReason::ParticipantMissing)
        return QStringLiteral("The assigned participant is not currently present in the Zoom roster.");
    if (output.health_reason == ZoomOutputHealthReason::ParticipantVideoOff)
        return QStringLiteral("The assigned participant is present, but Zoom reports their video is off.");
    if (output.health_reason == ZoomOutputHealthReason::ScreenShareUnavailable)
        return QStringLiteral("This output is assigned to screen share, but no participant is currently sharing.");
    if (output.health_reason == ZoomOutputHealthReason::ActiveSpeakerUnavailable)
        return QStringLiteral("This output is assigned to active speaker, but no participant with video is currently talking.");
    if (output.health_reason == ZoomOutputHealthReason::SpotlightUnavailable)
        return QString("This output is assigned to spotlight slot %1, but no current participant owns that slot.")
            .arg(output.spotlight_slot);
    if (output.observed_width == 0 || output.observed_height == 0)
        return output.subscribed_age_ms > 0
            ? QString("No video frame has been received for this output yet. CoreVideo has been waiting %1 ms and will retry automatically if the feed does not arrive.")
                .arg(output.subscribed_age_ms)
            : QStringLiteral("No video frame has been received for this output yet.");
    if (output.video_stale) {
        QString text = QString("Video frames stopped %1 ms ago. CoreVideo is keeping the last frame and will retry this feed automatically.")
            .arg(output.last_frame_age_ms);
        if (output.stale_recovery_attempts > 0) {
            text += QString(" Recovery attempts: %1.")
                .arg(output.stale_recovery_attempts);
        }
        if (output.stale_recovery_cooldown_ms > 0) {
            text += QString(" Next automatic retry in %1 ms.")
                .arg(output.stale_recovery_cooldown_ms);
        }
        return text;
    }

    QString text = QString("Live signal: %1x%2 at %3 fps. Requested: %4x%5.")
        .arg(output.observed_width)
        .arg(output.observed_height)
        .arg(output.observed_fps, 0, 'f', 2)
        .arg(video_resolution_width(output.video_resolution))
        .arg(video_resolution_height(output.video_resolution));
    if (output_signal_below_requested(output)) {
        text += QStringLiteral(" Zoom delivered a lower feed, so CoreVideo is using the best available live feed.");
    }
    if (output.quality_upgrade_attempts > 0) {
        text += QString(" Quality upgrade attempts: %1.")
            .arg(output.quality_upgrade_attempts);
    }
    if (output.quality_upgrade_cooldown_ms > 0) {
        text += QString(" Next quality retry in %1 ms.")
            .arg(output.quality_upgrade_cooldown_ms);
    }
    return text;
}

// -- Constructor ---------------------------------------------------------------
ZoomDock::ZoomDock(QWidget *parent)
    : QWidget(parent)
{
    const ZoomPluginSettings initial_settings = ZoomPluginSettings::load();
    setMinimumWidth(760);

    auto *vLayout = new QVBoxLayout(this);
    vLayout->setContentsMargins(8, 8, 8, 8);
    vLayout->setSpacing(6);

    // -- Status row ------------------------------------------------------------
    m_state_dot   = new CvStatusDot(this);
    m_state_label = new QLabel("Not connected", this);

    auto *status_row = new QHBoxLayout;
    status_row->setSpacing(8);
    status_row->addWidget(m_state_dot);
    status_row->addWidget(m_state_label, 1);
    vLayout->addLayout(status_row);

    m_error_label = new QLabel(this);
    m_error_label->setObjectName("errorLabel");
    m_error_label->setWordWrap(true);
    m_error_label->setVisible(false);
    vLayout->addWidget(m_error_label);

    // -- Active speaker --------------------------------------------------------
    m_speaker_label = new QLabel(QStringLiteral("-"), this);
    m_speaker_label->setObjectName("speakerValue");

    auto *speaker_row = new QHBoxLayout;
    speaker_row->setSpacing(6);
    speaker_row->addWidget(new QLabel("Active speaker:", this));
    speaker_row->addWidget(m_speaker_label, 1);
    vLayout->addLayout(speaker_row);

    auto *speaker_group = new QGroupBox("Active Speaker Director", this);
    auto *speaker_layout = new QVBoxLayout(speaker_group);
    speaker_layout->setSpacing(6);

    auto *speaker_grid = new QGridLayout;
    speaker_grid->setHorizontalSpacing(10);
    speaker_grid->setVerticalSpacing(4);
    m_director_speaker_label = new QLabel(QStringLiteral("-"), speaker_group);
    m_raw_speaker_label = new QLabel(QStringLiteral("-"), speaker_group);
    m_candidate_speaker_label = new QLabel(QStringLiteral("-"), speaker_group);
    m_last_speaker_label = new QLabel(QStringLiteral("-"), speaker_group);
    m_speaker_status_label = new QLabel(QStringLiteral("Automatic"), speaker_group);
    m_speaker_status_label->setWordWrap(true);
    speaker_grid->addWidget(new QLabel("Directed:", speaker_group), 0, 0);
    speaker_grid->addWidget(m_director_speaker_label, 0, 1);
    speaker_grid->addWidget(new QLabel("Raw:", speaker_group), 0, 2);
    speaker_grid->addWidget(m_raw_speaker_label, 0, 3);
    speaker_grid->addWidget(new QLabel("Candidate:", speaker_group), 1, 0);
    speaker_grid->addWidget(m_candidate_speaker_label, 1, 1);
    speaker_grid->addWidget(new QLabel("Last:", speaker_group), 1, 2);
    speaker_grid->addWidget(m_last_speaker_label, 1, 3);
    speaker_layout->addLayout(speaker_grid);
    speaker_layout->addWidget(m_speaker_status_label);

    auto *speaker_controls = new QHBoxLayout;
    speaker_controls->setSpacing(8);
    m_speaker_sensitivity_spin = new QSpinBox(speaker_group);
    m_speaker_sensitivity_spin->setRange(0, 3000);
    m_speaker_sensitivity_spin->setSingleStep(50);
    m_speaker_sensitivity_spin->setSuffix(" ms");
    m_speaker_sensitivity_spin->setValue(
        static_cast<int>(initial_settings.speaker_sensitivity_ms));
    m_speaker_sensitivity_spin->setToolTip(
        "How long someone must keep talking before CoreVideo considers switching.");
    m_speaker_hold_spin = new QSpinBox(speaker_group);
    m_speaker_hold_spin->setRange(0, 10000);
    m_speaker_hold_spin->setSingleStep(100);
    m_speaker_hold_spin->setSuffix(" ms");
    m_speaker_hold_spin->setValue(
        static_cast<int>(initial_settings.speaker_hold_ms));
    m_speaker_hold_spin->setToolTip(
        "Minimum time to hold the current speaker before switching away.");
    m_speaker_preset_combo = new QComboBox(speaker_group);
    m_speaker_preset_combo->addItem("Custom", QVariantMap{});
    m_speaker_preset_combo->addItem("Responsive", QVariantMap{
        {"sensitivity", 250}, {"hold", 1200}
    });
    m_speaker_preset_combo->addItem("Balanced", QVariantMap{
        {"sensitivity", 500}, {"hold", 2000}
    });
    m_speaker_preset_combo->addItem("Stable Panel", QVariantMap{
        {"sensitivity", 900}, {"hold", 3500}
    });
    m_speaker_preset_combo->setToolTip(
        "Presets for automatic active speaker switching behavior.");
    speaker_controls->addWidget(new QLabel("Preset", speaker_group));
    speaker_controls->addWidget(m_speaker_preset_combo);
    speaker_controls->addWidget(new QLabel("Sensitivity", speaker_group));
    speaker_controls->addWidget(m_speaker_sensitivity_spin);
    speaker_controls->addWidget(new QLabel("Hold", speaker_group));
    speaker_controls->addWidget(m_speaker_hold_spin);
    speaker_controls->addStretch(1);
    speaker_layout->addLayout(speaker_controls);
    connect(m_speaker_preset_combo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                if (index <= 0 || !m_speaker_preset_combo ||
                    !m_speaker_sensitivity_spin || !m_speaker_hold_spin)
                    return;
                const QVariantMap preset =
                    m_speaker_preset_combo->itemData(index).toMap();
                const int sensitivity = preset.value("sensitivity").toInt();
                const int hold = preset.value("hold").toInt();
                if (sensitivity <= 0 || hold <= 0)
                    return;
                m_speaker_sensitivity_spin->setValue(sensitivity);
                m_speaker_hold_spin->setValue(hold);
            });
    connect(m_speaker_sensitivity_spin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int value) {
                if (!m_alive->load(std::memory_order_acquire))
                    return;
                auto s = ZoomPluginSettings::load();
                s.speaker_sensitivity_ms = static_cast<uint32_t>(value);
                if (m_speaker_hold_spin)
                    s.speaker_hold_ms = static_cast<uint32_t>(m_speaker_hold_spin->value());
                s.save();
                apply_speaker_director_settings();
                update_state_indicator();
            });
    connect(m_speaker_hold_spin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int value) {
                if (!m_alive->load(std::memory_order_acquire))
                    return;
                auto s = ZoomPluginSettings::load();
                if (m_speaker_sensitivity_spin)
                    s.speaker_sensitivity_ms = static_cast<uint32_t>(
                        m_speaker_sensitivity_spin->value());
                s.speaker_hold_ms = static_cast<uint32_t>(value);
                s.save();
                apply_speaker_director_settings();
                update_state_indicator();
            });

    auto *exclude_controls = new QHBoxLayout;
    exclude_controls->setSpacing(8);
    m_speaker_exclude_combo_1 = new QComboBox(speaker_group);
    m_speaker_exclude_combo_2 = new QComboBox(speaker_group);
    m_speaker_exclude_combo_1->setMinimumWidth(160);
    m_speaker_exclude_combo_2->setMinimumWidth(160);
    m_speaker_exclude_combo_1->setToolTip(
        "Keep this participant out of the automatic active speaker slot.");
    m_speaker_exclude_combo_2->setToolTip(
        "Keep this participant out of the automatic active speaker slot.");
    exclude_controls->addWidget(new QLabel("Exclude", speaker_group));
    exclude_controls->addWidget(m_speaker_exclude_combo_1, 1);
    exclude_controls->addWidget(m_speaker_exclude_combo_2, 1);
    speaker_layout->addLayout(exclude_controls);
    auto save_exclusions = [this]() {
        if (!m_alive->load(std::memory_order_acquire))
            return;
        auto s = ZoomPluginSettings::load();
        s.speaker_exclude_participant_1 = m_speaker_exclude_combo_1
            ? static_cast<uint32_t>(m_speaker_exclude_combo_1->currentData().toUInt())
            : 0;
        s.speaker_exclude_participant_2 = m_speaker_exclude_combo_2
            ? static_cast<uint32_t>(m_speaker_exclude_combo_2->currentData().toUInt())
            : 0;
        if (s.speaker_exclude_participant_1 == s.speaker_exclude_participant_2)
            s.speaker_exclude_participant_2 = 0;
        s.save();
        apply_speaker_director_settings();
        const bool changed = SpeakerDirector::instance().update_roster(
                ZoomEngineClient::instance().roster(),
                ZoomEngineClient::instance().active_speaker_id(),
                os_gettime_ns() / 1000000ULL);
        if (changed)
            ZoomOutputManager::instance().resubscribe_all();
        update_state_indicator();
    };
    connect(m_speaker_exclude_combo_1, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [save_exclusions](int) { save_exclusions(); });
    connect(m_speaker_exclude_combo_2, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [save_exclusions](int) { save_exclusions(); });

    auto *override_controls = new QHBoxLayout;
    override_controls->setSpacing(8);
    m_speaker_override_combo = new QComboBox(speaker_group);
    m_speaker_override_combo->setMinimumWidth(180);
    m_speaker_override_combo->setToolTip(
        "Manually supersede the automatic speaker director until released.");
    m_speaker_take_btn = new QPushButton("Take", speaker_group);
    m_speaker_take_btn->setProperty("role", "primary");
    m_speaker_release_btn = new QPushButton("Release", speaker_group);
    override_controls->addWidget(new QLabel("Manual", speaker_group));
    override_controls->addWidget(m_speaker_override_combo, 1);
    override_controls->addWidget(m_speaker_take_btn);
    override_controls->addWidget(m_speaker_release_btn);
    speaker_layout->addLayout(override_controls);
    connect(m_speaker_take_btn, &QPushButton::clicked, this, [this]() {
        if (!m_speaker_override_combo)
            return;
        const uint32_t participant_id =
            static_cast<uint32_t>(m_speaker_override_combo->currentData().toUInt());
        if (SpeakerDirector::instance().set_manual_speaker(
                participant_id, os_gettime_ns() / 1000000ULL))
            ZoomOutputManager::instance().resubscribe_all();
        update_state_indicator();
    });
    connect(m_speaker_release_btn, &QPushButton::clicked, this, [this]() {
        if (SpeakerDirector::instance().clear_manual_speaker(
                os_gettime_ns() / 1000000ULL))
            ZoomOutputManager::instance().resubscribe_all();
        update_state_indicator();
    });
    vLayout->addWidget(speaker_group);
    // Credentials notice is hidden for published builds with an embedded
    // Meeting SDK public app key.
    m_credentials_banner = new CvBanner(
        CvBannerKind::Info,
        "CoreVideo is missing its embedded Zoom app identity.",
        this);
    m_credentials_banner->setActionText("Open Settings");
    connect(m_credentials_banner, &CvBanner::actionClicked, this, [this]() {
        ZoomSettingsDialog dlg(this);
        dlg.exec();
        update_credentials_banner();
    });
    vLayout->addWidget(m_credentials_banner);

    // -- Update-available notice -------------------------------------------------
    // Non-intrusive: hidden until (and unless) CvUpdateChecker confirms a
    // newer release exists. Never blocks startup, never auto-downloads.
    m_update_banner = new CvBanner(CvBannerKind::Info, QString(), this);
    m_update_banner->setActionText("View release");
    m_update_banner->setVisible(false);
    connect(m_update_banner, &CvBanner::actionClicked, this, [this]() {
        if (!m_update_url.isEmpty())
            QDesktopServices::openUrl(QUrl(m_update_url));
    });
    vLayout->addWidget(m_update_banner);

    connect(&CvUpdateChecker::instance(), &CvUpdateChecker::update_available,
            this, [this](const QString &tag, const QString &html_url) {
        show_update_banner(tag, html_url);
    });
    if (CvUpdateChecker::instance().has_known_update()) {
        show_update_banner(CvUpdateChecker::instance().known_update_tag(),
                           CvUpdateChecker::instance().known_update_url());
    } else if (initial_settings.check_for_updates_on_startup) {
        CvUpdateChecker::instance().check_once();
    }

    // -- Join controls ---------------------------------------------------------
    auto *join_group  = new QGroupBox("Join Meeting", this);
    auto *join_layout = new QVBoxLayout(join_group);
    join_layout->setSpacing(6);

    m_meeting_id = new QLineEdit(join_group);
    m_meeting_id->setPlaceholderText("Meeting ID or Zoom URL");
    m_meeting_id->setToolTip(
        "Enter a numeric meeting ID or paste a full Zoom URL "
        "(e.g. https://zoom.us/j/123?pwd=abc) - the ID and passcode "
        "will be extracted automatically.");

    m_passcode = new QLineEdit(join_group);
    m_passcode->setPlaceholderText("Passcode (optional)");
    m_passcode->setEchoMode(QLineEdit::Password);

    m_display_name = new QLineEdit(join_group);
    m_display_name->setPlaceholderText("Display name");

    m_join_token_type = new QComboBox(join_group);
    m_join_token_type->addItem("Zoom sign-in", "auto_zak");
    m_join_token_type->addItem("User ZAK", "user_zak");
    m_join_token_type->addItem("App privilege token", "app_privilege_token");
    m_join_token_type->setToolTip(
        "Use Zoom sign-in for normal joins. App privilege tokens are only "
        "for raw media permission; CoreVideo will still use signed-in Zoom "
        "context for the meeting join.");

    m_join_token = new QLineEdit(join_group);
    m_join_token->setPlaceholderText("Automatic from Zoom sign-in");
    m_join_token->setEchoMode(QLineEdit::Password);
    m_join_token->setToolTip(
        "Paste a user ZAK only when Zoom support provides one. Paste an app "
        "privilege token only for raw media permission.");
    m_join_token->setEnabled(false);
    connect(m_join_token_type, &QComboBox::currentIndexChanged, this, [this]() {
        const bool manual =
            m_join_token_type &&
            m_join_token_type->currentData().toString() != "auto_zak";
        m_join_token->setEnabled(manual);
        m_join_token->setPlaceholderText(
            manual ? QStringLiteral("Paste ZAK or app privilege token")
                   : QStringLiteral("Automatic from Zoom sign-in"));
    });

    m_join_btn  = new QPushButton("Join",  join_group);
    m_leave_btn = new QPushButton("Leave", join_group);
    m_leave_btn->setEnabled(false);

    // Role-based styling - evaluated when stylesheet is applied below
    m_join_btn->setProperty("role", "primary");
    m_leave_btn->setProperty("role", "danger");

    auto *join_btn_row = new QHBoxLayout;
    join_btn_row->setSpacing(6);
    join_btn_row->addWidget(m_join_btn);
    join_btn_row->addWidget(m_leave_btn);

    m_webinar_cb = new QCheckBox("Join as Webinar / Zoom Events", join_group);

    join_layout->addWidget(m_meeting_id);
    join_layout->addWidget(m_passcode);
    join_layout->addWidget(m_display_name);
    join_layout->addWidget(m_join_token_type);
    join_layout->addWidget(m_join_token);
    join_layout->addWidget(m_webinar_cb);
    join_layout->addLayout(join_btn_row);
    vLayout->addWidget(join_group);

    auto *engine_group  = new QGroupBox("Broadcast Engine", this);
    auto *engine_layout = new QHBoxLayout(engine_group);
    engine_layout->setSpacing(6);
    m_start_engine_btn = new QPushButton("Start Engine", engine_group);
    m_stop_engine_btn = new QPushButton("Stop Engine", engine_group);
    m_start_engine_btn->setProperty("role", "primary");
    m_stop_engine_btn->setProperty("role", "danger");
    m_start_engine_btn->setToolTip(
        "Start Zoom raw media capture and send participant video/audio to OBS outputs.");
    m_stop_engine_btn->setToolTip(
        "Stop Zoom raw media capture while staying joined to the meeting.");
    m_stop_engine_btn->setEnabled(false);
    engine_layout->addWidget(m_start_engine_btn);
    engine_layout->addWidget(m_stop_engine_btn);
    vLayout->addWidget(engine_group);

    auto *routing_group = new QGroupBox("Routing", this);
    auto *routing_layout = new QHBoxLayout(routing_group);
    routing_layout->setSpacing(8);
    auto *routing_label = new QLabel(
        "Manage participant-to-source assignments in the dedicated Output Manager.",
        routing_group);
    routing_label->setWordWrap(true);
    m_output_manager_btn = new QPushButton("Open Output Manager", routing_group);
    connect(m_output_manager_btn, &QPushButton::clicked,
            this, [this]() { open_output_manager(); });
    routing_layout->addWidget(routing_label, 1);
    routing_layout->addWidget(m_output_manager_btn);
    vLayout->addWidget(routing_group);

    // -- Talkback probe (Milestone 1 diagnostic) --------------------------------
    // This is NOT the talkback feature (Milestone 7: talk button, PTT, latch,
    // tally, level meter). Those are deliberately out of this dock -- the
    // operator has confirmed it stays config-and-tally-only. This is only the
    // "can this account even open a channel" probe, exposed here because it
    // was previously reachable only over the TCP control API, and its
    // progress otherwise only ever showed up as OBS log lines.
    auto *talkback_group = new QGroupBox("Talkback (probe)", this);
    auto *talkback_layout = new QVBoxLayout(talkback_group);
    talkback_layout->setSpacing(6);

    auto *talkback_info = new QLabel(
        "Milestone 1 diagnostic, not the talkback feature. Running it opens a "
        "channel, invites the selected participant, and sends a 3-second "
        "440Hz test tone that they WILL hear, ducking their meeting audio to "
        "30% for the duration; the channel is destroyed afterward. Requires "
        "host or co-host -- a plain participant is refused with "
        "SDKERR_NO_PERMISSION.",
        talkback_group);
    talkback_info->setWordWrap(true);
    talkback_layout->addWidget(talkback_info);

    // A dedicated, roster-driven selector -- not the legacy m_participant_list
    // above. That QListWidget is only ever filled inside refresh_outputs(),
    // and that function's very first line returns whenever m_output_table is
    // null, which it always is now that routing lives in the Output Manager
    // dialog, so it never actually populates. This combo is rebuilt from the
    // live roster every tick in update_state_indicator(), the same way
    // m_speaker_override_combo above it already is.
    m_talkback_participant_combo = new QComboBox(talkback_group);
    m_talkback_participant_combo->addItem(QStringLiteral("Select participant"), QVariant());
    m_talkback_participant_combo->setToolTip(
        "Participant to invite into the probe talkback channel.");
    talkback_layout->addWidget(m_talkback_participant_combo);

    m_talkback_probe_btn = new QPushButton("Probe Selected Participant", talkback_group);
    m_talkback_probe_btn->setEnabled(false);
    talkback_layout->addWidget(m_talkback_probe_btn);

    m_talkback_status_label = new QLabel(
        format_talkback_probe_status(ZoomEngineClient::instance().talkback_probe_status()),
        talkback_group);
    m_talkback_status_label->setWordWrap(true);
    m_talkback_status_label->setObjectName("talkbackProbeStatus");
    talkback_layout->addWidget(m_talkback_status_label);

    connect(m_talkback_probe_btn, &QPushButton::clicked, this, [this]() {
        if (!m_talkback_participant_combo)
            return;
        // Passing the NAME, never the id the combo could have carried
        // instead: Zoom user ids are meeting-scoped, so an id captured now
        // would point at nobody after a rejoin and at the wrong person once
        // ids get recycled -- the by-name resolution this whole feature (and
        // the Companion module, see CLAUDE.md) is built on.
        const QString name = m_talkback_participant_combo->currentData().toString();
        if (name.isEmpty())
            return;
        ZoomEngineClient::instance().talkback_probe(name.toStdString());
    });
    vLayout->addWidget(talkback_group);

    // Pre-populate join fields from the last successful join, if any.
    {
        const ZoomPluginSettings prefill = ZoomPluginSettings::load();
        if (!prefill.last_meeting_id.empty())
            m_meeting_id->setText(QString::fromStdString(prefill.last_meeting_id));
        m_display_name->setText(
            prefill.last_display_name.empty()
                ? QStringLiteral("OBS")
                : QString::fromStdString(prefill.last_display_name));
        m_webinar_cb->setChecked(prefill.last_was_webinar);
    }

    connect(m_join_btn,  &QPushButton::clicked, this, [this]() { on_join_clicked(); });
    connect(m_leave_btn, &QPushButton::clicked, this, [this]() { on_leave_clicked(); });
    connect(m_start_engine_btn, &QPushButton::clicked,
            this, [this]() { on_start_engine_clicked(); });
    connect(m_stop_engine_btn, &QPushButton::clicked,
            this, [this]() { on_stop_engine_clicked(); });
    m_pending_oauth_join_timer = new QTimer(this);
    m_pending_oauth_join_timer->setInterval(500);
    connect(m_pending_oauth_join_timer, &QTimer::timeout, this, [this]() {
        if (!m_alive->load(std::memory_order_acquire))
            return;
        const ZoomPluginSettings s = ZoomPluginSettings::load();
        if (s.oauth_access_token.empty() && s.oauth_refresh_token.empty())
            return;
        stop_pending_oauth_join();
        on_join_clicked();
    });

    // -- Recovery panel --------------------------------------------------------
    m_recovery_frame = new QFrame(this);
    m_recovery_frame->setObjectName("recoveryPanel");

    m_recovery_label  = new QLabel("", m_recovery_frame);
    m_recovery_label->setWordWrap(true);

    m_cancel_rec_btn = new QPushButton("Cancel Recovery", m_recovery_frame);
    connect(m_cancel_rec_btn, &QPushButton::clicked,
            this, [this]() { on_cancel_recovery_clicked(); });

    auto *rec_inner = new QHBoxLayout(m_recovery_frame);
    rec_inner->setContentsMargins(10, 7, 10, 7);
    rec_inner->setSpacing(8);
    rec_inner->addWidget(m_recovery_label, 1);
    rec_inner->addWidget(m_cancel_rec_btn);
    vLayout->addWidget(m_recovery_frame);

    m_countdown_timer = new QTimer(this);
    m_countdown_timer->setInterval(500);
    connect(m_countdown_timer, &QTimer::timeout, this, [this]() {
        update_recovery_panel();
    });

    // Periodic tick updates the lightweight state indicator, active speaker
    // director, and join-timeout watchdog.
    //
    // Deliberately NOT calling resubscribe_all() on an ordinary promotion
    // here (live-caught 2026-08-19: every automatic Active Speaker cut was
    // force-resubscribing all 8 unrelated fixed-participant outputs, every
    // 12-90s for the length of a show). That call was copied from the
    // manual Take/Release override buttons below, where resubscribe_all()
    // is at least a rare, deliberate operator action; the periodic tick
    // fires on every ordinary automatic cut. It was also never load-bearing
    // for the cut itself: SpeakerDirector::tick()'s return value only says
    // a promotion happened, and the Active Speaker source already picks up
    // the new directed_speaker_id() on its own, through the existing
    // director-handover machinery in zoom-source.cpp -- see
    // ZoomEngineClient::active_speaker_id(), which reads
    // SpeakerDirector::directed_speaker_id() directly and has no
    // dependency on resubscribe_all() ever having run.
    // resubscribe_all()'s own doc comment says what it is actually for:
    // recovering every source after an engine crash/reconnect, when the
    // engine "is brand new and knows nothing" -- not a routine speaker
    // change.
    m_refresh_timer = new QTimer(this);
    m_refresh_timer->setInterval(100);
    connect(m_refresh_timer, &QTimer::timeout, this, [this]() {
        SpeakerDirector::instance().tick(os_gettime_ns() / 1000000ULL);
        update_state_indicator();
    });
    m_refresh_timer->start();

    // Feed recovery checks can touch every CoreVideo source. Keep them off the
    // high-frequency UI timer so an 8-feed show does not spend unnecessary time
    // scanning outputs while still recovering stale/low-quality feeds promptly.
    m_health_retry_timer = new QTimer(this);
    m_health_retry_timer->setInterval(1000);
    connect(m_health_retry_timer, &QTimer::timeout, this, []() {
        ZoomOutputManager::instance().recover_stale_sources();
        ZoomOutputManager::instance().upgrade_low_quality_sources();
    });
    m_health_retry_timer->start();

    m_recovery_frame->setVisible(false);

    // -- Subscribe to roster / state changes -----------------------------------
    auto self  = this;
    auto alive = m_alive;

    ZoomEngineClient::instance().add_roster_callback(this, [self, alive]() {
        QMetaObject::invokeMethod(self, [self, alive]() {
            if (alive->load(std::memory_order_acquire)) self->refresh();
        }, Qt::QueuedConnection);
    });
    ZoomEngineClient::instance().add_error_callback(this, [self, alive](const std::string &message) {
        QMetaObject::invokeMethod(self, [self, alive, message]() {
            if (!alive->load(std::memory_order_acquire)) return;
            const QString text = QString::fromStdString(message);
            self->m_error_label->setText(text);
            self->m_error_label->setVisible(!text.isEmpty());
            self->update_state_indicator();
            if (!text.isEmpty())
                QMessageBox::warning(self, "Zoom Join", text);
        }, Qt::QueuedConnection);
    });

    // -- Apply stylesheet last so all properties are set before evaluation -----
    setStyleSheet(cv_stylesheet());

    update_credentials_banner();
    refresh();

}

ZoomDock::~ZoomDock()
{
    prepare_shutdown();
    if (m_join_thread.joinable())
        m_join_thread.join();
    ZoomEngineClient::instance().remove_roster_callback(this);
    ZoomEngineClient::instance().remove_error_callback(this);
}

void ZoomDock::prepare_shutdown()
{
    m_alive->store(false, std::memory_order_release);
    if (m_speaker_sensitivity_spin)
        QObject::disconnect(m_speaker_sensitivity_spin, nullptr, this, nullptr);
    if (m_speaker_hold_spin)
        QObject::disconnect(m_speaker_hold_spin, nullptr, this, nullptr);
    if (m_speaker_exclude_combo_1)
        QObject::disconnect(m_speaker_exclude_combo_1, nullptr, this, nullptr);
    if (m_speaker_exclude_combo_2)
        QObject::disconnect(m_speaker_exclude_combo_2, nullptr, this, nullptr);
    ZoomEngineClient::instance().remove_roster_callback(this);
    ZoomEngineClient::instance().remove_error_callback(this);
    stop_pending_oauth_join();
    if (m_countdown_timer)
        m_countdown_timer->stop();
    if (m_refresh_timer)
        m_refresh_timer->stop();
    if (m_health_retry_timer)
        m_health_retry_timer->stop();
}

void ZoomDock::apply_speaker_director_settings()
{
    const auto settings = ZoomPluginSettings::load();
    std::vector<uint32_t> excluded;
    if (settings.speaker_exclude_participant_1 != 0)
        excluded.push_back(settings.speaker_exclude_participant_1);
    if (settings.speaker_exclude_participant_2 != 0 &&
        settings.speaker_exclude_participant_2 !=
            settings.speaker_exclude_participant_1)
        excluded.push_back(settings.speaker_exclude_participant_2);

    const uint32_t sensitivity_ms = m_speaker_sensitivity_spin
        ? static_cast<uint32_t>(m_speaker_sensitivity_spin->value())
        : settings.speaker_sensitivity_ms;
    const uint32_t hold_ms = m_speaker_hold_spin
        ? static_cast<uint32_t>(m_speaker_hold_spin->value())
        : settings.speaker_hold_ms;

    SpeakerDirector::instance().configure(
        sensitivity_ms, hold_ms, settings.speaker_require_video, excluded);
}

// -- Internal helpers ----------------------------------------------------------
void ZoomDock::update_credentials_banner()
{
    if (!m_alive->load(std::memory_order_acquire))
        return;
    const ZoomPluginSettings s = ZoomPluginSettings::load();
    const bool has_public_app_key =
        !s.resolved_meeting_sdk_public_app_key().empty();
    const bool has_jwt = !s.resolved_jwt_token().empty();
    const bool has_sdk_pair = !s.sdk_key.empty() && !s.sdk_secret.empty();
    const bool missing = !has_public_app_key && !has_jwt && !has_sdk_pair;
    m_credentials_banner->setVisible(missing);
}

void ZoomDock::show_update_banner(const QString &tag, const QString &html_url)
{
    if (!m_alive->load(std::memory_order_acquire) || !m_update_banner)
        return;
    m_update_url = html_url;
    m_update_banner->setText(QString("CoreVideo %1 is available.").arg(tag));
    m_update_banner->setVisible(true);
}

void ZoomDock::start_pending_oauth_join()
{
    if (m_pending_oauth_join_timer && !m_pending_oauth_join_timer->isActive())
        m_pending_oauth_join_timer->start();
}

void ZoomDock::stop_pending_oauth_join()
{
    if (m_pending_oauth_join_timer)
        m_pending_oauth_join_timer->stop();
}

void ZoomDock::update_recovery_panel()
{
    const bool recovering = ZoomReconnectManager::instance().is_recovering();
    m_recovery_frame->setVisible(recovering);

    if (!recovering) {
        m_countdown_timer->stop();
        return;
    }

    const int attempt = ZoomReconnectManager::instance().attempt_count();
    const int max_att = ZoomReconnectManager::instance().policy().max_attempts;
    const int ms_left = ZoomReconnectManager::instance().next_retry_ms();

    if (ms_left > 0) {
        m_recovery_label->setText(
            QString("Reconnecting in %1s... (attempt %2/%3)")
                .arg((ms_left + 999) / 1000)
                .arg(attempt + 1)
                .arg(max_att));
    } else {
        m_recovery_label->setText(
            QString("Reconnecting... (attempt %1/%2)")
                .arg(attempt)
                .arg(max_att));
    }

    if (!m_countdown_timer->isActive())
        m_countdown_timer->start();
}

void ZoomDock::update_state_indicator()
{
    const MeetingState s = ZoomEngineClient::instance().state();

    // Don't charge the Zoom SDK init retry against the join deadline below.
    //
    // When an orphaned ZoomObsEngine from a previous OBS session still holds
    // the SDK, the engine's InitSDK is replayed on a bounded schedule
    // (zoom-sdk-init-retry.h): up to 78s of scheduled delay, plus the monitor
    // thread's ~1s tick per attempt, plus the round trips themselves. All of
    // that runs with the state already at Joining — on_join_clicked() sets
    // Joining before start(), and every start() is immediately followed by
    // join() — so the two-minute deadline would be most of the way spent before
    // the join request even reaches Zoom, and a meeting with a waiting room (or
    // one that has not started yet) would be cancelled while it was still
    // progressing normally.
    //
    // Holding the start of the window at "now" while a retry is pending keeps
    // the deadline measuring the join itself. It cannot hold the watchdog off
    // indefinitely: the retry schedule is capped at kSdkInitRetryMaxAttempts,
    // and exhausting it tears the engine down and moves the state to Failed,
    // which clears m_join_started_ms below.
    // The other open-ended wait inside the Joining window is Zoom holding us in
    // a waiting room, or the meeting not having started yet. That one is not
    // bounded by anything we control -- it ends when a host acts -- and on
    // 2026-08-22 it auto-left a live meeting after 114s of waiting. Both
    // suppressors and the deadline itself are decided by
    // join_watchdog_action() so they can be tested without Qt; see
    // src/join-watchdog.h.
    const JoinWatchdogAction watchdog = join_watchdog_action(
        s == MeetingState::Joining,
        m_join_timeout_reported,
        static_cast<uint64_t>(m_join_started_ms),
        static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()),
        ZoomEngineClient::instance().is_init_retry_pending(),
        ZoomEngineClient::instance().is_awaiting_admission(),
        kJoinWatchdogTimeoutMs);

    if (watchdog == JoinWatchdogAction::HoldWindow) {
        if (!m_join_wait_logged &&
            ZoomEngineClient::instance().is_awaiting_admission()) {
            m_join_wait_logged = true;
            blog(LOG_INFO,
                 "[obs-zoom-plugin] Join watchdog held: Zoom has us in a "
                 "waiting room (or the meeting has not started yet). This is "
                 "not a stalled join and will not be auto-cancelled");
        }
        m_join_started_ms = QDateTime::currentMSecsSinceEpoch();
    }

    if (watchdog == JoinWatchdogAction::Fire) {
        m_join_timeout_reported = true;
        // The warning dialog below is visible in the moment but leaves no
        // trace in the log — a real gap when this fires (as opposed to a
        // deliberate Leave click) is exactly what silently set
        // m_user_leaving and suppressed the next crash's auto-recovery in
        // a 2026-08-21 live incident that took real archaeology to explain
        // after the fact. blog() first so the log alone tells the story.
        blog(LOG_WARNING,
             "[obs-zoom-plugin] Join watchdog: no join progress in 120s -- "
             "auto-leaving and marking this attempt Failed");
        ZoomEngineClient::instance().leave();
        ZoomEngineClient::instance().set_state(MeetingState::Failed);
        QMessageBox::warning(this, "Zoom Join",
            "Zoom did not finish joining within two minutes. The join attempt "
            "was cancelled so you can try again.");
    }

    if (s != MeetingState::Joining) {
        m_join_started_ms = 0;
        m_join_timeout_reported = false;
        m_join_wait_logged = false;
    }

    m_state_dot->setState(s);
    const std::string last_error = ZoomEngineClient::instance().last_error();
    if (s == MeetingState::Failed && !last_error.empty()) {
        m_state_label->setText("Connection failed");
        m_error_label->setText(QString::fromStdString(last_error));
        m_error_label->setVisible(true);
    } else {
        m_state_label->setText(state_label_text(s));
        m_error_label->setVisible(false);
    }

    const bool in_meeting    = (s == MeetingState::InMeeting);
    const bool recovering    = (s == MeetingState::Recovering);
    const bool join_task     = m_join_in_progress.load(std::memory_order_acquire);
    const bool transitioning = join_task || s == MeetingState::Joining ||
                               s == MeetingState::Leaving;
    const bool media_active = ZoomEngineClient::instance().is_media_active();
    if (media_active && !m_last_media_active)
        ZoomOutputManager::instance().resubscribe_all();
    m_last_media_active = media_active;
    m_join_btn->setEnabled(!in_meeting && !transitioning && !recovering);
    m_leave_btn->setEnabled(in_meeting || transitioning || recovering);
    m_leave_btn->setText(in_meeting ? "Leave" : "Cancel");
    if (m_start_engine_btn && m_stop_engine_btn) {
        m_start_engine_btn->setEnabled(in_meeting && !media_active && !transitioning && !recovering);
        m_stop_engine_btn->setEnabled(in_meeting && media_active && !transitioning);
    }

    update_recovery_panel();

    // Active speaker name
    const uint32_t spk_id = ZoomEngineClient::instance().active_speaker_id();
    if (spk_id == 0) {
        m_speaker_label->setText(QStringLiteral("-"));
    } else {
        QString spk_name = QString("ID %1").arg(spk_id);
        for (const auto &p : ZoomEngineClient::instance().roster()) {
            if (p.user_id == spk_id && !p.display_name.empty()) {
                spk_name = QString::fromStdString(p.display_name);
                break;
            }
        }
        m_speaker_label->setText(spk_name);
    }

    const auto settings = ZoomPluginSettings::load();
    apply_speaker_director_settings();
    if (m_director_speaker_label && m_raw_speaker_label &&
        m_candidate_speaker_label && m_last_speaker_label) {
        const auto roster = ZoomEngineClient::instance().roster();
        auto participant_name = [&roster](uint32_t participant_id) {
            if (participant_id == 0)
                return QStringLiteral("-");
            QString name = QString("ID %1").arg(participant_id);
            for (const auto &p : roster) {
                if (p.user_id == participant_id && !p.display_name.empty()) {
                    name = QString::fromStdString(p.display_name);
                    break;
                }
            }
            return name;
        };
        const auto director =
            SpeakerDirector::instance().snapshot(os_gettime_ns() / 1000000ULL);
        const QString directed_name = participant_name(director.directed_speaker_id);
        m_speaker_label->setText(directed_name);
        m_director_speaker_label->setText(directed_name);
        m_raw_speaker_label->setText(participant_name(director.raw_speaker_id));
        m_candidate_speaker_label->setText(participant_name(director.candidate_speaker_id));
        m_last_speaker_label->setText(participant_name(director.last_speaker_id));
        const bool speaker_popup_open =
            combo_popup_open(m_speaker_preset_combo) ||
            combo_popup_open(m_speaker_override_combo) ||
            combo_popup_open(m_speaker_exclude_combo_1) ||
            combo_popup_open(m_speaker_exclude_combo_2);
        if (!speaker_popup_open) {
            if (m_speaker_override_combo) {
                std::vector<std::pair<QString, QVariant>> items;
                items.emplace_back(QStringLiteral("Select participant"), 0);
                for (const auto &p : roster) {
                    if (p.user_id == 0 || p.is_muted || !p.has_video)
                        continue;
                    items.emplace_back(participant_label(p), p.user_id);
                }

                const uint32_t current_id = static_cast<uint32_t>(
                    m_speaker_override_combo->currentData().toUInt());
                if (current_id != 0 && !item_list_contains_data(items, current_id))
                    items.emplace_back(participant_label_for_id(roster, current_id),
                                       current_id);
                if (director.manual_speaker_id != 0 &&
                    !item_list_contains_data(items, director.manual_speaker_id)) {
                    items.emplace_back(
                        participant_label_for_id(roster, director.manual_speaker_id),
                        director.manual_speaker_id);
                }

                const uint32_t preferred_id =
                    current_id != 0 ? current_id : director.manual_speaker_id;
                replace_combo_items(m_speaker_override_combo, items,
                                    QVariant(preferred_id));
            }

            auto refresh_exclude_combo =
                [&roster](QComboBox *combo, uint32_t selected) {
                    if (!combo)
                        return;
                    std::vector<std::pair<QString, QVariant>> items;
                    items.emplace_back(QStringLiteral("No exclusion"), 0);
                    for (const auto &p : roster) {
                        if (p.user_id == 0)
                            continue;
                        items.emplace_back(participant_label(p), p.user_id);
                    }
                    const uint32_t current_id = static_cast<uint32_t>(
                        combo->currentData().toUInt());
                    const uint32_t preferred_id =
                        selected != current_id ? selected : current_id;
                    if (preferred_id != 0 &&
                        !item_list_contains_data(items, preferred_id)) {
                        items.emplace_back(
                            participant_label_for_id(roster, preferred_id),
                            preferred_id);
                    }
                    replace_combo_items(combo, items, QVariant(preferred_id));
                };
            refresh_exclude_combo(m_speaker_exclude_combo_1,
                                  settings.speaker_exclude_participant_1);
            refresh_exclude_combo(m_speaker_exclude_combo_2,
                                  settings.speaker_exclude_participant_2);
        }
        if (m_speaker_take_btn && m_speaker_override_combo) {
            m_speaker_take_btn->setEnabled(
                m_speaker_override_combo->currentData().toUInt() != 0);
        }
        if (m_speaker_release_btn)
            m_speaker_release_btn->setEnabled(director.manual_active);
        m_candidate_speaker_label->setToolTip(
            QString("Candidate age: %1 ms").arg(
                static_cast<qulonglong>(director.candidate_elapsed_ms)));
        QString status;
        if (director.manual_active) {
            status = QString("Manual supersede locked to %1.").arg(directed_name);
        } else if (director.candidate_speaker_id != 0) {
            status = QString("Candidate %1 has held for %2 ms; current speaker can switch in %3 ms.")
                .arg(participant_name(director.candidate_speaker_id))
                .arg(static_cast<qulonglong>(director.candidate_elapsed_ms))
                .arg(static_cast<qulonglong>(director.hold_remaining_ms));
        } else if (director.directed_speaker_id != 0) {
            status = QString("Automatic. Holding %1; switch hold remaining %2 ms.")
                .arg(directed_name)
                .arg(static_cast<qulonglong>(director.hold_remaining_ms));
        } else {
            status = director.require_video
                ? QStringLiteral("Waiting for an unmuted participant with video.")
                : QStringLiteral("Waiting for an unmuted participant.");
        }
        if (!director.excluded_participant_ids.empty()) {
            QStringList excluded_names;
            for (const uint32_t id : director.excluded_participant_ids)
                excluded_names << participant_name(id);
            status += QString(" Excluding: %1.").arg(excluded_names.join(", "));
        }
        if (m_speaker_status_label)
            m_speaker_status_label->setText(status);
        m_speaker_label->setToolTip(status);
        m_director_speaker_label->setToolTip(m_speaker_label->toolTip());
    }

    // -- Talkback probe (Milestone 1 diagnostic) --------------------------------
    // Rebuilt every tick from the live roster, the same way
    // m_speaker_override_combo is above -- see the comment where this combo is
    // constructed for why it is not the legacy m_participant_list.
    if (m_talkback_participant_combo && !combo_popup_open(m_talkback_participant_combo)) {
        std::vector<std::pair<QString, QVariant>> items;
        items.emplace_back(QStringLiteral("Select participant"), QVariant());
        for (const auto &p : ZoomEngineClient::instance().roster()) {
            if (p.user_id == 0 || p.display_name.empty())
                continue;
            items.emplace_back(participant_label(p),
                               QVariant(QString::fromStdString(p.display_name)));
        }
        replace_combo_items(m_talkback_participant_combo, items, QVariant());
    }
    if (m_talkback_probe_btn && m_talkback_participant_combo) {
        // Disabled outside a live meeting (nobody to invite/nowhere to open a
        // channel) and until a participant is actually selected -- the combo's
        // placeholder item carries an invalid/empty data(), which
        // currentData().toString() reports as empty.
        m_talkback_probe_btn->setEnabled(
            in_meeting &&
            !m_talkback_participant_combo->currentData().toString().isEmpty());
    }
    if (m_talkback_status_label) {
        // Polled, not pushed: handle_event()'s talkback_probe branch just
        // stores the latest line under m_mtx (see zoom-engine-client.cpp) and
        // this timer-driven read picks it up on the next 100ms tick, matching
        // how every other readout on this dock (last_error(), roster(),
        // active_speaker_id()) already reaches the UI.
        m_talkback_status_label->setText(format_talkback_probe_status(
            ZoomEngineClient::instance().talkback_probe_status()));
    }
}

void ZoomDock::refresh()
{
    update_state_indicator();
}

void ZoomDock::refresh_outputs()
{
    if (!m_output_table)
        return;
    // If a dropdown popup is open, the user is mid-selection - rebuilding the
    // widgets right now would close the popup and lose the pick. Defer; the
    // next roster update (or any later refresh) will rebuild instead.
    for (int row = 0; row < m_output_table->rowCount(); ++row) {
        for (const int col : {DColAssignment, DColRequested, DColAudio}) {
            if (auto *combo = qobject_cast<QComboBox *>(
                    m_output_table->cellWidget(row, col))) {
                if (combo->view() && combo->view()->isVisible()) return;
            }
        }
    }

    // Snapshot any in-flight (picked-but-not-yet-applied) selections so that
    // a roster-driven rebuild does not silently revert the user's choice
    // before they have a chance to click Apply.
    struct PendingPick { QString assignment; int requested_idx; int audio_idx; bool isolate; };
    std::unordered_map<std::string, PendingPick> pending;
    const int output_vscroll = m_output_table->verticalScrollBar()
        ? m_output_table->verticalScrollBar()->value()
        : 0;
    const int output_hscroll = m_output_table->horizontalScrollBar()
        ? m_output_table->horizontalScrollBar()->value()
        : 0;
    for (int row = 0; row < m_output_table->rowCount(); ++row) {
        auto *name_item = m_output_table->item(row, DColName);
        auto *assign = qobject_cast<QComboBox *>(m_output_table->cellWidget(row, DColAssignment));
        auto *requested = qobject_cast<QComboBox *>(m_output_table->cellWidget(row, DColRequested));
        auto *audio  = qobject_cast<QComboBox *>(m_output_table->cellWidget(row, DColAudio));
        auto *isolate = qobject_cast<QCheckBox *>(m_output_table->cellWidget(row, DColIsolate));
        if (!name_item || !assign || !requested || !audio || !isolate) continue;
        pending[name_item->data(Qt::UserRole).toString().toStdString()] = {
            assign->currentData().toString(),
            requested->currentIndex(),
            audio->currentIndex(),
            isolate->isChecked()
        };
    }

    const auto outputs = ZoomOutputManager::instance().outputs();
    const auto roster  = ZoomEngineClient::instance().roster();

    // Rebuild participant list for drag-and-drop
    if (m_participant_list) {
        const QString current_participant =
            m_participant_list->currentItem()
                ? m_participant_list->currentItem()->data(Qt::UserRole).toString()
                : QString();
        QStringList selected_participants;
        for (auto *item : m_participant_list->selectedItems())
            selected_participants << item->data(Qt::UserRole).toString();
        const int participant_vscroll = m_participant_list->verticalScrollBar()
            ? m_participant_list->verticalScrollBar()->value()
            : 0;
        const int participant_hscroll = m_participant_list->horizontalScrollBar()
            ? m_participant_list->horizontalScrollBar()->value()
            : 0;
        const QString filter = m_participant_filter
            ? m_participant_filter->text().trimmed().toLower() : QString();
        m_participant_list->clear();
        for (const auto &p : roster) {
            const QString name = p.display_name.empty()
                ? QString("ID %1").arg(p.user_id)
                : QString::fromStdString(p.display_name);
            if (!filter.isEmpty() && !name.toLower().contains(filter) &&
                !QString::number(p.user_id).contains(filter)) continue;
            auto *item = new QListWidgetItem(participant_roster_label(p));
            item->setData(Qt::UserRole, QString::number(p.user_id));
            // Store the name alongside the id: Zoom user ids are
            // meeting-scoped, so any by-name action (e.g. the talkback probe)
            // must resolve off this, never off Qt::UserRole's id -- an id
            // captured now points at nobody after a rejoin and at the wrong
            // person once ids get recycled.
            item->setData(Qt::UserRole + 1, name);
            m_participant_list->addItem(item);
            const QString id = item->data(Qt::UserRole).toString();
            if (selected_participants.contains(id))
                item->setSelected(true);
            if (id == current_participant)
                m_participant_list->setCurrentItem(item);
        }
        if (m_participant_list->verticalScrollBar())
            m_participant_list->verticalScrollBar()->setValue(participant_vscroll);
        if (m_participant_list->horizontalScrollBar())
            m_participant_list->horizontalScrollBar()->setValue(participant_hscroll);
    }

    // Clear stale preview callbacks before rebuilding rows
    ZoomOutputManager::instance().clear_all_preview_cbs();

    m_output_table->setRowCount(static_cast<int>(outputs.size()));
    for (int row = 0; row < static_cast<int>(outputs.size()); ++row) {
        const auto &output = outputs[row];
        m_output_table->setRowHeight(row, kRowH);

        // Preview thumbnail
        auto *thumb = new QLabel(m_output_table);
        thumb->setFixedSize(kThumbW, kThumbH);
        thumb->setAlignment(Qt::AlignCenter);
        thumb->setStyleSheet("background: #111118; color: #505068; font-size: 9px;");
        thumb->setText("no video");
        m_output_table->setCellWidget(row, DColPreview, thumb);

        auto alive_ref = m_alive;
        QPointer<QLabel> thumbPtr(thumb);
        ZoomOutputManager::instance().set_preview_cb(output.source_name,
            [thumbPtr, alive_ref](uint32_t w, uint32_t h,
                const uint8_t *y, const uint8_t *u, const uint8_t *v,
                uint32_t sy, uint32_t suv) {
                if (!alive_ref->load(std::memory_order_acquire)) return;
                QImage img = i420_to_qimage_dock(w, h, y, u, v, sy, suv);
                QMetaObject::invokeMethod(qApp, [thumbPtr, alive_ref, img]() {
                    if (!alive_ref->load(std::memory_order_acquire) || !thumbPtr) return;
                    thumbPtr->setPixmap(QPixmap::fromImage(img));
                    thumbPtr->setText({});
                }, Qt::QueuedConnection);
            });

        auto *name_item = new QTableWidgetItem(
            output.display_name.empty()
            ? QString::fromStdString(output.source_name)
            : QString::fromStdString(output.display_name));
        name_item->setFlags(name_item->flags() & ~Qt::ItemIsEditable);
        name_item->setData(Qt::UserRole, QString::fromStdString(output.source_name));
        m_output_table->setItem(row, DColName, name_item);

        auto *assignment = new QComboBox(m_output_table);
        assignment->addItem("Active speaker", "active");
        assignment->addItem("None", "user:0");
        // Spotlight slots (ZoomISO-style). CoreVideo targets up to 8 live feeds.
        for (int slot = 1; slot <= 8; ++slot)
            assignment->addItem(QString("Spotlight %1").arg(slot),
                                QString("spotlight:%1").arg(slot));
        assignment->addItem(screen_share_assignment_label(roster), "screenshare");

        // Apply the filter from the search box, but always keep the currently
        // selected item visible (otherwise the user would lose context).
        const QString filter = m_participant_filter
            ? m_participant_filter->text().trimmed().toLower()
            : QString();
        // Camera-off participants cannot go into an output at all, so they are
        // hidden when the operator asks for it. The currently assigned
        // participant is passed as keep_user_id and so survives even with their
        // camera off -- the re-add below is the second line of defence, not the
        // first, because losing the selection would unbind the output.
        const auto assignable = visible_for_video_assignment(
            roster,
            ZoomPluginSettings::load().hide_participants_without_video,
            output.participant_id);
        for (const auto &p : assignable) {
            if (!filter.isEmpty()) {
                const QString name = QString::fromStdString(p.display_name).toLower();
                const QString idstr = QString::number(p.user_id);
                if (!name.contains(filter) && !idstr.contains(filter))
                    continue;
            }
            assignment->addItem(participant_label(p), QString("user:%1").arg(p.user_id));
        }

        QString current;
        switch (output.assignment) {
        case AssignmentMode::ActiveSpeaker:  current = "active"; break;
        case AssignmentMode::SpotlightIndex: current = QString("spotlight:%1").arg(output.spotlight_slot); break;
        case AssignmentMode::ScreenShare:    current = "screenshare"; break;
        case AssignmentMode::Participant:
        default:
            current = output.active_speaker
                ? "active"
                : QString("user:%1").arg(output.participant_id);
            break;
        }
        // If the current selection got filtered out, re-add it so it's visible.
        if (assignment->findData(current) < 0 && current.startsWith("user:")) {
            const uint32_t pid = current.mid(5).toUInt();
            for (const auto &p : roster) {
                if (p.user_id == pid) {
                    assignment->addItem(participant_label(p), current);
                    break;
                }
            }
        }
        const int idx = assignment->findData(current);
        if (idx >= 0) assignment->setCurrentIndex(idx);
        m_output_table->setCellWidget(row, DColAssignment, assignment);

        auto *requested = new QComboBox(m_output_table);
        requested->addItem("360p", static_cast<int>(VideoResolution::P360));
        requested->addItem("720p", static_cast<int>(VideoResolution::P720));
        requested->addItem("1080p", static_cast<int>(VideoResolution::P1080));
        requested->setCurrentIndex(static_cast<int>(output.video_resolution));
        m_output_table->setCellWidget(row, DColRequested, requested);

        auto *signal_item = new QTableWidgetItem(signal_label(output));
        signal_item->setFlags(signal_item->flags() & ~Qt::ItemIsEditable);
        signal_item->setTextAlignment(Qt::AlignCenter);
        signal_item->setToolTip(signal_tooltip(output));
        if (output.health_reason == ZoomOutputHealthReason::DuplicateAssignment)
            signal_item->setForeground(QColor("#ff6b6b"));
        else if (output.health_reason == ZoomOutputHealthReason::ZoomDeliveredLowerResolution)
            signal_item->setForeground(QColor("#f0b429"));
        m_output_table->setItem(row, DColSignal, signal_item);

        auto *audio = new QComboBox(m_output_table);
        audio->addItem("Mono",   static_cast<int>(AudioChannelMode::Mono));
        audio->addItem("Stereo", static_cast<int>(AudioChannelMode::Stereo));
        audio->setCurrentIndex(output.audio_mode == AudioChannelMode::Stereo ? 1 : 0);
        m_output_table->setCellWidget(row, DColAudio, audio);

        auto *isolate = new QCheckBox(m_output_table);
        isolate->setChecked(output.isolate_audio);
        isolate->setToolTip("Use isolated audio for the assigned participant");
        m_output_table->setCellWidget(row, DColIsolate, isolate);

        // Restore any user pick that was in-flight before the rebuild. We only
        // restore values that are still selectable in the rebuilt widgets so
        // a stale snapshot can never resurrect a participant that just left.
        auto pit = pending.find(output.source_name);
        if (pit != pending.end()) {
            const int aidx = assignment->findData(pit->second.assignment);
            if (aidx >= 0) assignment->setCurrentIndex(aidx);
            if (pit->second.requested_idx >= 0 && pit->second.requested_idx < requested->count())
                requested->setCurrentIndex(pit->second.requested_idx);
            if (pit->second.audio_idx >= 0 && pit->second.audio_idx < audio->count())
                audio->setCurrentIndex(pit->second.audio_idx);
            isolate->setChecked(pit->second.isolate);
        }
    }
    if (m_output_table->verticalScrollBar())
        m_output_table->verticalScrollBar()->setValue(output_vscroll);
    if (m_output_table->horizontalScrollBar())
        m_output_table->horizontalScrollBar()->setValue(output_hscroll);
}

void ZoomDock::open_output_manager()
{
    extern void corevideo_show_output_manager_dock();
    corevideo_show_output_manager_dock();
}

void ZoomDock::refresh_output_signal_cells()
{
    if (!m_output_table) return;
    const auto outputs = ZoomOutputManager::instance().outputs();
    std::unordered_map<std::string, ZoomOutputInfo> by_source;
    for (const auto &output : outputs)
        by_source.emplace(output.source_name, output);

    for (int row = 0; row < m_output_table->rowCount(); ++row) {
        auto *name_item = m_output_table->item(row, DColName);
        auto *signal_item = m_output_table->item(row, DColSignal);
        if (!name_item || !signal_item) continue;

        const std::string source_name =
            name_item->data(Qt::UserRole).toString().toStdString();
        const auto it = by_source.find(source_name);
        if (it == by_source.end()) continue;

        signal_item->setText(signal_label(it->second));
        signal_item->setToolTip(signal_tooltip(it->second));
        signal_item->setForeground(it->second.health_reason == ZoomOutputHealthReason::DuplicateAssignment
            ? QColor("#ff6b6b")
            : it->second.health_reason == ZoomOutputHealthReason::ZoomDeliveredLowerResolution
                ? QColor("#f0b429")
                : QColor());
    }
}

void ZoomDock::apply_outputs()
{
    if (!m_output_table)
        return;

    std::unordered_map<uint32_t, QStringList> fixed_assignments;
    for (int row = 0; row < m_output_table->rowCount(); ++row) {
        auto *name_item = m_output_table->item(row, DColName);
        auto *assignment = qobject_cast<QComboBox *>(m_output_table->cellWidget(row, DColAssignment));
        if (!name_item || !assignment) continue;

        const QString ad = assignment->currentData().toString();
        if (!ad.startsWith("user:")) continue;
        const uint32_t participant_id = ad.mid(5).toUInt();
        if (participant_id == 0) continue;

        const QString output_name = name_item->text().trimmed().isEmpty()
            ? name_item->data(Qt::UserRole).toString()
            : name_item->text();
        fixed_assignments[participant_id] << output_name;
    }

    QStringList duplicate_lines;
    for (const auto &entry : fixed_assignments) {
        if (entry.second.size() > 1) {
            duplicate_lines << QString("Participant ID %1: %2")
                .arg(entry.first)
                .arg(entry.second.join(", "));
        }
    }
    if (!duplicate_lines.isEmpty()) {
        const auto choice = QMessageBox::warning(
            this, "Duplicate Participant Assignment",
            "One or more participants are assigned to multiple fixed outputs.\n\n"
            "This can waste Zoom subscriptions and make video quality negotiation less stable.\n\n"
            + duplicate_lines.join("\n") +
            "\n\nApply anyway?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (choice != QMessageBox::Yes)
            return;
    }

    for (int row = 0; row < m_output_table->rowCount(); ++row) {
        auto *name_item  = m_output_table->item(row, DColName);
        auto *assignment = qobject_cast<QComboBox *>(m_output_table->cellWidget(row, DColAssignment));
        auto *requested  = qobject_cast<QComboBox *>(m_output_table->cellWidget(row, DColRequested));
        auto *audio      = qobject_cast<QComboBox *>(m_output_table->cellWidget(row, DColAudio));
        auto *isolate    = qobject_cast<QCheckBox *>(m_output_table->cellWidget(row, DColIsolate));
        if (!name_item || !assignment || !requested || !audio || !isolate) continue;

        const std::string source_name = name_item->data(Qt::UserRole).toString().toStdString();
        const QString ad = assignment->currentData().toString();
        const auto video_resolution =
            static_cast<VideoResolution>(requested->currentData().toInt());
        const auto audio_mode = static_cast<AudioChannelMode>(audio->currentData().toInt());

        AssignmentMode mode = AssignmentMode::Participant;
        uint32_t participant_id = 0;
        uint32_t spotlight_slot = 1;

        if (ad == "active") {
            mode = AssignmentMode::ActiveSpeaker;
        } else if (ad == "screenshare") {
            mode = AssignmentMode::ScreenShare;
        } else if (ad.startsWith("spotlight:")) {
            mode = AssignmentMode::SpotlightIndex;
            spotlight_slot = ad.mid(10).toUInt();
        } else if (ad.startsWith("user:")) {
            participant_id = ad.mid(5).toUInt();
            mode = AssignmentMode::Participant;
        }

        // Preserve the existing failover by reading current output info.
        uint32_t failover = 0;
        bool audience_audio = false;
        for (const auto &o : ZoomOutputManager::instance().outputs()) {
            if (o.source_name == source_name) {
                failover = o.failover_participant_id;
                audience_audio = o.audience_audio;
                break;
            }
        }

        ZoomOutputManager::instance().configure_output_ex(
            source_name, mode, participant_id, spotlight_slot, failover,
            isolate->isChecked(), audio_mode, video_resolution, audience_audio);
    }
}

void ZoomDock::on_join_clicked()
{
    const QString raw_input = m_meeting_id->text().trimmed();
    if (raw_input.isEmpty()) return;
    if (m_join_in_progress.load(std::memory_order_acquire)) return;
    stop_pending_oauth_join();
    if (m_join_thread.joinable())
        m_join_thread.join();

    // Accept either a raw meeting ID or a Zoom URL; the parser strips out a
    // numeric meeting ID and an optional pwd= passcode from the URL.
    const auto parsed = zoom_join_utils::parse_join_input(raw_input.toStdString());
    if (parsed.meeting_id.empty()) {
        // Use dynamic property to trigger the [error="true"] QSS rule
        m_meeting_id->setProperty("error", true);
        m_meeting_id->style()->unpolish(m_meeting_id);
        m_meeting_id->style()->polish(m_meeting_id);
        m_meeting_id->setToolTip(
            "Could not parse a numeric meeting ID from this input. "
            "Enter the ID directly or paste a Zoom join URL.");
        return;
    }
    // Clear error state
    m_meeting_id->setProperty("error", false);
    m_meeting_id->style()->unpolish(m_meeting_id);
    m_meeting_id->style()->polish(m_meeting_id);

    // If the URL carried a passcode, prefer it over an empty UI field;
    // otherwise the user-entered passcode wins.
    std::string passcode = m_passcode->text().toStdString();
    if (passcode.empty()) passcode = parsed.passcode;

    std::string display_name = m_display_name->text().trimmed().toStdString();
    if (display_name.empty()) display_name = "OBS";

    ZoomJoinAuthTokens tokens;
    tokens.on_behalf_token = parsed.on_behalf_token;
    tokens.user_zak = parsed.user_zak;
    tokens.app_privilege_token = parsed.app_privilege_token;
    const std::string typed_token = m_join_token->text().trimmed().toStdString();
    const QString token_type = m_join_token_type
        ? m_join_token_type->currentData().toString()
        : QStringLiteral("auto_zak");
    const bool manual_token_mode = token_type != "auto_zak";
    if (manual_token_mode && typed_token.empty() &&
        tokens.on_behalf_token.empty() &&
        tokens.user_zak.empty() &&
        tokens.app_privilege_token.empty()) {
        QMessageBox::warning(this, "Zoom Join Token",
            "A manual token type was selected, but the token field is empty. "
            "Use Zoom sign-in, or paste the selected token before joining.");
        return;
    }
    if (!typed_token.empty()) {
        if (token_type == "user_zak") {
            tokens.user_zak = typed_token;
        } else if (token_type == "app_privilege_token") {
            tokens.app_privilege_token = typed_token;
        } else {
            tokens.on_behalf_token = typed_token;
        }
    }

    ZoomPluginSettings s = ZoomPluginSettings::load();

    // ── Single, auditable join decision path (issue #89) ────────────────────
    // Every branch below is derived from one pure planner so the auth mode,
    // ZAK plan, and sign-in requirement can be unit-tested and are logged as one
    // coherent block. See src/zoom-join-decision.h.
    zoom_join::JoinDecisionInputs decision_in;
    decision_in.oauth_client_id         = s.oauth_client_id;
    decision_in.oauth_authorization_url = s.oauth_authorization_url;
    decision_in.meeting_sdk_auth_mode   = s.meeting_sdk_auth_mode;
    decision_in.sdk_public_app_key      = s.sdk_public_app_key;
    decision_in.has_sdk_key_secret_pair = !s.sdk_key.empty() && !s.sdk_secret.empty();
    decision_in.has_embedded_jwt        = !s.jwt_token.empty();
    decision_in.engine_already_authed   =
        ZoomEngineClient::instance().is_authenticated();
    decision_in.has_access_token        = !s.oauth_access_token.empty();
    decision_in.has_refresh_token       = !s.oauth_refresh_token.empty();
    decision_in.access_token_expired    =
        s.oauth_expires_at <= QDateTime::currentSecsSinceEpoch();
    decision_in.token_type              = token_type.toStdString();
    // tokens.{user_zak,on_behalf_token} already reflect any typed/parsed value,
    // so map them directly and leave has_typed_token false to avoid double count.
    decision_in.has_parsed_zak          = !tokens.user_zak.empty();
    decision_in.has_parsed_on_behalf    = !tokens.on_behalf_token.empty();

    const zoom_join::JoinDecisionPlan plan = zoom_join::plan_join(decision_in);
    blog(LOG_INFO, "[obs-zoom-plugin] %s",
         zoom_join::format_join_decision_path(decision_in, plan).c_str());

    const std::string resolved_public_app_key = plan.resolved_public_app_key;
    const bool use_public_app_key = !resolved_public_app_key.empty();
    std::string jwt = use_public_app_key ? std::string() : s.resolved_jwt_token();

    if (plan.blocking_error == zoom_join::ZoomJoinError::MissingCredentials) {
        QMessageBox::warning(this, "Zoom Authentication",
            QString::fromUtf8(zoom_join::join_error_guidance(plan.blocking_error)));
        return;
    }

    if (plan.needs_oauth_sign_in) {
        QString error;
        if (!ZoomOAuthManager::instance().begin_authorization(this, &error)) {
            QMessageBox::warning(this, "Zoom Authentication",
                error.isEmpty()
                    ? QString::fromUtf8(zoom_join::join_error_guidance(
                          zoom_join::ZoomJoinError::NeedsSignIn))
                    : error);
            return;
        }
        start_pending_oauth_join();
        QMessageBox::information(this, "Zoom Authentication",
            "Zoom sign-in was opened in your browser. After authorization completes, CoreVideo will retry this join.");
        return;
    }

    m_join_started_ms = QDateTime::currentMSecsSinceEpoch();
    m_join_timeout_reported = false;

    const bool webinar = m_webinar_cb && m_webinar_cb->isChecked();
    const MeetingKind kind = webinar ? MeetingKind::Webinar : MeetingKind::Meeting;

    m_join_in_progress.store(true, std::memory_order_release);
    const uint64_t join_generation =
        m_join_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    ZoomEngineClient::instance().set_state(MeetingState::Joining);
    update_state_indicator();

    auto self = this;
    auto alive = m_alive;
    const std::string meeting_id = parsed.meeting_id;
    m_join_thread = std::thread([self, alive, jwt, meeting_id, passcode,
                                 display_name, kind, webinar, join_generation,
                                 tokens]() mutable {
        if (tokens.user_zak.empty() &&
            tokens.on_behalf_token.empty()) {
            std::string zak;
            QString zak_error;
            if (ZoomOAuthManager::instance().fetch_zak_blocking(zak, meeting_id, &zak_error)) {
                tokens.user_zak = zak;
                blog(LOG_INFO, "[obs-zoom-plugin] Zoom OAuth ZAK fetched for Meeting SDK join");
            } else {
                blog(LOG_WARNING, "[obs-zoom-plugin] Zoom OAuth ZAK fetch failed: %s",
                     zak_error.toUtf8().constData());
                QMetaObject::invokeMethod(self, [self, alive, zak_error, join_generation]() {
                    if (!alive->load(std::memory_order_acquire)) return;
                    if (self->m_join_generation.load(std::memory_order_acquire) != join_generation)
                        return;
                    self->m_join_in_progress.store(false, std::memory_order_release);
                    ZoomEngineClient::instance().set_state(MeetingState::Failed);
                    QMessageBox::warning(self, "Zoom Authentication",
                        zak_error.isEmpty()
                            ? QStringLiteral("Could not fetch Zoom ZAK. Sign in with Zoom and try again.")
                            : zak_error);
                    self->update_state_indicator();
                }, Qt::QueuedConnection);
                return;
            }
        }

        const ZoomPluginSettings settings = ZoomPluginSettings::load();
        std::string public_app_key =
            settings.resolved_meeting_sdk_public_app_key();
        if (!public_app_key.empty() && settings.use_broker_sdk_jwt()) {
            QString sdk_jwt_error;
            if (ZoomOAuthManager::instance().fetch_sdk_jwt_blocking(jwt, &sdk_jwt_error)) {
                public_app_key.clear();
            } else {
                blog(LOG_WARNING, "[obs-zoom-plugin] Meeting SDK JWT fetch failed: %s",
                     sdk_jwt_error.toUtf8().constData());
                QMetaObject::invokeMethod(self, [self, alive, sdk_jwt_error, join_generation]() {
                    if (!alive->load(std::memory_order_acquire)) return;
                    if (self->m_join_generation.load(std::memory_order_acquire) != join_generation)
                        return;
                    self->m_join_in_progress.store(false, std::memory_order_release);
                    ZoomEngineClient::instance().set_state(MeetingState::Failed);
                    QMessageBox::warning(self, "Zoom Authentication",
                        sdk_jwt_error.isEmpty()
                            ? QStringLiteral("Could not fetch Meeting SDK auth from CoreVideo broker.")
                            : sdk_jwt_error);
                    self->update_state_indicator();
                }, Qt::QueuedConnection);
                return;
            }
        }
        blog(LOG_INFO,
             "[obs-zoom-plugin] [join-decision] resolved sdk_auth_mode=%s jwt_present=%d public_app_key=%s zak_present=%d broker_jwt=%d "
             "(Meeting SDK result code follows from the engine as auth_ok/auth_fail)",
             public_app_key.empty()
                 ? (jwt.empty() ? "none" : "jwt")
                 : "public_app_key",
             jwt.empty() ? 0 : 1,
             zoom_join::redacted_tail(public_app_key).c_str(),
             tokens.user_zak.empty() ? 0 : 1,
             settings.use_broker_sdk_jwt() ? 1 : 0);
        const bool started = ZoomEngineClient::instance().start(jwt, public_app_key);
        const bool still_current =
            self->m_join_generation.load(std::memory_order_acquire) == join_generation;
        const bool joined = started && still_current &&
            ZoomEngineClient::instance().join(meeting_id, passcode,
                                              display_name, kind, tokens);

        QMetaObject::invokeMethod(self, [self, alive, started, joined, meeting_id,
                                         display_name, webinar, join_generation]() {
            if (!alive->load(std::memory_order_acquire)) return;
            if (self->m_join_generation.load(std::memory_order_acquire) != join_generation) {
                self->m_join_in_progress.store(false, std::memory_order_release);
                self->update_state_indicator();
                return;
            }

            self->m_join_in_progress.store(false, std::memory_order_release);
            if (!started) {
                ZoomEngineClient::instance().set_state(MeetingState::Failed);
                const std::string last_error =
                    ZoomEngineClient::instance().last_error();
                QString message =
                    "Could not start Zoom authentication. Check the OBS log for details.";
                if (!last_error.empty())
                    message += "\n\n" + QString::fromStdString(last_error);
                QMessageBox::warning(self, "Zoom Authentication",
                    message);
            } else if (!joined) {
                ZoomEngineClient::instance().set_state(MeetingState::Failed);
                QMessageBox::warning(self, "Zoom Join",
                    "Could not send the join request to Zoom. Check the OBS log for details.");
            } else {
                ZoomPluginSettings saved = ZoomPluginSettings::load();
                saved.last_meeting_id = meeting_id;
                saved.last_display_name = display_name;
                saved.last_was_webinar = webinar;
                saved.save();
            }
            self->update_state_indicator();
        }, Qt::QueuedConnection);
    });
}

void ZoomDock::on_leave_clicked()
{
    m_join_started_ms = 0;
    m_join_timeout_reported = false;
    m_join_generation.fetch_add(1, std::memory_order_acq_rel);
    blog(LOG_INFO, "[obs-zoom-plugin] Leave button clicked");
    ZoomEngineClient::instance().leave();
    update_state_indicator();
}

void ZoomDock::on_start_engine_clicked()
{
    if (ZoomEngineClient::instance().state() != MeetingState::InMeeting) {
        QMessageBox::information(this, "Broadcast Engine",
            "Join the Zoom meeting before starting the broadcast engine.");
        return;
    }
    ZoomEngineClient::instance().start_media();
    update_state_indicator();
}

void ZoomDock::on_stop_engine_clicked()
{
    ZoomEngineClient::instance().stop_media();
    update_state_indicator();
}

void ZoomDock::on_launch_sidecar_clicked()
{
    const QString sidecar_path = sidecar_executable_path();
    if (!QFileInfo::exists(sidecar_path)) {
        QMessageBox::warning(this, "Launch Sidecar",
            QString("CoreVideoSidecar.exe was not found.\n\nExpected path:\n%1")
                .arg(sidecar_path));
        return;
    }

    const QString working_dir = QFileInfo(sidecar_path).absolutePath();
    if (!QProcess::startDetached(sidecar_path,
                                 QStringList{QStringLiteral("--obs-autoconnect")},
                                 working_dir)) {
        QMessageBox::warning(this, "Launch Sidecar",
            QString("Could not launch CoreVideo Sidecar.\n\nPath:\n%1")
                .arg(sidecar_path));
    }
}

void ZoomDock::on_cancel_recovery_clicked()
{
    blog(LOG_INFO, "[obs-zoom-plugin] Cancel Recovery button clicked");
    ZoomEngineClient::instance().stop();
    update_state_indicator();
}
