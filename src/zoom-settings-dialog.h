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
    void onRegisterUrlScheme();
    void onRefreshOAuth();
    void onDisconnectOAuth();

private:
    void updateOAuthStatus();

    QLineEdit *m_sdk_key_edit           = nullptr;
    QLineEdit *m_sdk_secret_edit        = nullptr;
    QLineEdit *m_jwt_token_edit         = nullptr;
    QLineEdit *m_oauth_client_id_edit   = nullptr;
    QLineEdit *m_oauth_authorization_url_edit = nullptr;
    QLineEdit *m_oauth_redirect_uri_edit = nullptr;
    QLineEdit *m_oauth_scopes_edit      = nullptr;
    QLabel *m_oauth_status_label        = nullptr;
    QPushButton *m_oauth_authorize_btn  = nullptr;
    QPushButton *m_oauth_register_scheme_btn = nullptr;
    QPushButton *m_oauth_refresh_btn    = nullptr;
    QPushButton *m_oauth_disconnect_btn = nullptr;
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
