#include "zoom-settings-dialog.h"
#include "cv-style.h"
#include "zoom-settings.h"
#include "zoom-control-server.h"
#include "zoom-oauth.h"
#include "zoom-osc-server.h"
#include "zoom-reconnect.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>

ZoomSettingsDialog::ZoomSettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Zoom Plugin Settings");
    setMinimumWidth(480);

    ZoomPluginSettings s = ZoomPluginSettings::load();

    // ── Zoom SDK ──────────────────────────────────────────────────────────────
    m_sdk_key_edit    = new QLineEdit(QString::fromStdString(s.sdk_key),    this);
    m_sdk_secret_edit = new QLineEdit(QString::fromStdString(s.sdk_secret), this);
    m_jwt_token_edit  = new QLineEdit(QString::fromStdString(s.jwt_token),  this);
    m_oauth_client_id_edit = new QLineEdit(QString::fromStdString(s.oauth_client_id), this);
    m_oauth_authorization_url_edit =
        new QLineEdit(QString::fromStdString(s.oauth_authorization_url), this);
    m_oauth_redirect_uri_edit =
        new QLineEdit(QString::fromStdString(s.oauth_redirect_uri), this);
    m_oauth_scopes_edit = new QLineEdit(QString::fromStdString(s.oauth_scopes), this);

    m_sdk_secret_edit->setEchoMode(QLineEdit::Password);
    m_jwt_token_edit->setEchoMode(QLineEdit::Password);
    m_jwt_token_edit->setPlaceholderText(
        "Optional override; leave blank to generate from SDK key / secret");

    auto *sdk_help = new QLabel(
        "Obtain your SDK key and secret from the "
        "<a href=\"https://marketplace.zoom.us\">Zoom Marketplace</a> "
        "(Develop → Build App → Meeting SDK).", this);
    sdk_help->setOpenExternalLinks(true);
    sdk_help->setWordWrap(true);
    sdk_help->setStyleSheet("QLabel { color: #7a8faa; font-size: 11px; }");

    auto *sdk_form = new QFormLayout;
    sdk_form->setSpacing(8);
    sdk_form->addRow(new QLabel("SDK Key:",    this), m_sdk_key_edit);
    sdk_form->addRow(new QLabel("SDK Secret:", this), m_sdk_secret_edit);
    sdk_form->addRow(new QLabel("JWT Token:",  this), m_jwt_token_edit);
    sdk_form->addRow(sdk_help);

    auto *sdk_group = new QGroupBox("Zoom SDK", this);
    sdk_group->setLayout(sdk_form);

    // ── Control Server ────────────────────────────────────────────────────────
    m_control_port_spin = new QSpinBox(this);
    m_control_port_spin->setRange(1024, 65535);
    m_control_port_spin->setValue(s.control_server_port);

    m_control_token_edit = new QLineEdit(QString::fromStdString(s.control_token), this);
    m_control_token_edit->setEchoMode(QLineEdit::Password);
    m_control_token_edit->setPlaceholderText("Leave blank to allow unauthenticated access");

    // ── OAuth / PKCE ──────────────────────────────────────────────────────────
    m_oauth_authorize_btn = new QPushButton("Authorize with Zoom", this);
    m_oauth_register_scheme_btn = new QPushButton("Register corevideo:// URL Scheme", this);
    m_oauth_authorize_btn->setProperty("role", "primary");
    connect(m_oauth_authorize_btn, &QPushButton::clicked,
            this, &ZoomSettingsDialog::onAuthorizeOAuth);
    connect(m_oauth_register_scheme_btn, &QPushButton::clicked,
            this, &ZoomSettingsDialog::onRegisterUrlScheme);

    auto *oauth_form = new QFormLayout;
    oauth_form->setSpacing(8);
    oauth_form->addRow(new QLabel("Client ID:",          this), m_oauth_client_id_edit);
    oauth_form->addRow(new QLabel("Authorization URL:",  this), m_oauth_authorization_url_edit);
    oauth_form->addRow(new QLabel("Redirect URI:",       this), m_oauth_redirect_uri_edit);
    oauth_form->addRow(new QLabel("Scopes:",             this), m_oauth_scopes_edit);
    oauth_form->addRow("", m_oauth_register_scheme_btn);
    oauth_form->addRow("", m_oauth_authorize_btn);

    auto *oauth_group = new QGroupBox("Zoom OAuth (PKCE)", this);
    oauth_group->setLayout(oauth_form);


    auto *ctrl_form = new QFormLayout;
    ctrl_form->setSpacing(8);
    ctrl_form->addRow(new QLabel("TCP Port:", this), m_control_port_spin);
    ctrl_form->addRow(new QLabel("Token:",    this), m_control_token_edit);

    auto *ctrl_group = new QGroupBox("Control Server (127.0.0.1 TCP/JSON)", this);
    ctrl_group->setLayout(ctrl_form);

    // ── OSC Server ────────────────────────────────────────────────────────────
    m_osc_port_spin = new QSpinBox(this);
    m_osc_port_spin->setRange(1024, 65535);
    m_osc_port_spin->setValue(s.osc_server_port);

    auto *osc_form = new QFormLayout;
    osc_form->setSpacing(8);
    osc_form->addRow(new QLabel("UDP Port:", this), m_osc_port_spin);

    auto *osc_group = new QGroupBox("OSC Server (127.0.0.1 UDP)", this);
    osc_group->setLayout(osc_form);

    // ── Hardware Video Acceleration ───────────────────────────────────────────
    m_hw_accel_combo = new QComboBox(this);
    m_hw_accel_combo->addItem("Disabled (CPU only)",          static_cast<int>(HwAccelMode::None));
    m_hw_accel_combo->addItem("Auto (detect best available)", static_cast<int>(HwAccelMode::Auto));
    m_hw_accel_combo->addItem("CUDA (NVIDIA)",                static_cast<int>(HwAccelMode::Cuda));
    m_hw_accel_combo->addItem("VAAPI (Intel / AMD on Linux)", static_cast<int>(HwAccelMode::Vaapi));
    m_hw_accel_combo->addItem("VideoToolbox (Apple)",         static_cast<int>(HwAccelMode::VideoToolbox));
    m_hw_accel_combo->addItem("QSV (Intel Quick Sync)",       static_cast<int>(HwAccelMode::Qsv));
    for (int i = 0; i < m_hw_accel_combo->count(); ++i) {
        if (m_hw_accel_combo->itemData(i).toInt() == static_cast<int>(s.hw_accel_mode)) {
            m_hw_accel_combo->setCurrentIndex(i);
            break;
        }
    }
