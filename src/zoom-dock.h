#pragma once

#include "talkback-dock-state.h"

#include <QFrame>
#include <QString>
#include <QWidget>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QTimer;
class CvStatusDot;
class CvBanner;

class ZoomDock : public QWidget {
    Q_OBJECT
public:
    explicit ZoomDock(QWidget *parent = nullptr);
    ~ZoomDock() override;
    void prepare_shutdown();

private:
    void refresh();
    void refresh_outputs();
    void refresh_output_signal_cells();
    void apply_outputs();
    void open_output_manager();

    void on_join_clicked();
    void on_leave_clicked();
    void on_start_engine_clicked();
    void on_stop_engine_clicked();
    void on_launch_sidecar_clicked();
    void on_cancel_recovery_clicked();
    void update_state_indicator();
    // Milestone 7. Everything the Talkback group polls, driven from the
    // existing 100ms refresh tick -- deliberately NOT a second timer: this
    // dock already has three (refresh, health retry, countdown) and the
    // engine-confirmed talkback state is polled exactly the way
    // last_error()/roster()/talkback_probe_status() already are.
    void refresh_talkback();
    void rebuild_talkback_key_buttons(
        const std::vector<TalkbackDockKeyButton> &buttons);
    void on_talkback_nominate_clicked();
    // `latch` is captured at PRESS time, not read at release: an operator who
    // toggles the Latch box while holding a button must not have the release
    // reinterpreted underneath them.
    void talkback_key_pressed(const std::string &target, bool latch);
    void talkback_key_released(const std::string &target);
    void apply_speaker_director_settings();
    void update_recovery_panel();
    void update_credentials_banner();
    void show_update_banner(const QString &tag, const QString &html_url);
    void start_pending_oauth_join();
    void stop_pending_oauth_join();

    // Status bar
    CvStatusDot *m_state_dot   = nullptr;
    QLabel      *m_state_label = nullptr;
    QLabel      *m_error_label = nullptr;

    // Active speaker
    QLabel      *m_speaker_label = nullptr;
    QLabel      *m_director_speaker_label = nullptr;
    QLabel      *m_raw_speaker_label = nullptr;
    QLabel      *m_candidate_speaker_label = nullptr;
    QLabel      *m_last_speaker_label = nullptr;
    QLabel      *m_speaker_status_label = nullptr;
    QSpinBox    *m_speaker_sensitivity_spin = nullptr;
    QSpinBox    *m_speaker_hold_spin = nullptr;
    QComboBox   *m_speaker_preset_combo = nullptr;
    QComboBox   *m_speaker_exclude_combo_1 = nullptr;
    QComboBox   *m_speaker_exclude_combo_2 = nullptr;
    QComboBox   *m_speaker_override_combo = nullptr;
    QPushButton *m_speaker_take_btn = nullptr;
    QPushButton *m_speaker_release_btn = nullptr;

    // First-run credentials notice
    CvBanner    *m_credentials_banner = nullptr;
    // Non-intrusive "a newer CoreVideo build is available" notice
    CvBanner    *m_update_banner      = nullptr;
    QString      m_update_url;

    // Join controls
    QLineEdit   *m_meeting_id   = nullptr;
    QLineEdit   *m_passcode     = nullptr;
    QLineEdit   *m_display_name = nullptr;
    QComboBox   *m_join_token_type = nullptr;
    QLineEdit   *m_join_token   = nullptr;
    QPushButton *m_join_btn     = nullptr;
    QPushButton *m_leave_btn    = nullptr;
    QPushButton *m_start_engine_btn = nullptr;
    QPushButton *m_stop_engine_btn  = nullptr;
    QPushButton *m_launch_sidecar_btn = nullptr;
    QPushButton *m_output_manager_btn = nullptr;
    QCheckBox   *m_webinar_cb   = nullptr;
    QLineEdit   *m_participant_filter = nullptr;
    QListWidget *m_participant_list   = nullptr;

