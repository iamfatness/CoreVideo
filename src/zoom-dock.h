#pragma once

#include <QFrame>
#include <QWidget>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

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
    void update_recovery_panel();
    void update_credentials_banner();
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
    QSpinBox    *m_speaker_sensitivity_spin = nullptr;
    QSpinBox    *m_speaker_hold_spin = nullptr;
    QComboBox   *m_speaker_override_combo = nullptr;
    QPushButton *m_speaker_take_btn = nullptr;
    QPushButton *m_speaker_release_btn = nullptr;

    // First-run credentials notice
    CvBanner    *m_credentials_banner = nullptr;

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

    // Recovery status panel (shown only while Recovering)
    QFrame      *m_recovery_frame  = nullptr;
    QLabel      *m_recovery_label  = nullptr;
    QPushButton *m_cancel_rec_btn  = nullptr;
    QTimer      *m_countdown_timer = nullptr;
    QTimer      *m_refresh_timer   = nullptr;

    qint64       m_join_started_ms      = 0;
    bool         m_join_timeout_reported = false;
    std::thread  m_join_thread;
    std::atomic<bool>     m_join_in_progress{false};
    std::atomic<uint64_t> m_join_generation{0};
    QTimer      *m_pending_oauth_join_timer = nullptr;
    bool         m_last_media_active = false;

    // Legacy output helpers are kept inert; routing now lives in Output Manager.
    QTableWidget *m_output_table = nullptr;
    QPushButton  *m_apply_btn   = nullptr;

    std::shared_ptr<std::atomic<bool>> m_alive =
        std::make_shared<std::atomic<bool>>(true);
};
