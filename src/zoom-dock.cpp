#include "zoom-dock.h"
#include "cv-onboarding.h"
#include "cv-style.h"
#include "cv-widgets.h"
#include "obs-utils.h"
#include "speaker-director.h"
#include "zoom-engine-client.h"
#include "zoom-oauth.h"
#include "zoom-output-manager.h"
#include "zoom-output-dialog.h"
#include "zoom-reconnect.h"
#include "zoom-settings.h"
#include "zoom-settings-dialog.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QColor>
#include <QDateTime>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
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
#include <QStringList>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
#include <algorithm>
#include <unordered_map>
#if defined(_WIN32)
#include <windows.h>
#endif

// â”€â”€ Column layout for the output table â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

static std::string redacted_tail(const std::string &value)
{
    if (value.empty()) return "empty";
    if (value.size() <= 4) return "****";
    return "****" + value.substr(value.size() - 4);
}

// Fast I420 â†’ RGB888 for dock thumbnails (shared with output dialog logic)
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

// â”€â”€ Draggable participant list â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

// â”€â”€ Drop-enabled output table â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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
    case MeetingState::Joining:    return "Joiningâ€¦";
    case MeetingState::InMeeting:  return "In meeting";
    case MeetingState::Leaving:    return "Leavingâ€¦";
    case MeetingState::Recovering: return "Recoveringâ€¦";
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

static QString signal_label(const ZoomOutputInfo &output)
{
    if (output.observed_width == 0 || output.observed_height == 0)
        return QStringLiteral("Waiting");
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
    if (output.observed_width == 0 || output.observed_height == 0)
        return QStringLiteral("No video frame has been received for this output yet.");

    QString text = QString("Live signal: %1x%2 at %3 fps. Requested: %4x%5.")
        .arg(output.observed_width)
        .arg(output.observed_height)
        .arg(output.observed_fps, 0, 'f', 2)
        .arg(video_resolution_width(output.video_resolution))
        .arg(video_resolution_height(output.video_resolution));
    if (output_signal_below_requested(output)) {
        text += QStringLiteral(" Zoom delivered a lower feed, so CoreVideo is using the best available live feed.");
    }
    return text;
}

