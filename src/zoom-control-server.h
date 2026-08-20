#pragma once

#include "zoom-types.h"
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTcpSocket>
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
    // QPointer, not a raw socket: command handlers can block (the join path
    // fetches a ZAK over HTTPS with a nested event loop), and a client that
    // disconnects meanwhile has its socket deleteLater'd INSIDE that nested
    // loop. A raw pointer then dangles and the response write is a
    // use-after-free (live SIGSEGV, 2026-08-20, driven by one-shot nc
    // clients). QPointer nulls itself on destruction; write_response checks.
    void handle_line(QPointer<QTcpSocket> socket, const QByteArray &line);
    void write_response(const QPointer<QTcpSocket> &socket,
                        const QJsonObject &response);
    void remove_subscriber(QTcpSocket *socket);
    void poll_and_push();

    QTcpServer         *m_server      = nullptr;
    std::string         m_token;
    QSet<QTcpSocket *>  m_event_subs;
    QTimer             *m_poll_timer  = nullptr;
    MeetingState        m_last_state  = MeetingState::Idle;
    uint32_t            m_last_speaker = 0;
    uint32_t            m_last_directed_speaker = 0;
    uint32_t            m_last_candidate_speaker = 0;
    uint32_t            m_last_manual_speaker = 0;
    bool                m_running     = false;
};
