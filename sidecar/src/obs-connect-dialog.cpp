#include "obs-connect-dialog.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QLabel>

OBSConnectDialog::OBSConnectDialog(const OBSClient::Config &cfg, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("OBS Connection");
    setMinimumWidth(360);

    auto *vl = new QVBoxLayout(this);

    auto *help = new QLabel(
        "Enter the obs-websocket connection details. The token is the password "
        "set in OBS → Tools → WebSocket Server Settings.", this);
    help->setWordWrap(true);
    help->setStyleSheet("color: #8080a0; font-size: 11px;");
    vl->addWidget(help);

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 8, 0, 0);

    m_host = new QLineEdit(cfg.host, this);
    m_port = new QSpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setValue(cfg.port);
    m_password = new QLineEdit(cfg.password, this);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText("Leave empty if no auth");
    m_autoReconnect = new QCheckBox("Reconnect automatically on disconnect", this);
    m_autoReconnect->setChecked(cfg.autoReconnect);

    form->addRow("Host:",     m_host);
    form->addRow("Port:",     m_port);
    form->addRow("Token:",    m_password);
    form->addRow("",          m_autoReconnect);
    vl->addLayout(form);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
    buttons->button(QDialogButtonBox::Ok)->setText("Connect");
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    vl->addWidget(buttons);
}

OBSClient::Config OBSConnectDialog::config() const
{
    OBSClient::Config c;
    c.host          = m_host->text().trimmed().isEmpty() ? "localhost" : m_host->text().trimmed();
    c.port          = m_port->value();
    c.password      = m_password->text();
    c.autoReconnect = m_autoReconnect->isChecked();
    return c;
}
