#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QtGlobal>

class QTcpServer;
class QTcpSocket;

class ZoomControlServer : public QObject {
public:
    static ZoomControlServer &instance();

    bool start(quint16 port = 19870);
    void stop();

private:
    explicit ZoomControlServer(QObject *parent = nullptr);

    void on_new_connection();
    void handle_line(QTcpSocket *socket, const QByteArray &line);
    void write_response(QTcpSocket *socket, const QJsonObject &response);

    QTcpServer *m_server = nullptr;
};
