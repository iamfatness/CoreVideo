#pragma once
#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;

class ZoomSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ZoomSettingsDialog(QWidget *parent = nullptr);

private slots:
    void onSave();

private:
    QLineEdit *m_sdk_key_edit           = nullptr;
    QLineEdit *m_sdk_secret_edit        = nullptr;
    QLineEdit *m_jwt_token_edit         = nullptr;
    QSpinBox  *m_control_port_spin      = nullptr;
    QSpinBox  *m_osc_port_spin          = nullptr;
    QLineEdit *m_control_token_edit     = nullptr;
    QComboBox *m_hw_accel_combo         = nullptr;
};
