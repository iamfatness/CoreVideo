#pragma once
#include "obs-client.h"
#include <QWidget>

class QLineEdit;
class QSpinBox;
class QCheckBox;

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(QWidget *parent = nullptr);

    // Load saved values from QSettings into the form
    void loadSettings();
    // Persist form values to QSettings
    void saveSettings();

    // Getters used by MainWindow
    QString targetScene()       const;
    QString sourcePattern()     const;  // e.g. "Zoom Participant %1"
    int     canvasWidth()       const;
    int     canvasHeight()      const;
    OBSClient::Config obsConfig() const;

signals:
    void settingsChanged();

private:
    QLineEdit *m_scene         = nullptr;
    QLineEdit *m_sourcePattern = nullptr;
    QSpinBox  *m_canvasW       = nullptr;
    QSpinBox  *m_canvasH       = nullptr;
    QLineEdit *m_obsHost       = nullptr;
    QSpinBox  *m_obsPort       = nullptr;
    QLineEdit *m_obsPassword   = nullptr;
    QCheckBox *m_autoReconnect = nullptr;
};
