#include "zoom-settings-dialog.h"
#include "zoom-settings.h"
#include "zoom-control-server.h"
#include "zoom-osc-server.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QComboBox>
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

    m_osc_port_spin = new QSpinBox(this);
    m_osc_port_spin->setRange(1024, 65535);
    m_osc_port_spin->setValue(s.osc_server_port);

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
    ctrl_form->addRow(new QLabel("TCP Port:",  this), m_control_port_spin);
    ctrl_form->addRow(new QLabel("Token:", this), m_control_token_edit);

    auto *ctrl_group = new QGroupBox("Control Server (127.0.0.1 TCP/JSON)", this);
    ctrl_group->setLayout(ctrl_form);

    auto *osc_form = new QFormLayout;
    osc_form->addRow(new QLabel("UDP Port:", this), m_osc_port_spin);

    auto *osc_group = new QGroupBox("OSC Server (127.0.0.1 UDP)", this);
    osc_group->setLayout(osc_form);

    m_hw_accel_combo = new QComboBox(this);
    m_hw_accel_combo->addItem("Disabled (CPU only)",          static_cast<int>(HwAccelMode::None));
    m_hw_accel_combo->addItem("Auto (detect best available)", static_cast<int>(HwAccelMode::Auto));
    m_hw_accel_combo->addItem("CUDA (NVIDIA)",                static_cast<int>(HwAccelMode::Cuda));
    m_hw_accel_combo->addItem("VAAPI (Intel / AMD on Linux)", static_cast<int>(HwAccelMode::Vaapi));
    m_hw_accel_combo->addItem("VideoToolbox (Apple)",         static_cast<int>(HwAccelMode::VideoToolbox));
    m_hw_accel_combo->addItem("QSV (Intel Quick Sync)",       static_cast<int>(HwAccelMode::Qsv));
    // Pre-select the current setting.
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
    hw_form->addRow(new QLabel("Mode:", this), m_hw_accel_combo);

    auto *hw_group = new QGroupBox("Hardware Video Acceleration", this);
    hw_group->setLayout(hw_form);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ZoomSettingsDialog::onSave);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(sdk_group);
    layout->addWidget(ctrl_group);
    layout->addWidget(osc_group);
    layout->addWidget(hw_group);
    layout->addWidget(buttons);
}

void ZoomSettingsDialog::onSave()
{
    ZoomPluginSettings s;
    s.sdk_key             = m_sdk_key_edit->text().toStdString();
    s.sdk_secret          = m_sdk_secret_edit->text().toStdString();
    s.jwt_token           = m_jwt_token_edit->text().toStdString();
    s.control_server_port = static_cast<uint16_t>(m_control_port_spin->value());
    s.osc_server_port     = static_cast<uint16_t>(m_osc_port_spin->value());
    s.control_token       = m_control_token_edit->text().toStdString();
    s.hw_accel_mode       = static_cast<HwAccelMode>(
        m_hw_accel_combo->currentData().toInt());
    s.save();

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
