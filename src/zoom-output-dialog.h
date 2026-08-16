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
    // Bounds schedule_deferred_refresh()'s retry loop. A combo popup is
    // transient (closes on its own within a keystroke or two), but a
    // spinbox merely having keyboard focus is not -- an operator who clicks
    // into a Delay field and leaves it focused must not freeze Signal/SDK/
    // A/V Offset updates for the whole table indefinitely. Reset to 0 every
    // time refresh() actually runs.
    int           m_deferred_refresh_attempts = 0;
    // Shared liveness flag — set to false in destructor so any in-flight
    // preview callbacks don't try to update widgets that are already destroyed.
    std::shared_ptr<std::atomic<bool>> m_alive =
        std::make_shared<std::atomic<bool>>(true);
};
