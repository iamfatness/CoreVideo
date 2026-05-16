#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTimer;

class ZoomIsoPanel : public QWidget {
    Q_OBJECT
public:
    explicit ZoomIsoPanel(QWidget *parent = nullptr);
    ~ZoomIsoPanel() override;
    void prepare_shutdown();

private:
    void browse_output_dir();
    void browse_ffmpeg();
    void test_ffmpeg();
    void start_recording();
    void stop_recording();
    void refresh_status();
    void persist_settings() const;
    void set_error(const QString &message);

    QLineEdit *m_output_dir = nullptr;
    QLineEdit *m_ffmpeg_path = nullptr;
    QCheckBox *m_record_program = nullptr;
    QPushButton *m_start_btn = nullptr;
    QPushButton *m_stop_btn = nullptr;
    QPushButton *m_test_btn = nullptr;
    QPushButton *m_open_folder_btn = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_error = nullptr;
    QTableWidget *m_sessions = nullptr;
    QTimer *m_refresh_timer = nullptr;
    bool m_shutting_down = false;
};
