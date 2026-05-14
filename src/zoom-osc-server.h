#pragma once

#include <QObject>
#include <QtGlobal>
#include <cstdint>
#include <string>
#include <vector>

class QUdpSocket;
class QHostAddress;

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

    QUdpSocket *m_socket = nullptr;
};
