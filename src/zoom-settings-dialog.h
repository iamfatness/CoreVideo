#pragma once
#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QSpinBox;

class ZoomSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ZoomSettingsDialog(QWidget *parent = nullptr);

private slots:
    void onSave();
    void onAuthorizeOAuth();
    void onRefreshOAuth();
    void onDisconnectOAuth();

private:
    bool saveSettings(bool close_dialog, bool restart_servers);
    void updateOAuthStatus();

    QLabel *m_oauth_status_label        = nullptr;
    QPushButton *m_oauth_authorize_btn  = nullptr;
    QPushButton *m_oauth_refresh_btn    = nullptr;
    QPushButton *m_oauth_disconnect_btn = nullptr;
    QSpinBox  *m_control_port_spin      = nullptr;
    QSpinBox  *m_osc_port_spin          = nullptr;
    QLineEdit *m_control_token_edit     = nullptr;
    QComboBox *m_hw_accel_combo         = nullptr;
    // Global delay for the DEDICATED CoreVideo audio sources -- the ones a
    // show routes to program. Not the Output Manager's per-row Delay, which
    // acts on a video source's own embedded audio.
    QSpinBox  *m_audio_delay_spin       = nullptr;
    // Auto-reconnect
    QCheckBox *m_rc_enabled_cb          = nullptr;
    QSpinBox  *m_rc_max_attempts_spin   = nullptr;
    QSpinBox  *m_rc_base_delay_spin     = nullptr;
    QSpinBox  *m_rc_max_delay_spin      = nullptr;
    QCheckBox *m_rc_on_crash_cb         = nullptr;
    QCheckBox *m_rc_on_disc_cb          = nullptr;
    QCheckBox *m_rc_on_auth_cb          = nullptr;
    // Update check
    QCheckBox *m_check_updates_cb       = nullptr;
};