    // Talkback probe (Milestone 1 diagnostic, not the talkback feature -- see
    // zoom-dock.cpp where the group box is built). m_talkback_participant_combo
    // is a dedicated, roster-driven selector rather than the legacy
    // m_participant_list above: that list is only ever populated inside
    // refresh_outputs(), which returns immediately whenever m_output_table is
    // null (it always is now that routing lives in the Output Manager
    // dialog), so it never actually runs.
    QComboBox   *m_talkback_participant_combo = nullptr;
    QPushButton *m_talkback_probe_btn         = nullptr;
    QLabel      *m_talkback_status_label      = nullptr;

    // Talkback (Milestone 7): the operator surface proper -- source choice and
    // its program-track warning, nomination and its budget outcome, and the
    // key buttons. Dock keying is a deliberate, owner-approved deviation from
    // the spec, which locked keying to Companion/control API/hotkey and
    // explicitly not the dock; see src/talkback-dock-state.h.
    QComboBox   *m_talkback_source_combo   = nullptr;
    QLabel      *m_talkback_track_label    = nullptr;
    QListWidget *m_talkback_nominee_list   = nullptr;
    QPushButton *m_talkback_nominate_btn   = nullptr;
    QLabel      *m_talkback_plan_label     = nullptr;
    QWidget     *m_talkback_key_row        = nullptr;
    QCheckBox   *m_talkback_latch_cb       = nullptr;
    QLabel      *m_talkback_tally_label    = nullptr;
    QLabel      *m_talkback_notice        = nullptr;
    std::vector<QPushButton *> m_talkback_key_buttons;
    // What the last rebuild of the key buttons was built from. The buttons are
    // rebuilt only when this changes: the tick runs ten times a second, and
    // deleting the widget the operator is holding is not a refresh, it is a
    // lost release (see talkback_dock_release_lost()).
    std::string m_talkback_key_signature;
    // Which roster names the nominee list was last built from, for the same
    // reason -- a rebuild loses the tick-boxes the operator has just set.
    std::string m_talkback_roster_signature;
    // The target THIS dock currently has keyed, empty when it has none. A key
    // opened over the control API is visible in the tally but is not ours to
    // close, so the dock's own release/backstop paths key off this rather than
    // off the controller's "open" flag.
    std::string m_talkback_dock_target;
    bool        m_talkback_dock_latched = false;
    // The dock's own last refusal -- one the engine never sees (no source
    // chosen, the source is not in the current scene, OBS is at a sample rate
    // the Zoom talkback API will not take, a nominee named "all"). Cleared by
    // the next attempt that gets past it.
    QString     m_talkback_notice_text;

    // Recovery status panel (shown only while Recovering)
    QFrame      *m_recovery_frame  = nullptr;
    QLabel      *m_recovery_label  = nullptr;
    QPushButton *m_cancel_rec_btn  = nullptr;
    QTimer      *m_countdown_timer = nullptr;
    QTimer      *m_refresh_timer   = nullptr;

    qint64       m_join_started_ms      = 0;
    bool         m_join_timeout_reported = false;
    // Log the "held for a waiting room" explanation once per join attempt, not
    // on every 100ms refresh tick.
    bool         m_join_wait_logged = false;
    std::thread  m_join_thread;
    std::atomic<bool>     m_join_in_progress{false};
    std::atomic<uint64_t> m_join_generation{0};
    QTimer      *m_pending_oauth_join_timer = nullptr;
    bool         m_last_media_active = false;
    QTimer      *m_health_retry_timer = nullptr;

    // Legacy output helpers are kept inert; routing now lives in Output Manager.
    QTableWidget *m_output_table = nullptr;
    QPushButton  *m_apply_btn   = nullptr;

    std::shared_ptr<std::atomic<bool>> m_alive =
        std::make_shared<std::atomic<bool>>(true);
};
