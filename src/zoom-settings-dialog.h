#pragma once
#include <QDialog>

class QCheckBox;
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
    // Auto-reconnect
    QCheckBox *m_rc_enabled_cb          = nullptr;
    QSpinBox  *m_rc_max_attempts_spin   = nullptr;
    QSpinBox  *m_rc_base_delay_spin     = nullptr;
    QSpinBox  *m_rc_max_delay_spin      = nullptr;
    QCheckBox *m_rc_on_crash_cb         = nullptr;
    QCheckBox *m_rc_on_disc_cb          = nullptr;
    QCheckBox *m_rc_on_auth_cb          = nullptr;
};