#ifndef COREVIDEO_HW_ACCEL
    m_hw_accel_combo->setEnabled(false);
    m_hw_accel_combo->setToolTip("Rebuild with ENABLE_FFMPEG_HW_ACCEL=ON to enable");
#endif

    auto *hw_form = new QFormLayout;
    hw_form->setSpacing(8);
    hw_form->addRow(new QLabel("Mode:", this), m_hw_accel_combo);

    auto *hw_group = new QGroupBox("Hardware Video Acceleration", this);
    hw_group->setLayout(hw_form);

    // ── Auto-Reconnect ────────────────────────────────────────────────────────
    const ZoomReconnectPolicy &rp = s.reconnect_policy;

    m_rc_enabled_cb = new QCheckBox("Enable auto-reconnect", this);
    m_rc_enabled_cb->setChecked(rp.enabled);

    m_rc_max_attempts_spin = new QSpinBox(this);
    m_rc_max_attempts_spin->setRange(1, 20);
    m_rc_max_attempts_spin->setValue(rp.max_attempts);

    m_rc_base_delay_spin = new QSpinBox(this);
    m_rc_base_delay_spin->setRange(500, 30000);
    m_rc_base_delay_spin->setSingleStep(500);
    m_rc_base_delay_spin->setSuffix(" ms");
    m_rc_base_delay_spin->setValue(rp.base_delay_ms);

    m_rc_max_delay_spin = new QSpinBox(this);
    m_rc_max_delay_spin->setRange(1000, 120000);
    m_rc_max_delay_spin->setSingleStep(1000);
    m_rc_max_delay_spin->setSuffix(" ms");
    m_rc_max_delay_spin->setValue(rp.max_delay_ms);

    m_rc_on_crash_cb = new QCheckBox("On engine crash", this);
    m_rc_on_crash_cb->setChecked(rp.on_engine_crash);
    m_rc_on_disc_cb  = new QCheckBox("On meeting disconnect / network drop", this);
    m_rc_on_disc_cb->setChecked(rp.on_disconnect);
    m_rc_on_auth_cb  = new QCheckBox("On authentication failure (may loop if credentials are wrong)", this);
    m_rc_on_auth_cb->setChecked(rp.on_auth_fail);

    auto *rc_form = new QFormLayout;
    rc_form->setSpacing(8);
    rc_form->addRow("",                     m_rc_enabled_cb);
    rc_form->addRow("Max attempts:",        m_rc_max_attempts_spin);
    rc_form->addRow("Initial retry delay:", m_rc_base_delay_spin);
    rc_form->addRow("Max retry delay:",     m_rc_max_delay_spin);
    rc_form->addRow("Reconnect triggers:",  m_rc_on_crash_cb);
    rc_form->addRow("",                     m_rc_on_disc_cb);
    rc_form->addRow("",                     m_rc_on_auth_cb);

    auto *rc_group = new QGroupBox("Auto-Reconnect", this);
    rc_group->setLayout(rc_form);

    // ── Buttons ───────────────────────────────────────────────────────────────
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ZoomSettingsDialog::onSave);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Style the Save button as a primary action
    if (auto *save_btn = buttons->button(QDialogButtonBox::Save))
        save_btn->setProperty("role", "primary");

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(8);
    layout->addWidget(sdk_group);
    layout->addWidget(oauth_group);
    layout->addWidget(ctrl_group);
    layout->addWidget(osc_group);
    layout->addWidget(hw_group);
    layout->addWidget(rc_group);
    layout->addWidget(buttons);

    // Apply stylesheet last so all widget properties are already set
    setStyleSheet(cv_stylesheet());
}

