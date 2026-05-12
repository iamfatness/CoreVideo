#pragma once

#include <QDialog>

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
    void apply();

    QTableWidget *m_table = nullptr;
    QTableWidget *m_participant_table = nullptr;
    QLineEdit *m_filter = nullptr;
    QLineEdit *m_preset_name = nullptr;
    QComboBox *m_preset_combo = nullptr;
};
