#pragma once

#include "zoom-types.h"
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QtGlobal>
#include <string>

class QTcpServer;
class QTcpSocket;

class ZoomControlServer : public QObject {
public:
    static ZoomControlServer &instance();

    bool start(quint16 port = 19870);
    void stop();
    void set_token(const std::string &token);

    // Push a JSON event to all sockets that sent subscribe_events.
    void push_event(const QJsonObject &event);

private:
    explicit ZoomControlServer(QObject *parent = nullptr);

    void on_new_connection();
    void handle_line(QTcpSocket *socket, const QByteArray &line);
    void write_response(QTcpSocket *socket, const QJsonObject &response);
    void remove_subscriber(QTcpSocket *socket);
    void poll_and_push();

    QTcpServer         *m_server      = nullptr;
    std::string         m_token;
    QSet<QTcpSocket *>  m_event_subs;
    QTimer             *m_poll_timer  = nullptr;
    MeetingState        m_last_state  = MeetingState::Idle;
    uint32_t            m_last_speaker = 0;
};
