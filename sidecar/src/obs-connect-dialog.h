#pragma once
#include "obs-client.h"
#include <QDialog>

class QLineEdit;
class QSpinBox;
class QCheckBox;

class OBSConnectDialog : public QDialog {
    Q_OBJECT
public:
    explicit OBSConnectDialog(const OBSClient::Config &cfg,
                              QWidget *parent = nullptr);

    OBSClient::Config config() const;

private:
    QLineEdit *m_host = nullptr;
    QSpinBox  *m_port = nullptr;
    QLineEdit *m_password = nullptr;
    QCheckBox *m_autoReconnect = nullptr;
};
