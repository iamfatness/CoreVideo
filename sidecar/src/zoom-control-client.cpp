#include "zoom-control-client.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>

ZoomControlClient::ZoomControlClient(QObject *parent)
    : QObject(parent)
{
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(1500);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        refreshParticipants();
        refreshOutputs();
    });
}

void ZoomControlClient::start(const QString &host, quint16 port)
{
    m_host = host;
    m_port = port;
    refreshParticipants();
    refreshOutputs();
    m_pollTimer->start();
}

void ZoomControlClient::refreshParticipants()
{
    sendRequest(QJsonObject{{"cmd", "list_participants"}},
                [this](const QJsonObject &response) {
        if (!response.value("ok").toBool()) {
            emit log(QStringLiteral("Zoom roster unavailable: %1")
                         .arg(response.value("error").toString("unknown_error")));
            return;
        }

        QVector<ParticipantInfo> participants;
        for (const auto &value : response.value("participants").toArray()) {
            const QJsonObject obj = value.toObject();
            if (!obj.value("has_video").toBool()) continue;
            participants.append(participantFromJson(obj));
        }
        emit participantsUpdated(participants);
    });
}

void ZoomControlClient::refreshOutputs()
{
    sendRequest(QJsonObject{{"cmd", "list_outputs"}},
                [this](const QJsonObject &response) {
        if (!response.value("ok").toBool()) return;

        QStringList sourceNames;
        for (const auto &value : response.value("outputs").toArray()) {
            const QString source = value.toObject().value("source").toString();
            if (!source.isEmpty() && !sourceNames.contains(source))
                sourceNames.append(source);
        }
        if (!sourceNames.isEmpty())
            emit outputSourcesUpdated(sourceNames);
    });
}

void ZoomControlClient::assignOutput(const QString &sourceName,
                                     int participantId,
                                     bool isolateAudio,
                                     const QString &audioChannels)
{
    sendRequest(QJsonObject{
                    {"cmd", "assign_output_ex"},
                    {"source", sourceName},
                    {"mode", "participant"},
                    {"participant_id", participantId},
                    {"isolate_audio", isolateAudio},
                    {"audio_channels", audioChannels},
                    {"video_resolution", "1080p"},
                },
                [this, sourceName](const QJsonObject &response) {
        if (response.value("ok").toBool()) {
            emit log(QStringLiteral("Assigned output '%1'.").arg(sourceName));
        } else {
            emit log(QStringLiteral("Assign output '%1' failed: %2")
                         .arg(sourceName,
                              response.value("error").toString("unknown_error")));
        }
    });
}

void ZoomControlClient::sendRequest(
    const QJsonObject &request,
    std::function<void(const QJsonObject &)> handler)
{
    auto *socket = new QTcpSocket(this);
    connect(socket, &QTcpSocket::connected, this, [socket, request]() {
        socket->write(QJsonDocument(request).toJson(QJsonDocument::Compact));
        socket->write("\n");
        socket->flush();
    });
    connect(socket, &QTcpSocket::readyRead, this, [socket, handler]() {
        if (!socket->canReadLine()) return;
        const QByteArray line = socket->readLine().trimmed();
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
        if (error.error == QJsonParseError::NoError && doc.isObject())
            handler(doc.object());
        socket->disconnectFromHost();
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
        emit log(QStringLiteral("Zoom control connection failed: %1").arg(socket->errorString()));
        socket->deleteLater();
    });
    connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    socket->connectToHost(m_host, m_port);
}

ParticipantInfo ZoomControlClient::participantFromJson(const QJsonObject &obj)
{
    ParticipantInfo p;
    p.id = obj.value("id").toInt();
    p.name = obj.value("name").toString(QStringLiteral("Participant %1").arg(p.id));
    p.initials = initialsForName(p.name);
    p.color = colorForId(p.id);
    p.hasVideo = obj.value("has_video").toBool();
    p.isTalking = obj.value("is_talking").toBool();
    p.isSharingScreen = obj.value("is_sharing_screen").toBool();
    p.slotAssign = -1;
    return p;
}

QString ZoomControlClient::initialsForName(const QString &name)
{
    QString result;
    for (const QString &part : name.split(' ', Qt::SkipEmptyParts)) {
        result.append(part.left(1).toUpper());
        if (result.size() >= 2) break;
    }
    return result.isEmpty() ? QStringLiteral("?") : result;
}

QColor ZoomControlClient::colorForId(int id)
{
    const QByteArray hash =
        QCryptographicHash::hash(QByteArray::number(id), QCryptographicHash::Md5);
    return QColor::fromHsv(static_cast<unsigned char>(hash[0]), 180, 210);
}
