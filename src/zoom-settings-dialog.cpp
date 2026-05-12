#include "zoom-settings-dialog.h"
#include "zoom-settings.h"
#include "zoom-auth.h"
#include "zoom-control-server.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>
#include <QMessageBox>

ZoomSettingsDialog::ZoomSettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Zoom Plugin Settings");
    setMinimumWidth(460);

    ZoomPluginSettings s = ZoomPluginSettings::load();

    m_sdk_key_edit    = new QLineEdit(QString::fromStdString(s.sdk_key),    this);
    m_sdk_secret_edit = new QLineEdit(QString::fromStdString(s.sdk_secret), this);
    m_jwt_token_edit  = new QLineEdit(QString::fromStdString(s.jwt_token),  this);

    m_sdk_secret_edit->setEchoMode(QLineEdit::Password);
    m_jwt_token_edit->setEchoMode(QLineEdit::Password);

    m_control_port_spin = new QSpinBox(this);
    m_control_port_spin->setRange(1024, 65535);
    m_control_port_spin->setValue(s.control_server_port);

    m_control_token_edit = new QLineEdit(QString::fromStdString(s.control_token), this);
    m_control_token_edit->setEchoMode(QLineEdit::Password);
    m_control_token_edit->setPlaceholderText("Leave blank to allow unauthenticated access");

    auto *sdk_form = new QFormLayout;
    sdk_form->addRow(new QLabel("SDK Key:",    this), m_sdk_key_edit);
    sdk_form->addRow(new QLabel("SDK Secret:", this), m_sdk_secret_edit);
    sdk_form->addRow(new QLabel("JWT Token:",  this), m_jwt_token_edit);

    auto *sdk_group = new QGroupBox("Zoom SDK", this);
    sdk_group->setLayout(sdk_form);

    auto *ctrl_form = new QFormLayout;
    ctrl_form->addRow(new QLabel("Port:",  this), m_control_port_spin);
    ctrl_form->addRow(new QLabel("Token:", this), m_control_token_edit);

    auto *ctrl_group = new QGroupBox("Control Server (127.0.0.1)", this);
    ctrl_group->setLayout(ctrl_form);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ZoomSettingsDialog::onSave);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(sdk_group);
    layout->addWidget(ctrl_group);
    layout->addWidget(buttons);
}

void ZoomSettingsDialog::onSave()
{
    ZoomPluginSettings s;
    s.sdk_key             = m_sdk_key_edit->text().toStdString();
    s.sdk_secret          = m_sdk_secret_edit->text().toStdString();
    s.jwt_token           = m_jwt_token_edit->text().toStdString();
    s.control_server_port = static_cast<uint16_t>(m_control_port_spin->value());
    s.control_token       = m_control_token_edit->text().toStdString();
    s.save();

    ZoomAuth &auth = ZoomAuth::instance();
    if (!s.sdk_key.empty() && !s.sdk_secret.empty()) {
        if (!auth.init(s.sdk_key, s.sdk_secret)) {
            QMessageBox::warning(this, "Zoom Plugin",
                "SDK initialisation failed. Check SDK key and secret.");
            return;
        }
    }
    if (!s.jwt_token.empty() &&
        auth.state() != ZoomAuthState::Authenticated &&
        auth.state() != ZoomAuthState::Authenticating) {
        auth.authenticate(s.jwt_token);
    }

    ZoomControlServer::instance().set_token(s.control_token);

    accept();
}
