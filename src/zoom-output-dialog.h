#pragma once

#include <QDialog>
#include <atomic>
#include <memory>

class QTableWidget;
class QLineEdit;
class QComboBox;

class ZoomOutputDialog : public QDialog {
public:
    explicit ZoomOutputDialog(QWidget *parent = nullptr);
    ~ZoomOutputDialog() override;

private:
    void refresh();
    void refresh_participants();
    void refresh_profiles();
    void apply();
    void save_profile();
    void load_profile();
    void delete_profile();

    QTableWidget *m_table            = nullptr;
    QTableWidget *m_participant_table = nullptr;
    QLineEdit    *m_filter            = nullptr;
    QComboBox    *m_profile_combo     = nullptr;
    // Shared liveness flag — set to false in destructor so any in-flight
    // preview callbacks don't try to update widgets that are already destroyed.
    std::shared_ptr<std::atomic<bool>> m_alive =
        std::make_shared<std::atomic<bool>>(true);
};