// â”€â”€ Constructor â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
ZoomDock::ZoomDock(QWidget *parent)
    : QWidget(parent)
{
    const ZoomPluginSettings initial_settings = ZoomPluginSettings::load();
    setMinimumWidth(760);

    auto *vLayout = new QVBoxLayout(this);
    vLayout->setContentsMargins(8, 8, 8, 8);
    vLayout->setSpacing(6);

    // â”€â”€ Status row â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

    // â”€â”€ Active speaker â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_speaker_label = new QLabel(QStringLiteral("â€”"), this);
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
    speaker_grid->addWidget(new QLabel("Directed:", speaker_group), 0, 0);
    speaker_grid->addWidget(m_director_speaker_label, 0, 1);
    speaker_grid->addWidget(new QLabel("Raw:", speaker_group), 0, 2);
    speaker_grid->addWidget(m_raw_speaker_label, 0, 3);
    speaker_grid->addWidget(new QLabel("Candidate:", speaker_group), 1, 0);
    speaker_grid->addWidget(m_candidate_speaker_label, 1, 1);
    speaker_grid->addWidget(new QLabel("Last:", speaker_group), 1, 2);
    speaker_grid->addWidget(m_last_speaker_label, 1, 3);
    speaker_layout->addLayout(speaker_grid);

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
    speaker_controls->addWidget(new QLabel("Sensitivity", speaker_group));
    speaker_controls->addWidget(m_speaker_sensitivity_spin);
    speaker_controls->addWidget(new QLabel("Hold", speaker_group));
    speaker_controls->addWidget(m_speaker_hold_spin);
    speaker_controls->addStretch(1);
    speaker_layout->addLayout(speaker_controls);
    connect(m_speaker_sensitivity_spin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int value) {
                auto s = ZoomPluginSettings::load();
                s.speaker_sensitivity_ms = static_cast<uint32_t>(value);
                if (m_speaker_hold_spin)
                    s.speaker_hold_ms = static_cast<uint32_t>(m_speaker_hold_spin->value());
                s.save();
                update_state_indicator();
            });
    connect(m_speaker_hold_spin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int value) {
                auto s = ZoomPluginSettings::load();
                if (m_speaker_sensitivity_spin)
                    s.speaker_sensitivity_ms = static_cast<uint32_t>(
                        m_speaker_sensitivity_spin->value());
                s.speaker_hold_ms = static_cast<uint32_t>(value);
                s.save();
                update_state_indicator();
            });

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
    // â”€â”€ Credentials notice (hidden once credentials are set) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_credentials_banner = new CvBanner(
        CvBannerKind::Info,
        "SDK credentials required to join meetings.",
        this);
    m_credentials_banner->setActionText("Open Settings");
    connect(m_credentials_banner, &CvBanner::actionClicked, this, [this]() {
        ZoomSettingsDialog dlg(this);
        dlg.exec();
        update_credentials_banner();
    });
    vLayout->addWidget(m_credentials_banner);

    // â”€â”€ Join controls â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto *join_group  = new QGroupBox("Join Meeting", this);
    auto *join_layout = new QVBoxLayout(join_group);
    join_layout->setSpacing(6);

    m_meeting_id = new QLineEdit(join_group);
    m_meeting_id->setPlaceholderText("Meeting ID or Zoom URL");
    m_meeting_id->setToolTip(
        "Enter a numeric meeting ID or paste a full Zoom URL "
        "(e.g. https://zoom.us/j/123?pwd=abc) â€” the ID and passcode "
        "will be extracted automatically.");

    m_passcode = new QLineEdit(join_group);
    m_passcode->setPlaceholderText("Passcode (optional)");
    m_passcode->setEchoMode(QLineEdit::Password);

    m_display_name = new QLineEdit(join_group);
    m_display_name->setPlaceholderText("Display name");

    m_join_token_type = new QComboBox(join_group);
    m_join_token_type->addItem("Auto Zoom sign-in / ZAK", "auto_zak");
    m_join_token_type->addItem("User ZAK", "user_zak");
    m_join_token_type->addItem("App privilege token", "app_privilege_token");
    m_join_token_type->setToolTip(
        "Use Auto. App privilege tokens are only for raw media permission; "
        "CoreVideo will still use Zoom sign-in/ZAK for the meeting join.");

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

    // Role-based styling â€” evaluated when stylesheet is applied below
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
    m_launch_sidecar_btn = new QPushButton("Launch Sidecar", engine_group);
    m_start_engine_btn->setProperty("role", "primary");
    m_stop_engine_btn->setProperty("role", "danger");
    m_start_engine_btn->setToolTip(
        "Start Zoom raw media capture and send participant video/audio to OBS outputs.");
    m_stop_engine_btn->setToolTip(
        "Stop Zoom raw media capture while staying joined to the meeting.");
    m_launch_sidecar_btn->setToolTip(
        "Open the CoreVideo Sidecar production console and connect it to OBS.");
    m_stop_engine_btn->setEnabled(false);
    engine_layout->addWidget(m_start_engine_btn);
    engine_layout->addWidget(m_stop_engine_btn);
    engine_layout->addWidget(m_launch_sidecar_btn);
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
    connect(m_launch_sidecar_btn, &QPushButton::clicked,
            this, [this]() { on_launch_sidecar_clicked(); });
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

    // â”€â”€ Recovery panel â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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
    m_refresh_timer = new QTimer(this);
    m_refresh_timer->setInterval(100);
    connect(m_refresh_timer, &QTimer::timeout, this, [this]() {
        if (SpeakerDirector::instance().tick(os_gettime_ns() / 1000000ULL))
            ZoomOutputManager::instance().resubscribe_all();
        update_state_indicator();
    });
    m_refresh_timer->start();

    m_recovery_frame->setVisible(false);

    // â”€â”€ Subscribe to roster / state changes â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

    // â”€â”€ Apply stylesheet last so all properties are set before evaluation â”€â”€â”€â”€â”€
    setStyleSheet(cv_stylesheet());

    update_credentials_banner();
    refresh();

    // â”€â”€ First-run onboarding wizard â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (!CvOnboardingWizard::isCompleted()) {
        const ZoomPluginSettings &cfg = ZoomPluginSettings::load();
        const bool noCredentials = cfg.sdk_key.empty()
            && cfg.sdk_secret.empty()
            && cfg.jwt_token.empty();
        if (noCredentials) {
            QTimer::singleShot(0, this, [this]() {
                CvOnboardingWizard wiz(this);
                wiz.exec();
                update_credentials_banner();
            });
        }
    }
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
    ZoomEngineClient::instance().remove_roster_callback(this);
    ZoomEngineClient::instance().remove_error_callback(this);
    m_alive->store(false, std::memory_order_release);
    stop_pending_oauth_join();
    if (m_countdown_timer)
        m_countdown_timer->stop();
    if (m_refresh_timer)
        m_refresh_timer->stop();
}

