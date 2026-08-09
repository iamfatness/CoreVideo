#include "zoom-output-dialog.h"
#include "cv-style.h"
#include "zoom-engine-client.h"
#include "zoom-output-manager.h"
#include "zoom-output-profile.h"
#include <QAbstractItemView>
#include <QApplication>
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
#include <QScrollBar>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

enum OutputColumns {
    ColumnPreview = 0,
    ColumnName,
    ColumnAssignment,
    ColumnResolution,
    ColumnDelivered,
    ColumnSdk,
    ColumnAudio,
    ColumnAudioRole,
    ColumnCount
};

// QTableWidget stretches a cell widget to fill the whole (tall) row cell. A
// widget that cannot grow (fixed-size preview label) ends up anchored to the
// top of the cell, while an unconstrained widget (a combo box) balloons to the
// full row height. Both break the row's shared vertical baseline. Wrapping the
// widget in a holder with a centering layout pins it to its natural size,
// vertically centered, so every cell in the row lines up.
static QWidget *center_in_cell(QWidget *inner, Qt::Alignment align, int h_margin)
{
    auto *holder = new QWidget(inner->parentWidget());
    auto *lay = new QHBoxLayout(holder);
    lay->setContentsMargins(h_margin, 0, h_margin, 0);
    // A horizontally-centered widget keeps its own width (stretch 0); otherwise
    // it fills the cell width (stretch 1) but is still vertically centered.
    const int stretch = (align & Qt::AlignHCenter) ? 0 : 1;
    lay->addWidget(inner, stretch, align);
    return holder;
}

// Combos live inside a center_in_cell() holder, so fetch the combo from the
// cell widget whether it is the widget itself or the holder's child.
static QComboBox *cell_combo(QTableWidget *table, int row, int col)
{
    QWidget *w = table->cellWidget(row, col);
    if (!w)
        return nullptr;
    if (auto *c = qobject_cast<QComboBox *>(w))
        return c;
    return w->findChild<QComboBox *>();
}

// Fast I420 -> RGB888 conversion for preview thumbnails.
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
    if (p.is_sharing_screen) label += " [sharing]";
    return label;
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

// Presentation order for Output Manager rows. Sources arrive in OBS load
// order (effectively random); operators expect Participant, Participant 2 …
// Participant 8, then Active Speaker, then Slots. Sort by a category rank
// then a natural number so "Participant 2" precedes "Participant 10".
static void output_sort_key(const ZoomOutputInfo &o, int &category, int &number,
                            QString &tiebreak)
{
    QString name = QString::fromStdString(
        o.source_name.empty() ? o.display_name : o.source_name).trimmed();
    if (name.startsWith(QLatin1String("CoreVideo "), Qt::CaseInsensitive))
        name = name.mid(10).trimmed();

    tiebreak = name.toLower();
    number = 0;
    // Trailing integer, if any (bare "Participant" sorts as 1).
    QString base = name;
    int i = name.size();
    while (i > 0 && name[i - 1].isDigit())
        --i;
    if (i < name.size()) {
        number = name.mid(i).toInt();
        base = name.left(i).trimmed();
    } else if (name.compare(QLatin1String("Participant"),
                            Qt::CaseInsensitive) == 0) {
        number = 1;
    }

    if (base.compare(QLatin1String("Participant"), Qt::CaseInsensitive) == 0)
        category = 0;
    else if (base.compare(QLatin1String("Active Speaker"),
                          Qt::CaseInsensitive) == 0)
        category = 1;
    else if (base.compare(QLatin1String("Slot"), Qt::CaseInsensitive) == 0)
        category = 2;
    else
        category = 3;
}

static bool output_sort_less(const ZoomOutputInfo &a, const ZoomOutputInfo &b)
{
    int ca, na, cb, nb;
    QString ta, tb;
    output_sort_key(a, ca, na, ta);
    output_sort_key(b, cb, nb, tb);
    if (ca != cb) return ca < cb;
    if (na != nb) return na < nb;
    return ta < tb;
}

