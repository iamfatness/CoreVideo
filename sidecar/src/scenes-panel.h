#pragma once
#include <QWidget>
#include <QStringList>

class QListWidget;
class QLabel;
class QPushButton;

class ScenesPanel : public QWidget {
    Q_OBJECT
public:
    explicit ScenesPanel(QWidget *parent = nullptr);

    void setScenes(const QStringList &scenes);
    void setCurrentScene(const QString &name);
    QString selectedScene() const;

signals:
    void sceneActivated(const QString &name);   // user double-clicked / pressed Activate
    void refreshRequested();

private:
    QListWidget *m_list         = nullptr;
    QLabel      *m_currentLabel = nullptr;
    QPushButton *m_activateBtn  = nullptr;
    QPushButton *m_refreshBtn   = nullptr;
};