// â”€â”€ Internal helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
void ZoomDock::update_credentials_banner()
{
    if (!m_alive->load(std::memory_order_acquire))
        return;
    const ZoomPluginSettings s = ZoomPluginSettings::load();
    const bool missing = s.sdk_key.empty() && s.sdk_secret.empty() && s.jwt_token.empty();
    m_credentials_banner->setVisible(missing);
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
            QString("Reconnecting in %1sâ€¦ (attempt %2/%3)")
                .arg((ms_left + 999) / 1000)
                .arg(attempt + 1)
                .arg(max_att));
    } else {
        m_recovery_label->setText(
            QString("Reconnectingâ€¦ (attempt %1/%2)")
                .arg(attempt)
                .arg(max_att));
    }

    if (!m_countdown_timer->isActive())
        m_countdown_timer->start();
}

void ZoomDock::update_state_indicator()
{
    const MeetingState s = ZoomEngineClient::instance().state();

    if (s == MeetingState::Joining && m_join_started_ms > 0 &&
        !m_join_timeout_reported &&
        QDateTime::currentMSecsSinceEpoch() - m_join_started_ms > 120000) {
        m_join_timeout_reported = true;
        ZoomEngineClient::instance().leave();
        ZoomEngineClient::instance().set_state(MeetingState::Failed);
        QMessageBox::warning(this, "Zoom Join",
            "Zoom did not finish joining within two minutes. The join attempt "
            "was cancelled so you can try again.");
    }

    if (s != MeetingState::Joining) {
        m_join_started_ms = 0;
        m_join_timeout_reported = false;
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
        m_speaker_label->setText(QStringLiteral("â€”"));
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

    if (m_speaker_sensitivity_spin && m_speaker_hold_spin) {
        SpeakerDirector::instance().configure(
            static_cast<uint32_t>(m_speaker_sensitivity_spin->value()),
            static_cast<uint32_t>(m_speaker_hold_spin->value()),
            true);
    }
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
        if (m_speaker_override_combo && !m_speaker_override_combo->view()->isVisible()) {
            const QVariant current = m_speaker_override_combo->currentData();
            m_speaker_override_combo->blockSignals(true);
            m_speaker_override_combo->clear();
            m_speaker_override_combo->addItem("Select participant", 0);
            for (const auto &p : roster) {
                if (p.user_id == 0 || p.is_muted || !p.has_video)
                    continue;
                m_speaker_override_combo->addItem(participant_label(p), p.user_id);
            }
            const int idx = m_speaker_override_combo->findData(current);
            if (idx >= 0)
                m_speaker_override_combo->setCurrentIndex(idx);
            else if (director.manual_speaker_id != 0) {
                const int midx = m_speaker_override_combo->findData(director.manual_speaker_id);
                if (midx >= 0)
                    m_speaker_override_combo->setCurrentIndex(midx);
            }
            m_speaker_override_combo->blockSignals(false);
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
        m_speaker_label->setToolTip(
            QString("Hold remaining: %1 ms").arg(
                static_cast<qulonglong>(director.hold_remaining_ms)));
        m_director_speaker_label->setToolTip(m_speaker_label->toolTip());
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
    // If a dropdown popup is open, the user is mid-selection â€” rebuilding the
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
            m_participant_list->addItem(item);
        }
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
        // Spotlight slots (ZoomISO-style)
        for (int slot = 1; slot <= 4; ++slot)
            assignment->addItem(QString("Spotlight %1").arg(slot),
                                QString("spotlight:%1").arg(slot));
        assignment->addItem("Screen share", "screenshare");

        // Apply the filter from the search box, but always keep the currently
        // selected item visible (otherwise the user would lose context).
        const QString filter = m_participant_filter
            ? m_participant_filter->text().trimmed().toLower()
            : QString();
        for (const auto &p : roster) {
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
        if (output_signal_below_requested(output))
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
}

void ZoomDock::open_output_manager()
{
    auto *main_win = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    QWidget *parent = main_win ? static_cast<QWidget *>(main_win) : this;
    ZoomOutputDialog dlg(parent);
    dlg.exec();
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
        signal_item->setForeground(output_signal_below_requested(it->second)
            ? QColor("#f0b429")
            : QColor());
    }
}

void ZoomDock::apply_outputs()
{
    if (!m_output_table)
        return;
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
            "Use Auto Zoom sign-in / ZAK, or paste the selected token before joining.");
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
    std::string jwt = s.resolved_jwt_token();
    if (!ZoomEngineClient::instance().is_authenticated() && jwt.empty()) {
        QMessageBox::warning(this, "Zoom Authentication",
            "This CoreVideo build does not have Zoom Meeting SDK credentials "
            "configured. Rebuild with embedded SDK credentials or restore a "
            "valid SDK JWT before joining.");
        return;
    }

    const bool needs_oauth_zak =
        tokens.user_zak.empty() &&
        tokens.on_behalf_token.empty();
    blog(LOG_INFO,
         "[obs-zoom-plugin] Zoom join token mode=%s zak_needed=%d typed_token=%d parsed_obf=%d parsed_zak=%d parsed_app=%d app_privilege_present=%d sdk_key=%s oauth_client_id=%s oauth_scopes=\"%s\"",
         token_type.toUtf8().constData(),
         needs_oauth_zak ? 1 : 0,
         typed_token.empty() ? 0 : 1,
         parsed.on_behalf_token.empty() ? 0 : 1,
         parsed.user_zak.empty() ? 0 : 1,
         parsed.app_privilege_token.empty() ? 0 : 1,
         tokens.app_privilege_token.empty() ? 0 : 1,
         redacted_tail(s.sdk_key).c_str(),
         redacted_tail(s.oauth_client_id).c_str(),
         s.oauth_scopes.c_str());
    if (needs_oauth_zak &&
        s.oauth_access_token.empty() &&
        s.oauth_refresh_token.empty()) {
        QString error;
        if (!ZoomOAuthManager::instance().begin_authorization(this, &error)) {
            QMessageBox::warning(this, "Zoom Authentication",
                error.isEmpty()
                    ? QStringLiteral("Sign in with Zoom before joining meetings that require owner/host context.")
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

        const bool started = ZoomEngineClient::instance().start(jwt);
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
    ZoomEngineClient::instance().stop();
    update_state_indicator();
}
