#pragma once

#include <QWidget>
#include <atomic>
#include <memory>

class QTableWidget;
class QLineEdit;
class QComboBox;
class QLabel;
class QCheckBox;

class ZoomOutputDialog : public QWidget {
public:
    explicit ZoomOutputDialog(QWidget *parent = nullptr);
    ~ZoomOutputDialog() override;
    void refresh_now();
    void prepare_shutdown();

private:
    void refresh();
    void refresh_participants();
    void refresh_profiles();
    bool has_open_output_combo_popup() const;
    void schedule_deferred_refresh();
    void apply();
    void save_profile();
    void load_profile();
    void delete_profile();

    QTableWidget *m_table            = nullptr;
    QTableWidget *m_participant_table = nullptr;
    QLineEdit    *m_filter            = nullptr;
    QCheckBox    *m_hide_non_video    = nullptr;
    QComboBox    *m_profile_combo     = nullptr;
    QLabel       *m_output_summary    = nullptr;
    bool          m_deferred_refresh_queued = false;
    // Shared liveness flag — set to false in destructor so any in-flight
    // preview callbacks don't try to update widgets that are already destroyed.
    std::shared_ptr<std::atomic<bool>> m_alive =
        std::make_shared<std::atomic<bool>>(true);
};
