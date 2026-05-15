#pragma once

#include <QWidget>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QComboBox;
class QCheckBox;
class QTimer;

class ZoomDock : public QWidget {
    Q_OBJECT
public:
    explicit ZoomDock(QWidget *parent = nullptr);
    ~ZoomDock() override;

private:
    void refresh();
    void refresh_outputs();
    void apply_outputs();

    void on_join_clicked();
    void on_leave_clicked();
    void on_cancel_recovery_clicked();
    void update_state_indicator();
    void update_recovery_panel();

    // Meeting control bar
    QLabel      *m_state_dot    = nullptr;
    QLabel      *m_state_label  = nullptr;
    QLabel      *m_error_label  = nullptr;
    QLineEdit   *m_meeting_id   = nullptr;
    QLineEdit   *m_passcode     = nullptr;
    QLineEdit   *m_display_name = nullptr;
    QComboBox   *m_join_token_type = nullptr;
    QLineEdit   *m_join_token   = nullptr;
    QPushButton *m_join_btn     = nullptr;
    QPushButton *m_leave_btn    = nullptr;
    QCheckBox   *m_webinar_cb   = nullptr;
    QLineEdit   *m_participant_filter = nullptr;

    // Active speaker
    QLabel      *m_speaker_label = nullptr;

    // Recovery status panel (shown only when Recovering)
    QLabel      *m_recovery_label    = nullptr;
    QPushButton *m_cancel_rec_btn    = nullptr;
    QTimer      *m_countdown_timer   = nullptr;
    QTimer      *m_refresh_timer     = nullptr;
    qint64       m_join_started_ms   = 0;
    bool         m_join_timeout_reported = false;
    std::thread  m_join_thread;
    std::atomic<bool> m_join_in_progress{false};
    std::atomic<uint64_t> m_join_generation{0};

    // Output table (replaces the modal Output Manager)
    QTableWidget *m_output_table = nullptr;
    QPushButton  *m_apply_btn    = nullptr;

    std::shared_ptr<std::atomic<bool>> m_alive =
        std::make_shared<std::atomic<bool>>(true);
};
