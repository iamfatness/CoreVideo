#pragma once

#include <QWidget>
#include <QSet>
#include <QString>
#include <vector>

struct ZoomOutputInfo;
class QListWidget;

class QCheckBox;
class QComboBox;
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
    void refresh_now();

private:
    void browse_output_dir();
    void browse_ffmpeg();
    void test_ffmpeg();
    void start_recording();
    void stop_recording();
    void refresh_status();
    void refresh_feeds();
    std::vector<ZoomOutputInfo> selected_outputs() const;
    void refresh_encoder_guidance();
    void refresh_capacity_guidance();
    void persist_settings() const;
    void set_error(const QString &message);

    QLineEdit *m_output_dir = nullptr;
    QLineEdit *m_ffmpeg_path = nullptr;
    QComboBox *m_video_encoder = nullptr;
    QCheckBox *m_record_program = nullptr;
    QListWidget *m_feeds = nullptr;
    QSet<QString> m_selected_feeds;
    QPushButton *m_start_btn = nullptr;
    QPushButton *m_stop_btn = nullptr;
    QPushButton *m_test_btn = nullptr;
    QPushButton *m_open_folder_btn = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_encoder_guidance = nullptr;
    QLabel *m_capacity_guidance = nullptr;
    QLabel *m_disk_status = nullptr;
    QLabel *m_error = nullptr;
    QTableWidget *m_sessions = nullptr;
    QTimer *m_refresh_timer = nullptr;
    bool m_shutting_down = false;
};
