#include "zoom-dock.h"
#include "obs-utils.h"
#include "zoom-engine-client.h"
#include "zoom-output-manager.h"
#include "zoom-reconnect.h"
#include "zoom-settings.h"
#include "zoom-settings-dialog.h"
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
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
    case MeetingState::InMeeting:  return "color: #22cc44; font-size: 18px;";
    case MeetingState::Joining:
    case MeetingState::Leaving:    return "color: #f0a000; font-size: 18px;";
    case MeetingState::Recovering: return "color: #f0a000; font-size: 18px;";
    case MeetingState::Failed:     return "color: #ee3333; font-size: 18px;";
    default:                       return "color: #666666; font-size: 18px;";
    }
}

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

// ── Constructor ───────────────────────────────────────────────────────────────
ZoomDock::ZoomDock(QWidget *parent)
    : QWidget(parent)
{
    setMinimumWidth(340);

    auto *vLayout = new QVBoxLayout(this);
    vLayout->setContentsMargins(6, 6, 6, 6);
    vLayout->setSpacing(6);

    // ── Status row ────────────────────────────────────────────────────────────
    m_state_dot   = new QLabel("●", this);
    m_state_label = new QLabel("Not connected", this);
    m_state_dot->setStyleSheet("color: #666666; font-size: 18px;");

    auto *status_row = new QHBoxLayout;
    status_row->addWidget(m_state_dot);
    status_row->addWidget(m_state_label, 1);
    vLayout->addLayout(status_row);

    // ── Active speaker ────────────────────────────────────────────────────────
    m_speaker_label = new QLabel("—", this);
    m_speaker_label->setStyleSheet("color: #aaaaaa; font-style: italic;");

    auto *speaker_row = new QHBoxLayout;
    speaker_row->addWidget(new QLabel("Active speaker:", this));
    speaker_row->addWidget(m_speaker_label, 1);
    vLayout->addLayout(speaker_row);

    // ── Join controls ─────────────────────────────────────────────────────────
    auto *join_group  = new QGroupBox("Join Meeting", this);
    auto *join_layout = new QVBoxLayout(join_group);

    m_meeting_id = new QLineEdit(join_group);
    m_meeting_id->setPlaceholderText("Meeting ID or Zoom URL");
    m_meeting_id->setToolTip(
        "Enter a numeric meeting ID or paste a full Zoom URL "
        "(e.g. https://zoom.us/j/123?pwd=abc) — the ID and passcode "
        "will be extracted automatically.");

    m_passcode = new QLineEdit(join_group);
    m_passcode->setPlaceholderText("Passcode (optional)");
    m_passcode->setEchoMode(QLineEdit::Password);

    m_display_name = new QLineEdit(join_group);
    m_display_name->setPlaceholderText("Display name");

    m_join_btn  = new QPushButton("Join",  join_group);
    m_leave_btn = new QPushButton("Leave", join_group);
    m_leave_btn->setEnabled(false);

    auto *join_btn_row = new QHBoxLayout;
    join_btn_row->addWidget(m_join_btn);
    join_btn_row->addWidget(m_leave_btn);

    m_webinar_cb = new QCheckBox("Join as Webinar / Zoom Events", join_group);

    join_layout->addWidget(m_meeting_id);
    join_layout->addWidget(m_passcode);
    join_layout->addWidget(m_display_name);
    join_layout->addWidget(m_webinar_cb);
    join_layout->addLayout(join_btn_row);
    vLayout->addWidget(join_group);

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

    // ── Recovery panel ────────────────────────────────────────────────────────
    m_recovery_label = new QLabel("", this);
    m_recovery_label->setStyleSheet("color: #f0a000;");
    m_recovery_label->setWordWrap(true);

    m_cancel_rec_btn = new QPushButton("Cancel Recovery", this);
    connect(m_cancel_rec_btn, &QPushButton::clicked,
            this, [this]() { on_cancel_recovery_clicked(); });

    auto *rec_row = new QHBoxLayout;
    rec_row->addWidget(m_recovery_label, 1);
    rec_row->addWidget(m_cancel_rec_btn);
    vLayout->addLayout(rec_row);

    m_countdown_timer = new QTimer(this);
    m_countdown_timer->setInterval(500);
    connect(m_countdown_timer, &QTimer::timeout, this, [this]() {
        update_recovery_panel();
    });

    m_refresh_timer = new QTimer(this);
    m_refresh_timer->setInterval(500);
    connect(m_refresh_timer, &QTimer::timeout, this, [this]() {
        refresh();
    });
    m_refresh_timer->start();

    // Hidden until recovery begins.
    m_recovery_label->setVisible(false);
    m_cancel_rec_btn->setVisible(false);

    // ── Output table ──────────────────────────────────────────────────────────
    auto *output_group  = new QGroupBox("Outputs", this);
    auto *output_layout = new QVBoxLayout(output_group);

    m_participant_filter = new QLineEdit(output_group);
    m_participant_filter->setPlaceholderText("Filter participants...");
    m_participant_filter->setClearButtonEnabled(true);
    connect(m_participant_filter, &QLineEdit::textChanged,
            this, [this](const QString &) { refresh_outputs(); });
    output_layout->addWidget(m_participant_filter);

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
    if (m_join_thread.joinable())
        m_join_thread.join();
    ZoomEngineClient::instance().remove_roster_callback(this);
    ZoomOutputManager::instance().clear_all_preview_cbs();
}

