#pragma once

#include "zoom-types.h"
#include <QDateTime>
#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <QtGlobal>
#include <cstdint>
#include <string>
#include <vector>

class QUdpSocket;

// Lightweight OSC argument — supports int32, float32, and string types.
struct OscArg {
    enum Type { Int32, Float32, String } type;
    int32_t     i = 0;
    float       f = 0.f;
    std::string s;
};

class ZoomOscServer : public QObject {
public:
    static ZoomOscServer &instance();

    bool start(quint16 port = 19871);
    void stop();

private:
    explicit ZoomOscServer(QObject *parent = nullptr);

    void on_datagram_ready();
    void dispatch(const QString &address, const std::vector<OscArg> &args,
                  const QHostAddress &sender, quint16 sender_port);

    // Build and send a simple OSC bundle reply to sender.
    void send_status(const QHostAddress &to, quint16 port);
    void send_recovery_status(const QHostAddress &to, quint16 port);
    void send_outputs(const QHostAddress &to, quint16 port);
    void send_participants(const QHostAddress &to, quint16 port);

    void handle_subscribe(const QHostAddress &addr, quint16 port);
    void handle_unsubscribe(const QHostAddress &addr, quint16 port);
    void push_to_all(const std::string &address, const std::string &type_tags,
                     const std::vector<OscArg> &args);
    void poll_and_push();

    static constexpr int kSubscriberTtlMs  = 300000;
    static constexpr int kPollIntervalMs   = 250;

    struct Subscriber {
        QHostAddress addr;
        quint16      port;
        QDateTime    renewed_at;
    };

    QUdpSocket         *m_socket       = nullptr;
    QVector<Subscriber> m_subscribers;
    QTimer             *m_poll_timer   = nullptr;
    MeetingState        m_last_state   = MeetingState::Idle;
    uint32_t            m_last_speaker = 0;
};
