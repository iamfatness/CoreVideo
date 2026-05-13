#include "zoom-dock.h"
#include "zoom-engine-client.h"
#include "zoom-output-manager.h"
#include "zoom-settings.h"
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>

// ── Column layout for the output table ───────────────────────────────────────
enum DockOutputColumns {
    DColName       = 0,
    DColAssignment = 1,
    DColAudio      = 2,
    DColIsolate    = 3,
    DColCount      = 4
};

// ── State dot colour ─────────────────────────────────────────────────────────
static const char *state_dot_style(MeetingState s)
{
    switch (s) {
    case MeetingState::InMeeting: return "color: #22cc44; font-size: 18px;";
    case MeetingState::Joining:
    case MeetingState::Leaving:   return "color: #f0a000; font-size: 18px;";
    case MeetingState::Failed:    return "color: #ee3333; font-size: 18px;";
    default:                      return "color: #666666; font-size: 18px;";
    }
}

static const char *state_label_text(MeetingState s)
{
    switch (s) {
    case MeetingState::Idle:      return "Not connected";
    case MeetingState::Joining:   return "Joining…";
    case MeetingState::InMeeting: return "In meeting";
    case MeetingState::Leaving:   return "Leaving…";
    case MeetingState::Failed:    return "Connection failed";
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

// ── Constructor ───────────────────────────────────────────────────────────────
ZoomDock::ZoomDock(QWidget *parent)
    : QDockWidget("Zoom Control", parent)
{
    setObjectName("ZoomControlDock");
    setMinimumWidth(340);

    auto *root    = new QWidget(this);
    auto *vLayout = new QVBoxLayout(root);
    vLayout->setContentsMargins(6, 6, 6, 6);
    vLayout->setSpacing(6);

    // ── Status row ────────────────────────────────────────────────────────────
    m_state_dot   = new QLabel("●", root);
    m_state_label = new QLabel("Not connected", root);
    m_state_dot->setStyleSheet("color: #666666; font-size: 18px;");

    auto *status_row = new QHBoxLayout;
    status_row->addWidget(m_state_dot);
    status_row->addWidget(m_state_label, 1);
    vLayout->addLayout(status_row);

    // ── Active speaker ────────────────────────────────────────────────────────
    m_speaker_label = new QLabel("—", root);
    m_speaker_label->setStyleSheet("color: #aaaaaa; font-style: italic;");

    auto *speaker_row = new QHBoxLayout;
    speaker_row->addWidget(new QLabel("Active speaker:", root));
    speaker_row->addWidget(m_speaker_label, 1);
    vLayout->addLayout(speaker_row);

    // ── Join controls ─────────────────────────────────────────────────────────
    auto *join_group  = new QGroupBox("Join Meeting", root);
    auto *join_layout = new QVBoxLayout(join_group);

    m_meeting_id = new QLineEdit(join_group);
    m_meeting_id->setPlaceholderText("Meeting ID");

    m_passcode = new QLineEdit(join_group);
    m_passcode->setPlaceholderText("Passcode (optional)");
    m_passcode->setEchoMode(QLineEdit::Password);

    m_join_btn  = new QPushButton("Join",  join_group);
    m_leave_btn = new QPushButton("Leave", join_group);
    m_leave_btn->setEnabled(false);

    auto *join_btn_row = new QHBoxLayout;
    join_btn_row->addWidget(m_join_btn);
    join_btn_row->addWidget(m_leave_btn);

    join_layout->addWidget(m_meeting_id);
    join_layout->addWidget(m_passcode);
    join_layout->addLayout(join_btn_row);
    vLayout->addWidget(join_group);

    connect(m_join_btn,  &QPushButton::clicked, this, [this]() { on_join_clicked(); });
    connect(m_leave_btn, &QPushButton::clicked, this, [this]() { on_leave_clicked(); });

    // ── Output table ──────────────────────────────────────────────────────────
    auto *output_group  = new QGroupBox("Outputs", root);
    auto *output_layout = new QVBoxLayout(output_group);

    m_output_table = new QTableWidget(output_group);
    m_output_table->setColumnCount(DColCount);
    m_output_table->setHorizontalHeaderLabels({"Output", "Assignment", "Audio", "Isolated"});
    m_output_table->horizontalHeader()->setSectionResizeMode(DColName,       QHeaderView::Stretch);
    m_output_table->horizontalHeader()->setSectionResizeMode(DColAssignment, QHeaderView::Stretch);
    m_output_table->horizontalHeader()->setSectionResizeMode(DColAudio,      QHeaderView::ResizeToContents);
    m_output_table->horizontalHeader()->setSectionResizeMode(DColIsolate,    QHeaderView::ResizeToContents);
    m_output_table->verticalHeader()->setVisible(false);
    m_output_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_output_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    m_apply_btn = new QPushButton("Apply", output_group);
    connect(m_apply_btn, &QPushButton::clicked, this, [this]() { apply_outputs(); });

    output_layout->addWidget(m_output_table);
    output_layout->addWidget(m_apply_btn);
    vLayout->addWidget(output_group, 1);

    setWidget(root);

    // ── Subscribe to roster / state changes ──────────────────────────────────
    auto self  = this;
    auto alive = m_alive;

    ZoomEngineClient::instance().add_roster_callback(this, [self, alive]() {
        QMetaObject::invokeMethod(self, [self, alive]() {
            if (alive->load(std::memory_order_acquire)) self->refresh();
        }, Qt::QueuedConnection);
    });

    refresh();
}

ZoomDock::~ZoomDock()
{
    m_alive->store(false, std::memory_order_release);
    ZoomEngineClient::instance().remove_roster_callback(this);
    ZoomOutputManager::instance().clear_all_preview_cbs();
}

// ── Internal helpers ──────────────────────────────────────────────────────────
void ZoomDock::update_state_indicator()
{
    const MeetingState s = ZoomEngineClient::instance().state();
    m_state_dot->setStyleSheet(state_dot_style(s));
    m_state_label->setText(state_label_text(s));

    const bool in_meeting = (s == MeetingState::InMeeting);
    const bool transitioning = (s == MeetingState::Joining || s == MeetingState::Leaving);
    m_join_btn->setEnabled(!in_meeting && !transitioning);
    m_leave_btn->setEnabled(in_meeting);

    // Active speaker name
    const uint32_t spk_id = ZoomEngineClient::instance().active_speaker_id();
    if (spk_id == 0) {
        m_speaker_label->setText("—");
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
}

void ZoomDock::refresh()
{
    update_state_indicator();
    refresh_outputs();
}

void ZoomDock::refresh_outputs()
{
    const auto outputs = ZoomOutputManager::instance().outputs();
    const auto roster  = ZoomEngineClient::instance().roster();

    m_output_table->setRowCount(static_cast<int>(outputs.size()));
    for (int row = 0; row < static_cast<int>(outputs.size()); ++row) {
        const auto &output = outputs[row];

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
        for (const auto &p : roster)
            assignment->addItem(participant_label(p), QString("user:%1").arg(p.user_id));
        const QString current = output.active_speaker
            ? "active"
            : QString("user:%1").arg(output.participant_id);
        const int idx = assignment->findData(current);
        if (idx >= 0) assignment->setCurrentIndex(idx);
        m_output_table->setCellWidget(row, DColAssignment, assignment);

        auto *audio = new QComboBox(m_output_table);
        audio->addItem("Mono",   static_cast<int>(AudioChannelMode::Mono));
        audio->addItem("Stereo", static_cast<int>(AudioChannelMode::Stereo));
        audio->setCurrentIndex(output.audio_mode == AudioChannelMode::Stereo ? 1 : 0);
        m_output_table->setCellWidget(row, DColAudio, audio);

        auto *isolate = new QCheckBox(m_output_table);
        isolate->setChecked(output.isolate_audio);
        isolate->setToolTip("Use isolated audio for the assigned participant");
        m_output_table->setCellWidget(row, DColIsolate, isolate);
    }
}

void ZoomDock::apply_outputs()
{
    for (int row = 0; row < m_output_table->rowCount(); ++row) {
        auto *name_item  = m_output_table->item(row, DColName);
        auto *assignment = qobject_cast<QComboBox *>(m_output_table->cellWidget(row, DColAssignment));
        auto *audio      = qobject_cast<QComboBox *>(m_output_table->cellWidget(row, DColAudio));
        auto *isolate    = qobject_cast<QCheckBox *>(m_output_table->cellWidget(row, DColIsolate));
        if (!name_item || !assignment || !audio || !isolate) continue;

        const std::string source_name = name_item->data(Qt::UserRole).toString().toStdString();
        const QString ad = assignment->currentData().toString();
        const bool active_speaker = (ad == "active");
        const uint32_t participant_id = ad.startsWith("user:") ? ad.mid(5).toUInt() : 0;
        const auto audio_mode = static_cast<AudioChannelMode>(audio->currentData().toInt());

        ZoomOutputManager::instance().configure_output(
            source_name, participant_id, active_speaker,
            isolate->isChecked(), audio_mode);
    }
}

void ZoomDock::on_join_clicked()
{
    const QString meeting_id = m_meeting_id->text().trimmed();
    if (meeting_id.isEmpty()) return;

    const ZoomPluginSettings s = ZoomPluginSettings::load();
    if (!ZoomEngineClient::instance().start(s.jwt_token)) return;

    ZoomEngineClient::instance().join(
        meeting_id.toStdString(),
        m_passcode->text().toStdString(),
        "OBS");

    update_state_indicator();
}

void ZoomDock::on_leave_clicked()
{
    ZoomEngineClient::instance().leave();
    update_state_indicator();
}
