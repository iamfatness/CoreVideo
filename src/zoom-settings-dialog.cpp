#include "zoom-settings-dialog.h"
#include "zoom-settings.h"
#include "zoom-auth.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>

ZoomSettingsDialog::ZoomSettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Zoom Plugin Settings");
    setMinimumWidth(420);

    ZoomPluginSettings s = ZoomPluginSettings::load();

    m_sdk_key_edit    = new QLineEdit(QString::fromStdString(s.sdk_key),    this);
    m_sdk_secret_edit = new QLineEdit(QString::fromStdString(s.sdk_secret), this);
    m_jwt_token_edit  = new QLineEdit(QString::fromStdString(s.jwt_token),  this);

    m_sdk_secret_edit->setEchoMode(QLineEdit::Password);
    m_jwt_token_edit->setEchoMode(QLineEdit::Password);

    auto *form = new QFormLayout;
    form->addRow(new QLabel("SDK Key:",    this), m_sdk_key_edit);
    form->addRow(new QLabel("SDK Secret:", this), m_sdk_secret_edit);
    form->addRow(new QLabel("JWT Token:",  this), m_jwt_token_edit);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ZoomSettingsDialog::onSave);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

void ZoomSettingsDialog::onSave()
{
    ZoomPluginSettings s;
    s.sdk_key    = m_sdk_key_edit->text().toStdString();
    s.sdk_secret = m_sdk_secret_edit->text().toStdString();
    s.jwt_token  = m_jwt_token_edit->text().toStdString();
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

    accept();
}