static QString signal_text(const ZoomOutputInfo &output)
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
        return output.recovery_stalled
            ? QStringLiteral("! Can't\nsubscribe")
            : QStringLiteral("No signal");
    if (output.video_stale)
        return QString("! Stale\n%1.%2s")
            .arg(output.last_frame_age_ms / 1000)
            .arg((output.last_frame_age_ms / 100) % 10);
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
        return QStringLiteral("This output is assigned to active speaker, but no participant with video is currently talking. The source will wait on the last good frame or placeholder until a valid speaker is available.");
    if (output.health_reason == ZoomOutputHealthReason::SpotlightUnavailable)
        return QString("This output is assigned to spotlight slot %1, but no current participant owns that slot. Spotlight someone in Zoom or choose a different assignment.")
            .arg(output.spotlight_slot);
    if (output.observed_width == 0 || output.observed_height == 0)
        return output.subscribed_age_ms > 0
            ? QString("No live video frame has been received. CoreVideo has been waiting %1 ms and will retry automatically if the feed does not arrive.")
                .arg(output.subscribed_age_ms)
            : QStringLiteral("No live video frame has been received.");
    if (output.video_stale) {
        QString text = QString("Video frames stopped %1 ms ago. CoreVideo is preserving the last good frame and will retry this feed automatically.")
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
    QString text = QString("Requested %1x%2. Receiving %3x%4 at %5 fps.")
        .arg(video_resolution_width(output.video_resolution))
        .arg(video_resolution_height(output.video_resolution))
        .arg(output.observed_width)
        .arg(output.observed_height)
        .arg(output.observed_fps, 0, 'f', 2);
    if (output_signal_below_requested(output))
        text += QStringLiteral(" CoreVideo is using the best available Zoom feed.");
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

static QString operator_action_text(const ZoomOutputInfo &output)
{
    switch (output.health_reason) {
    case ZoomOutputHealthReason::RawMediaNotReady:
        return QStringLiteral("Start engine/media");
    case ZoomOutputHealthReason::DuplicateAssignment:
        return QStringLiteral("Reassign duplicate");
    case ZoomOutputHealthReason::ParticipantMissing:
        return QStringLiteral("Refresh roster or choose another participant");
    case ZoomOutputHealthReason::ParticipantVideoOff:
        return QStringLiteral("Ask participant to start video");
    case ZoomOutputHealthReason::WaitingForFirstFrame:
        return QStringLiteral("Wait for first frame or recover");
    case ZoomOutputHealthReason::StaleFrame:
        return QStringLiteral("Recover stale feed");
    case ZoomOutputHealthReason::ZoomDeliveredLowerResolution:
        return QStringLiteral("Retry quality upgrade");
    case ZoomOutputHealthReason::ScreenShareUnavailable:
        return QStringLiteral("Start screen share or reassign");
    case ZoomOutputHealthReason::ActiveSpeakerUnavailable:
        return QStringLiteral("Wait for a talking participant with video");
    case ZoomOutputHealthReason::SpotlightUnavailable:
        return QStringLiteral("Set spotlight slot or reassign");
    case ZoomOutputHealthReason::Ok:
    default:
        break;
    }
    if (output_signal_below_requested(output))
        return QStringLiteral("Retry quality upgrade");
    if (output_signal_missing_or_stale(output))
        return QStringLiteral("Recover stale feed");
    return QStringLiteral("No action needed");
}

static QString with_operator_action_tooltip(const QString &tooltip,
                                            const ZoomOutputInfo &output)
{
    return QString("%1\n\nRecommended action: %2")
        .arg(tooltip, operator_action_text(output));
}

static QString resolution_label_from_int(int resolution)
{
    switch (static_cast<VideoResolution>(resolution)) {
    case VideoResolution::P360: return QStringLiteral("360p");
    case VideoResolution::P1080: return QStringLiteral("1080p");
    case VideoResolution::P720:
    default: return QStringLiteral("720p");
    }
}

static QString sdk_quality_text(const ZoomOutputInfo &output)
{
    QStringList lines;
    if (output.negotiated_resolution >= 0) {
        lines << QString("Negotiated %1")
            .arg(resolution_label_from_int(output.negotiated_resolution));
    } else {
        lines << QStringLiteral("Negotiating");
    }

    if (output.last_video_subscribe_code >= 0) {
        lines << QString("Subscribe %1")
            .arg(output.last_video_subscribe_code == 0
                 ? QStringLiteral("OK")
                 : QString::number(output.last_video_subscribe_code));
    } else if (!output.last_quality_stage.empty()) {
        lines << QString::fromStdString(output.last_quality_stage);
    }

    if (output.subscription_downgraded ||
        output.health_reason == ZoomOutputHealthReason::ZoomDeliveredLowerResolution) {
        lines << QStringLiteral("Downgraded");
    } else if (output.quality_upgrade_cooldown_ms > 0) {
        lines << QString("Retry in %1s")
            .arg(output.quality_upgrade_cooldown_ms / 1000);
    }
    return lines.join(QStringLiteral("\n"));
}

static QString sdk_quality_tooltip(const ZoomOutputInfo &output)
{
    QString text = QString("Requested: %1. ")
        .arg(resolution_label_from_int(static_cast<int>(output.video_resolution)));
    text += output.negotiated_resolution >= 0
        ? QString("Negotiated: %1. ")
            .arg(resolution_label_from_int(output.negotiated_resolution))
        : QStringLiteral("Negotiated: unknown. ");

    if (output.last_set_resolution_code >= 0)
        text += QString("setRawDataResolution code: %1. ")
            .arg(output.last_set_resolution_code);
    if (output.last_video_subscribe_code >= 0)
        text += QString("subscribe code: %1. ")
            .arg(output.last_video_subscribe_code);
    if (output.last_raw_status >= 0)
        text += QString("raw status: %1. ").arg(output.last_raw_status);
    if (!output.last_quality_stage.empty()) {
        text += QString("Last SDK stage: %1 (%2 ms ago). ")
            .arg(QString::fromStdString(output.last_quality_stage))
            .arg(output.last_quality_event_age_ms);
    }
    if (output.subscription_downgraded)
        text += QStringLiteral("Zoom accepted the subscription but negotiated a lower raw feed than requested. ");
    if (output.quality_upgrade_attempts > 0)
        text += QString("Quality retry attempts: %1. ")
            .arg(output.quality_upgrade_attempts);
    if (output.quality_upgrade_cooldown_ms > 0)
        text += QString("Next automatic quality retry in %1 ms. ")
            .arg(output.quality_upgrade_cooldown_ms);
    if (output.observed_height >= 1080)
        text += QStringLiteral("Automatic quality retry is disabled because this feed is already 1080p.");
    return text.trimmed();
}

static QString output_summary_text(const std::vector<ZoomOutputInfo> &outputs,
                                   const std::vector<ParticipantInfo> &roster)
{
    int ok = 0;
    int warnings = 0;
    int stale_or_missing = 0;
    int lower_resolution = 0;
    int routing_unavailable = 0;
    int duplicate_assignments = 0;
    int recoverable = 0;
    int quality_retries = 0;
    int screen_share_outputs = 0;
    int video_on = 0;
    int sharing = 0;

    for (const auto &p : roster) {
        if (p.has_video)
            ++video_on;
        if (p.is_sharing_screen)
            ++sharing;
    }

    for (const auto &output : outputs) {
        if (output.assignment == AssignmentMode::ScreenShare)
            ++screen_share_outputs;
        if (output.health_reason == ZoomOutputHealthReason::Ok) {
            ++ok;
            continue;
        }
        ++warnings;
        if (output_signal_missing_or_stale(output))
            ++stale_or_missing;
        if (output.health_reason == ZoomOutputHealthReason::ZoomDeliveredLowerResolution)
            ++lower_resolution;
        if (output.health_reason == ZoomOutputHealthReason::DuplicateAssignment)
            ++duplicate_assignments;
        if (output_signal_missing_or_stale(output))
            ++recoverable;
        if (output.health_reason == ZoomOutputHealthReason::ZoomDeliveredLowerResolution ||
            output_signal_below_requested(output))
            ++quality_retries;
        if (output.health_reason == ZoomOutputHealthReason::ActiveSpeakerUnavailable ||
            output.health_reason == ZoomOutputHealthReason::SpotlightUnavailable ||
            output.health_reason == ZoomOutputHealthReason::ScreenShareUnavailable) {
            ++routing_unavailable;
        }
    }

    return QString("Outputs: %1  OK: %2  Warnings: %3  Stale/no signal: %4  Lower than requested: %5  Routing unavailable: %6  Duplicates: %7  Actions: Recover %8, Retry quality %9  Screen share outputs: %10  Participants with video: %11  Sharing: %12")
        .arg(outputs.size())
        .arg(ok)
        .arg(warnings)
        .arg(stale_or_missing)
        .arg(lower_resolution)
        .arg(routing_unavailable)
        .arg(duplicate_assignments)
        .arg(recoverable)
        .arg(quality_retries)
        .arg(screen_share_outputs)
        .arg(video_on)
        .arg(sharing);
}

static QString assignment_data_for_output(const ZoomOutputInfo &output)
{
    switch (output.assignment) {
    case AssignmentMode::ActiveSpeaker:
        return QStringLiteral("active");
    case AssignmentMode::ScreenShare:
        return QStringLiteral("screenshare");
    case AssignmentMode::SpotlightIndex:
        return QStringLiteral("spotlight:%1").arg(output.spotlight_slot);
    case AssignmentMode::Participant:
    default:
        return QStringLiteral("user:%1").arg(output.participant_id);
    }
}

// Selects `data` in the assignment combo, inserting a representative item
// if it is not present. A combo left at its default index silently displays
// "Active speaker" (index 0) for every unmatched assignment — and because
// refresh snapshots displayed values and re-applies them, that misdisplay
// became the real assignment one refresh later (all rows flipped to Active
// Speaker after a roster blip, 2026-08-09 report). The true value must
// always be representable, e.g. a participant who has left the roster.
static void select_assignment_value(QComboBox *combo, const QString &data)
{
    int idx = combo->findData(data);
    if (idx < 0) {
        QString label = data;
        if (data.startsWith(QStringLiteral("user:")))
            label = QString("Participant %1 (not in meeting)").arg(data.mid(5));
        combo->addItem(label, data);
        idx = combo->count() - 1;
    }
    combo->setCurrentIndex(idx);
}

// The periodic rebuild preserves only combos the user actually touched (or
// is mid-interaction with); everything else re-reads live source state, so
// changes made elsewhere — e.g. in the source's Properties dialog — appear
// in the Output Manager instead of being overwritten by a stale snapshot.
static void mark_dirty_on_user_change(QComboBox *combo)
{
    QObject::connect(combo, QOverload<int>::of(&QComboBox::activated), combo,
                     [combo](int) { combo->setProperty("user_dirty", true); });
}

static bool combo_has_user_edit(QComboBox *combo)
{
    if (!combo)
        return false;
    if (combo->property("user_dirty").toBool())
        return true;
    return combo->view() && combo->view()->isVisible();
}

static AssignmentMode assignment_mode_from_data(const QString &data,
                                                uint32_t &participant_id,
                                                uint32_t &spotlight_slot)
{
    participant_id = 0;
    spotlight_slot = 1;
    if (data == QStringLiteral("active"))
        return AssignmentMode::ActiveSpeaker;
    if (data == QStringLiteral("screenshare"))
        return AssignmentMode::ScreenShare;
    if (data.startsWith(QStringLiteral("spotlight:"))) {
        const uint slot = data.mid(10).toUInt();
        spotlight_slot = std::clamp<uint>(slot > 0 ? slot : 1, 1, 8);
        return AssignmentMode::SpotlightIndex;
    }
    if (data.startsWith(QStringLiteral("user:")))
        participant_id = data.mid(5).toUInt();
    return AssignmentMode::Participant;
}

static QString audio_role_data_for_output(const ZoomOutputInfo &output)
{
    if (output.isolate_audio)
        return QStringLiteral("isolate");
    if (output.audience_audio)
        return QStringLiteral("audience");
    return QStringLiteral("mix");
}

static void audio_route_flags_from_data(const QString &data,
                                        bool &isolate_audio,
                                        bool &audience_audio)
{
    isolate_audio = data == QStringLiteral("isolate");
    audience_audio = !isolate_audio && data == QStringLiteral("audience");
}

ZoomOutputDialog::ZoomOutputDialog(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle("Zoom Output Manager");
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumSize(1500, 840);
    resize(1600, 920);

    // Profile toolbar.
    m_profile_combo = new QComboBox(this);
    m_profile_combo->setMinimumWidth(180);
    m_profile_combo->setPlaceholderText("- select profile -");

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

    m_output_summary = new QLabel(this);
    m_output_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_output_summary->setWordWrap(true);

    m_participant_table = new QTableWidget(this);
    m_participant_table->setColumnCount(7);
    m_participant_table->setHorizontalHeaderLabels({
        "Participant", "ID", "Video", "Audio", "Talking", "Share", "Spotlight"
    });
    m_participant_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_participant_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_participant_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_participant_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_participant_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_participant_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_participant_table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_participant_table->verticalHeader()->setVisible(false);
    m_participant_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_participant_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_participant_table->setMinimumHeight(170);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels({
        "Preview", "Output", "Assignment", "Request", "Signal",
        "SDK Status", "Audio", "Audio Role"
    });
    m_table->horizontalHeader()->setMinimumSectionSize(104);
    m_table->horizontalHeader()->setSectionResizeMode(ColumnPreview,    QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(ColumnName,       QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColumnAssignment, QHeaderView::Interactive);
    m_table->horizontalHeader()->setSectionResizeMode(ColumnResolution, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(ColumnDelivered,  QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(ColumnSdk,        QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(ColumnAudio,      QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(ColumnAudioRole,  QHeaderView::Fixed);
    m_table->setColumnWidth(ColumnPreview, 162);
    m_table->setColumnWidth(ColumnName, 240);
    m_table->setColumnWidth(ColumnAssignment, 300);
    m_table->setColumnWidth(ColumnResolution, 124);
    m_table->setColumnWidth(ColumnDelivered, 168);
    m_table->setColumnWidth(ColumnSdk, 208);
    m_table->setColumnWidth(ColumnAudio, 148);
    m_table->setColumnWidth(ColumnAudioRole, 168);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(104);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setMinimumHeight(460);
    m_table->setMinimumWidth(1440);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply |
                                         QDialogButtonBox::Close, this);
    auto *refresh_button = buttons->addButton("Refresh", QDialogButtonBox::ActionRole);
    auto *recover_button = buttons->addButton("Recover Stale Feeds",
                                              QDialogButtonBox::ActionRole);
    auto *quality_button = buttons->addButton("Retry Quality Upgrade",
                                              QDialogButtonBox::ActionRole);

    if (auto *apply_btn = buttons->button(QDialogButtonBox::Apply))
        apply_btn->setProperty("role", "primary");

    connect(refresh_button, &QPushButton::clicked, this, [this]() { refresh(); });
    connect(recover_button, &QPushButton::clicked, this, [this]() {
        const uint32_t recovered =
            ZoomOutputManager::instance().recover_stale_sources(true);
        refresh();
        QMessageBox::information(
            this, "Recover Stale Feeds",
            QString("Recovery requested for %1 output%2.")
                .arg(recovered)
                .arg(recovered == 1 ? "" : "s"));
    });
    connect(quality_button, &QPushButton::clicked, this, [this]() {
        const uint32_t upgraded =
            ZoomOutputManager::instance().upgrade_low_quality_sources(true);
        refresh();
        QMessageBox::information(
            this, "Retry Quality Upgrade",
            QString("Quality upgrade requested for %1 output%2.")
                .arg(upgraded)
                .arg(upgraded == 1 ? "" : "s"));
    });
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, [this]() { apply(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QWidget::hide);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(profile_row);
    layout->addWidget(new QLabel("Participants", this));
    layout->addWidget(m_filter);
    layout->addWidget(m_participant_table);
    layout->addWidget(new QLabel("Outputs", this));
    layout->addWidget(m_output_summary);
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
    prepare_shutdown();
}

void ZoomOutputDialog::prepare_shutdown()
{
    if (m_alive)
        m_alive->store(false, std::memory_order_release);
    ZoomEngineClient::instance().remove_roster_callback(this);
    ZoomOutputManager::instance().clear_all_preview_cbs();
}

void ZoomOutputDialog::refresh_now()
{
    refresh();
}

bool ZoomOutputDialog::has_open_output_combo_popup() const
{
    if (!m_table) return false;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        for (const int col : {
                 ColumnAssignment, ColumnResolution, ColumnAudio, ColumnAudioRole
             }) {
            if (auto *combo = cell_combo(m_table, row, col)) {
                if (combo->view() && combo->view()->isVisible())
                    return true;
            }
        }
    }
    return false;
}

void ZoomOutputDialog::schedule_deferred_refresh()
{
    if (m_deferred_refresh_queued)
        return;
    m_deferred_refresh_queued = true;
    QTimer::singleShot(250, this, [this]() {
        m_deferred_refresh_queued = false;
        if (has_open_output_combo_popup()) {
            schedule_deferred_refresh();
            return;
        }
        refresh();
    });
}

void ZoomOutputDialog::refresh()
{
    if (has_open_output_combo_popup()) {
        schedule_deferred_refresh();
        return;
    }

    const int output_scroll =
        m_table && m_table->verticalScrollBar()
        ? m_table->verticalScrollBar()->value()
        : 0;
    const int participant_scroll =
        m_participant_table && m_participant_table->verticalScrollBar()
        ? m_participant_table->verticalScrollBar()->value()
        : 0;

    struct PendingPick {
        QVariant assignment;
        QVariant resolution;
        QVariant audio;
        QVariant audio_role;
    };
    std::unordered_map<std::string, PendingPick> pending;
    if (m_table) {
        for (int row = 0; row < m_table->rowCount(); ++row) {
            auto *name_item = m_table->item(row, ColumnName);
            auto *assignment = cell_combo(m_table, row, ColumnAssignment);
            auto *resolution = cell_combo(m_table, row, ColumnResolution);
            auto *audio = cell_combo(m_table, row, ColumnAudio);
            auto *audio_role = cell_combo(m_table, row, ColumnAudioRole);
            if (!name_item || !assignment || !resolution || !audio || !audio_role)
                continue;

            // Preserve only unapplied USER edits across the rebuild; an
            // invalid QVariant means "take live state". Snapshotting every
            // displayed value hid Properties-dialog changes forever and
            // once turned a transient misdisplay into a mass reassignment.
            PendingPick pick;
            if (combo_has_user_edit(assignment))
                pick.assignment = assignment->currentData();
            if (combo_has_user_edit(resolution))
                pick.resolution = resolution->currentData();
            if (combo_has_user_edit(audio))
                pick.audio = audio->currentData();
            if (combo_has_user_edit(audio_role))
                pick.audio_role = audio_role->currentData();
            if (pick.assignment.isValid() || pick.resolution.isValid() ||
                pick.audio.isValid() || pick.audio_role.isValid())
                pending[name_item->data(Qt::UserRole).toString()
                            .toStdString()] = pick;
        }
        m_table->setUpdatesEnabled(false);
    }
    if (m_participant_table) m_participant_table->setUpdatesEnabled(false);

    refresh_participants();

    // Clear any existing preview callbacks before rebuilding rows.
    ZoomOutputManager::instance().clear_all_preview_cbs();

    auto outputs = ZoomOutputManager::instance().outputs();
    std::sort(outputs.begin(), outputs.end(), output_sort_less);
    const std::vector<ParticipantInfo> roster = ZoomEngineClient::instance().roster();
    if (m_output_summary) {
        const bool has_warning = std::any_of(
            outputs.begin(), outputs.end(), [](const ZoomOutputInfo &output) {
                return output.health_reason != ZoomOutputHealthReason::Ok;
            });
        m_output_summary->setText(output_summary_text(outputs, roster));
        m_output_summary->setStyleSheet(has_warning
            ? "color: #f0b429; font-weight: 700;"
            : "color: #66d989; font-weight: 700;");
    }

    m_table->setRowCount(static_cast<int>(outputs.size()));
    for (int row = 0; row < static_cast<int>(outputs.size()); ++row) {
        m_table->setRowHeight(row, 104);
        const auto &output = outputs[row];

        // Preview thumbnail label
        auto *preview_label = new QLabel(m_table);
        preview_label->setFixedSize(160, 90);
        preview_label->setAlignment(Qt::AlignCenter);
        preview_label->setStyleSheet("background: #1a1a1a;");
        preview_label->setText("No video");
        m_table->setCellWidget(row, ColumnPreview,
            center_in_cell(preview_label, Qt::AlignCenter, 0));

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
        assignment->setMinimumWidth(280);
        // NOTE: "Active speaker" sits at index 0 — never leave a combo at
        // its default index when the real assignment can't be matched (see
        // select_assignment_value).
        assignment->addItem("Active speaker", "active");
        assignment->addItem(screen_share_assignment_label(roster), "screenshare");
        assignment->addItem("None", "user:0");
        for (int slot = 1; slot <= 8; ++slot) {
            assignment->addItem(QString("Spotlight %1").arg(slot),
                                QString("spotlight:%1").arg(slot));
        }
        for (const auto &p : roster)
            assignment->addItem(participant_label(p), QString("user:%1").arg(p.user_id));
        const QString current_assignment = assignment_data_for_output(output);
        select_assignment_value(assignment, current_assignment);
        mark_dirty_on_user_change(assignment);
        m_table->setCellWidget(row, ColumnAssignment,
            center_in_cell(assignment, Qt::AlignVCenter, 6));

        auto *audio = new QComboBox(m_table);
        audio->setMinimumWidth(112);
        audio->addItem("Mono", static_cast<int>(AudioChannelMode::Mono));
        audio->addItem("Stereo", static_cast<int>(AudioChannelMode::Stereo));
        audio->setCurrentIndex(output.audio_mode == AudioChannelMode::Stereo ? 1 : 0);
        mark_dirty_on_user_change(audio);

        auto *resolution = new QComboBox(m_table);
        resolution->setMinimumWidth(96);
        resolution->addItem("360p", static_cast<int>(VideoResolution::P360));
        resolution->addItem("720p", static_cast<int>(VideoResolution::P720));
        resolution->addItem("1080p", static_cast<int>(VideoResolution::P1080));
        resolution->setCurrentIndex(
            output.video_resolution == VideoResolution::P360 ? 0 :
            output.video_resolution == VideoResolution::P1080 ? 2 : 1);
        mark_dirty_on_user_change(resolution);
        m_table->setCellWidget(row, ColumnResolution,
            center_in_cell(resolution, Qt::AlignVCenter, 6));

        auto *signal = new QLabel(m_table);
        signal->setAlignment(Qt::AlignCenter);
        signal->setMinimumWidth(152);
        signal->setWordWrap(true);
        signal->setMargin(4);
        signal->setTextInteractionFlags(Qt::TextSelectableByMouse);
        signal->setText(signal_text(output));
        signal->setToolTip(with_operator_action_tooltip(signal_tooltip(output), output));
        if (output.health_reason == ZoomOutputHealthReason::DuplicateAssignment)
            signal->setStyleSheet("color: #ff6b6b; font-weight: 700;");
        else if (output.health_reason == ZoomOutputHealthReason::ZoomDeliveredLowerResolution ||
                 output.health_reason == ZoomOutputHealthReason::ActiveSpeakerUnavailable ||
                 output.health_reason == ZoomOutputHealthReason::SpotlightUnavailable ||
                 output.health_reason == ZoomOutputHealthReason::ScreenShareUnavailable)
            signal->setStyleSheet("color: #f0b429; font-weight: 700;");
        m_table->setCellWidget(row, ColumnDelivered, signal);

        auto *sdk = new QLabel(m_table);
        sdk->setAlignment(Qt::AlignCenter);
        sdk->setMinimumWidth(188);
        sdk->setWordWrap(true);
        sdk->setMargin(4);
        sdk->setTextInteractionFlags(Qt::TextSelectableByMouse);
        sdk->setText(sdk_quality_text(output));
        sdk->setToolTip(with_operator_action_tooltip(sdk_quality_tooltip(output), output));
        if (output.last_video_subscribe_code > 0)
            sdk->setStyleSheet("color: #ff6b6b; font-weight: 700;");
        else if (output.subscription_downgraded ||
                 output.health_reason == ZoomOutputHealthReason::ZoomDeliveredLowerResolution)
            sdk->setStyleSheet("color: #f0b429; font-weight: 700;");
        else if (output.last_video_subscribe_code == 0)
            sdk->setStyleSheet("color: #66d989; font-weight: 700;");
        m_table->setCellWidget(row, ColumnSdk, sdk);

        m_table->setCellWidget(row, ColumnAudio,
            center_in_cell(audio, Qt::AlignVCenter, 6));

        auto *audio_role = new QComboBox(m_table);
        audio_role->setMinimumWidth(132);
        audio_role->addItem("Mix", "mix");
        audio_role->addItem("Isolated", "isolate");
        audio_role->addItem("Audience", "audience");
        audio_role->setToolTip(
            "Mix uses the meeting mix. Isolated follows the assigned participant. "
            "Audience follows the residual active speaker not already isolated.");
        const int role_index =
            audio_role->findData(audio_role_data_for_output(output));
        if (role_index >= 0)
            audio_role->setCurrentIndex(role_index);
        mark_dirty_on_user_change(audio_role);
        m_table->setCellWidget(row, ColumnAudioRole,
            center_in_cell(audio_role, Qt::AlignVCenter, 6));

        const auto pit = pending.find(output.source_name);
        if (pit != pending.end()) {
            // Restore the user's unapplied edits and keep them marked dirty
            // so they continue to survive rebuilds until Apply.
            if (pit->second.assignment.isValid()) {
                select_assignment_value(assignment,
                                        pit->second.assignment.toString());
                assignment->setProperty("user_dirty", true);
            }
            int idx = pit->second.resolution.isValid()
                ? resolution->findData(pit->second.resolution) : -1;
            if (idx >= 0) {
                resolution->setCurrentIndex(idx);
                resolution->setProperty("user_dirty", true);
            }
            idx = pit->second.audio.isValid()
                ? audio->findData(pit->second.audio) : -1;
            if (idx >= 0) {
                audio->setCurrentIndex(idx);
                audio->setProperty("user_dirty", true);
            }
            idx = pit->second.audio_role.isValid()
                ? audio_role->findData(pit->second.audio_role) : -1;
            if (idx >= 0) {
                audio_role->setCurrentIndex(idx);
                audio_role->setProperty("user_dirty", true);
            }
        }
    }

    if (m_table) {
        m_table->setUpdatesEnabled(true);
        if (m_table->verticalScrollBar())
            m_table->verticalScrollBar()->setValue(output_scroll);
    }
    if (m_participant_table) {
        m_participant_table->setUpdatesEnabled(true);
        if (m_participant_table->verticalScrollBar())
            m_participant_table->verticalScrollBar()->setValue(participant_scroll);
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

        auto *video_item = new QTableWidgetItem(p.has_video ? "* On" : "Off");
        video_item->setForeground(p.has_video ? QColor("#22cc44") : QColor("#666666"));
        m_participant_table->setItem(row, 2, video_item);

        auto *audio_item = new QTableWidgetItem(p.is_muted ? "Muted" : "* Open");
        audio_item->setForeground(p.is_muted ? QColor("#f0a000") : QColor("#22cc44"));
        m_participant_table->setItem(row, 3, audio_item);

        auto *talking_item = new QTableWidgetItem(p.is_talking ? "*" : "");
        talking_item->setForeground(QColor("#22cc44"));
        talking_item->setTextAlignment(Qt::AlignCenter);
        m_participant_table->setItem(row, 4, talking_item);

        auto *share_item = new QTableWidgetItem(
            p.is_sharing_screen ? QStringLiteral("Sharing") : QStringLiteral("-"));
        share_item->setForeground(p.is_sharing_screen ? QColor("#22cc44") : QColor("#666666"));
        share_item->setTextAlignment(Qt::AlignCenter);
        m_participant_table->setItem(row, 5, share_item);

        auto *spotlight_item = new QTableWidgetItem(
            p.spotlight_index > 0
            ? QString::number(p.spotlight_index)
            : QStringLiteral("-"));
        spotlight_item->setForeground(p.spotlight_index > 0 ? QColor("#f0b429") : QColor("#666666"));
        spotlight_item->setTextAlignment(Qt::AlignCenter);
        m_participant_table->setItem(row, 6, spotlight_item);
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
    if (!ZoomOutputProfile::is_valid_name(name.trimmed().toStdString())) {
        QMessageBox::warning(
            this, "Save Profile",
            "Use a simple profile name up to 80 characters. Do not use path "
            "separators, reserved Windows device names, or characters like "
            "< > : \" / \\ | ? *.");
        return;
    }

    // Collect current table state as ZoomOutputInfo list
    std::vector<ZoomOutputInfo> outputs;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *name_item  = m_table->item(row, ColumnName);
        auto *assignment = cell_combo(m_table, row, ColumnAssignment);
        auto *resolution = cell_combo(m_table, row, ColumnResolution);
        auto *audio      = cell_combo(m_table, row, ColumnAudio);
        auto *audio_role = cell_combo(m_table, row, ColumnAudioRole);
        if (!name_item || !assignment || !resolution || !audio || !audio_role) continue;

        ZoomOutputInfo o;
        o.source_name    = name_item->data(Qt::UserRole).toString().toStdString();
        const QString ad = assignment->currentData().toString();
        uint32_t participant_id = 0;
        uint32_t spotlight_slot = 1;
        o.assignment = assignment_mode_from_data(ad, participant_id, spotlight_slot);
        o.active_speaker = (o.assignment == AssignmentMode::ActiveSpeaker);
        o.participant_id = participant_id;
        o.spotlight_slot = spotlight_slot;
        audio_route_flags_from_data(audio_role->currentData().toString(),
                                    o.isolate_audio,
                                    o.audience_audio);
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

    int matched = 0;
    int unmatched = 0;
    QStringList missing_sources;
    // Apply each entry in the profile to the matching output row
    for (const auto &o : profile) {
        bool found = false;
        for (int row = 0; row < m_table->rowCount(); ++row) {
            auto *name_item = m_table->item(row, ColumnName);
            if (!name_item) continue;
            if (name_item->data(Qt::UserRole).toString().toStdString() != o.source_name)
                continue;

            auto *assignment = cell_combo(m_table, row, ColumnAssignment);
            auto *resolution = cell_combo(m_table, row, ColumnResolution);
            auto *audio      = cell_combo(m_table, row, ColumnAudio);
            auto *audio_role = cell_combo(m_table, row, ColumnAudioRole);
            if (!assignment || !resolution || !audio || !audio_role) continue;

            // Profile values are staged, unapplied edits: select them
            // faithfully (inserting missing participants rather than
            // falling back to index 0) and mark them dirty so they
            // survive rebuilds until the operator hits Apply.
            select_assignment_value(assignment, assignment_data_for_output(o));
            assignment->setProperty("user_dirty", true);
            const int res_idx = resolution->findData(
                static_cast<int>(o.video_resolution));
            if (res_idx >= 0) resolution->setCurrentIndex(res_idx);
            resolution->setProperty("user_dirty", true);
            audio->setCurrentIndex(o.audio_mode == AudioChannelMode::Stereo ? 1 : 0);
            audio->setProperty("user_dirty", true);
            const int role_idx =
                audio_role->findData(audio_role_data_for_output(o));
            if (role_idx >= 0)
                audio_role->setCurrentIndex(role_idx);
            audio_role->setProperty("user_dirty", true);
            found = true;
            ++matched;
            break;
        }
        if (!found) {
            ++unmatched;
            missing_sources << QString::fromStdString(o.source_name);
        }
    }

    QString message = QString("Loaded profile '%1'.\n\nMatched outputs: %2")
        .arg(QString::fromStdString(name))
        .arg(matched);
    if (unmatched > 0) {
        message += QString("\nMissing current outputs: %1").arg(unmatched);
        message += QStringLiteral("\n\nProfile entries not found in this OBS scene/source set:\n");
        message += missing_sources.join(QStringLiteral("\n"));
    }
    QMessageBox::information(this, "Load Profile", message);
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
    std::unordered_map<uint32_t, QStringList> fixed_assignments;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *name_item = m_table->item(row, ColumnName);
        auto *assignment = cell_combo(m_table, row, ColumnAssignment);
        if (!name_item || !assignment) continue;

        const QString assignment_data = assignment->currentData().toString();
        uint32_t participant_id = 0;
        uint32_t spotlight_slot = 1;
        const AssignmentMode assignment_mode =
            assignment_mode_from_data(assignment_data, participant_id, spotlight_slot);
        if (assignment_mode == AssignmentMode::Participant && participant_id != 0) {
            const QString output_name = name_item->text().trimmed().isEmpty()
                ? name_item->data(Qt::UserRole).toString()
                : name_item->text();
            fixed_assignments[participant_id] << output_name;
        }
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

    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *name_item = m_table->item(row, ColumnName);
        auto *assignment = cell_combo(m_table, row, ColumnAssignment);
        auto *resolution = cell_combo(m_table, row, ColumnResolution);
        auto *audio = cell_combo(m_table, row, ColumnAudio);
        auto *audio_role = cell_combo(m_table, row, ColumnAudioRole);
        if (!name_item || !assignment || !resolution || !audio || !audio_role) continue;

        const std::string source_name =
            name_item->data(Qt::UserRole).toString().toStdString();
        const QString assignment_data = assignment->currentData().toString();
        uint32_t participant_id = 0;
        uint32_t spotlight_slot = 1;
        const AssignmentMode assignment_mode =
            assignment_mode_from_data(assignment_data, participant_id, spotlight_slot);
        const auto audio_mode = static_cast<AudioChannelMode>(
            audio->currentData().toInt());
        const auto video_resolution = static_cast<VideoResolution>(
            resolution->currentData().toInt());

        bool isolate_audio = false;
        bool audience_audio = false;
        audio_route_flags_from_data(audio_role->currentData().toString(),
                                    isolate_audio,
                                    audience_audio);

        ZoomOutputManager::instance().configure_output_ex(
            source_name, assignment_mode, participant_id, spotlight_slot, 0,
            isolate_audio, audio_mode, video_resolution, audience_audio);
    }
    refresh();
}
