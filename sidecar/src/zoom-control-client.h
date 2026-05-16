#pragma once

#include "participant-panel.h"
#include <QColor>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QVector>
#include <functional>

class QTcpSocket;

class ZoomControlClient : public QObject {
    Q_OBJECT
public:
    explicit ZoomControlClient(QObject *parent = nullptr);

    void start(const QString &host = QStringLiteral("127.0.0.1"), quint16 port = 19870);
    void refreshParticipants();
    void refreshOutputs();
    void assignOutput(const QString &sourceName,
                      int participantId,
                      bool isolateAudio = false,
                      const QString &audioChannels = QStringLiteral("mono"));

signals:
    void participantsUpdated(const QVector<ParticipantInfo> &participants);
    void outputSourcesUpdated(const QStringList &sourceNames);
    void log(const QString &message);

private:
    void sendRequest(const QJsonObject &request,
                     std::function<void(const QJsonObject &)> handler);
    static ParticipantInfo participantFromJson(const QJsonObject &obj);
    static QString initialsForName(const QString &name);
    static QColor colorForId(int id);

    QString m_host = QStringLiteral("127.0.0.1");
    quint16 m_port = 19870;
    QTimer *m_pollTimer = nullptr;
};