// ── Internal helpers ──────────────────────────────────────────────────────────
void ZoomDock::update_recovery_panel()
{
    const bool recovering = ZoomReconnectManager::instance().is_recovering();
    m_recovery_label->setVisible(recovering);
    m_cancel_rec_btn->setVisible(recovering);

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

    m_state_dot->setStyleSheet(state_dot_style(s));
    m_state_label->setText(state_label_text(s));

    const bool in_meeting    = (s == MeetingState::InMeeting);
    const bool recovering    = (s == MeetingState::Recovering);
    const bool join_task     = m_join_in_progress.load(std::memory_order_acquire);
    const bool transitioning = join_task || s == MeetingState::Joining ||
                               s == MeetingState::Leaving;
    m_join_btn->setEnabled(!in_meeting && !transitioning && !recovering);
    m_leave_btn->setEnabled(in_meeting || transitioning || recovering);
    m_leave_btn->setText(in_meeting ? "Leave" : "Cancel");

    update_recovery_panel();

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
        for (const auto &o : ZoomOutputManager::instance().outputs()) {
            if (o.source_name == source_name) {
                failover = o.failover_participant_id;
                break;
            }
        }

        ZoomOutputManager::instance().configure_output_ex(
            source_name, mode, participant_id, spotlight_slot, failover,
            isolate->isChecked(), audio_mode);
    }
}

void ZoomDock::on_join_clicked()
{
    const QString raw_input = m_meeting_id->text().trimmed();
    if (raw_input.isEmpty()) return;
    if (m_join_in_progress.load(std::memory_order_acquire)) return;
    if (m_join_thread.joinable())
        m_join_thread.join();

    // Accept either a raw meeting ID or a Zoom URL; the parser strips out a
    // numeric meeting ID and an optional pwd= passcode from the URL.
    const auto parsed = zoom_join_utils::parse_join_input(raw_input.toStdString());
    if (parsed.meeting_id.empty()) {
        m_meeting_id->setStyleSheet("border: 1px solid #ee3333;");
        m_meeting_id->setToolTip(
            "Could not parse a numeric meeting ID from this input. "
            "Enter the ID directly or paste a Zoom join URL.");
        return;
    }
    m_meeting_id->setStyleSheet(QString());

    // If the URL carried a passcode, prefer it over an empty UI field;
    // otherwise the user-entered passcode wins.
    std::string passcode = m_passcode->text().toStdString();
    if (passcode.empty()) passcode = parsed.passcode;

    std::string display_name = m_display_name->text().trimmed().toStdString();
    if (display_name.empty()) display_name = "OBS";

    ZoomPluginSettings s = ZoomPluginSettings::load();
    std::string jwt = s.resolved_jwt_token();
    if (!ZoomEngineClient::instance().is_authenticated() && jwt.empty()) {
        QMessageBox::information(this, "Zoom Authentication",
            "Enter your Zoom Meeting SDK credentials before joining.");
        ZoomSettingsDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted) return;
        s = ZoomPluginSettings::load();
        jwt = s.resolved_jwt_token();
    }

    if (!ZoomEngineClient::instance().is_authenticated() && jwt.empty()) {
        QMessageBox::warning(this, "Zoom Authentication",
            "Zoom Meeting SDK authentication is still missing. Enter an SDK key "
            "and secret, or a valid SDK JWT override, then try Join again.");
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
                                 display_name, kind, webinar, join_generation]() {
        const bool started = ZoomEngineClient::instance().start(jwt);
        const bool still_current =
            self->m_join_generation.load(std::memory_order_acquire) == join_generation;
        const bool joined = started && still_current &&
            ZoomEngineClient::instance().join(meeting_id, passcode,
                                              display_name, kind);

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
                QMessageBox::warning(self, "Zoom Authentication",
                    "Could not start Zoom authentication. Check the OBS log for details.");
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

void ZoomDock::on_cancel_recovery_clicked()
{
    ZoomReconnectManager::instance().cancel();
    update_state_indicator();
}