void ZoomSettingsDialog::onSave()
{
    ZoomPluginSettings s = ZoomPluginSettings::load();
    s.sdk_key             = m_sdk_key_edit->text().toStdString();
    s.sdk_secret          = m_sdk_secret_edit->text().toStdString();
    s.jwt_token           = m_jwt_token_edit->text().toStdString();
    s.oauth_client_id     = m_oauth_client_id_edit->text().toStdString();
    s.oauth_authorization_url =
        m_oauth_authorization_url_edit->text().toStdString();
    s.oauth_redirect_uri  = m_oauth_redirect_uri_edit->text().toStdString();
    s.oauth_scopes        = m_oauth_scopes_edit->text().toStdString();
    s.control_server_port = static_cast<uint16_t>(m_control_port_spin->value());
    s.osc_server_port     = static_cast<uint16_t>(m_osc_port_spin->value());
    s.control_token       = m_control_token_edit->text().toStdString();
    s.hw_accel_mode       = static_cast<HwAccelMode>(
        m_hw_accel_combo->currentData().toInt());
    s.reconnect_policy.enabled         = m_rc_enabled_cb->isChecked();
    s.reconnect_policy.max_attempts    = m_rc_max_attempts_spin->value();
    s.reconnect_policy.base_delay_ms   = m_rc_base_delay_spin->value();
    s.reconnect_policy.max_delay_ms    = m_rc_max_delay_spin->value();
    s.reconnect_policy.on_engine_crash = m_rc_on_crash_cb->isChecked();
    s.reconnect_policy.on_disconnect   = m_rc_on_disc_cb->isChecked();
    s.reconnect_policy.on_auth_fail    = m_rc_on_auth_cb->isChecked();
    s.save();
    ZoomReconnectManager::instance().set_policy(s.reconnect_policy);

    ZoomControlServer::instance().set_token(s.control_token);

    ZoomOscServer::instance().stop();
    if (!ZoomOscServer::instance().start(s.osc_server_port)) {
        QMessageBox::warning(this, "OSC Server",
            QString("Failed to bind OSC server on port %1.\n"
                    "Check that the port is not already in use.")
                .arg(s.osc_server_port));
        return;
    }

    accept();
}

void ZoomSettingsDialog::onAuthorizeOAuth()
{
    ZoomPluginSettings s = ZoomPluginSettings::load();
    s.oauth_client_id = m_oauth_client_id_edit->text().toStdString();
    s.oauth_authorization_url =
        m_oauth_authorization_url_edit->text().toStdString();
    s.oauth_redirect_uri = m_oauth_redirect_uri_edit->text().toStdString();
    s.oauth_scopes = m_oauth_scopes_edit->text().toStdString();
    s.save();
    QString error;
    if (!ZoomOAuthManager::instance().begin_authorization(this, &error)) {
        QMessageBox::warning(this, "Zoom OAuth", error);
    }
}

void ZoomSettingsDialog::onRegisterUrlScheme()
{
    ZoomPluginSettings s = ZoomPluginSettings::load();
    s.oauth_client_id = m_oauth_client_id_edit->text().toStdString();
    s.oauth_authorization_url =
        m_oauth_authorization_url_edit->text().toStdString();
    s.oauth_redirect_uri = m_oauth_redirect_uri_edit->text().toStdString();
    s.oauth_scopes = m_oauth_scopes_edit->text().toStdString();
    s.save();
    QString error;
    if (!ZoomOAuthManager::instance().register_url_scheme(&error)) {
        QMessageBox::warning(this, "Zoom OAuth", error);
        return;
    }
    QMessageBox::information(this, "Zoom OAuth",
        "Registered corevideo://oauth/callback for this Windows user.");
}
