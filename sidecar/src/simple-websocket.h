#pragma once

#include <QObject>
#include <QByteArray>
#include <QTcpSocket>

class SimpleWebSocket : public QObject {
    Q_OBJECT
public:
    explicit SimpleWebSocket(QObject *parent = nullptr);

    void open(const QString &host, quint16 port, const QString &path = "/");
    void close();
    void sendTextMessage(const QByteArray &payload);
    bool isConnected() const { return m_connected; }

signals:
    void connected();
    void disconnected();
    void textMessageReceived(const QString &message);
    void errorOccurred(const QString &message);

private slots:
    void onReadyRead();
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketError(QAbstractSocket::SocketError);

private:
    void consumeHandshake();
    void consumeFrames();
    void sendFrame(quint8 opcode, const QByteArray &payload);

    QTcpSocket m_socket;
    QByteArray m_buffer;
    QByteArray m_key;
    bool m_handshakeComplete = false;
    bool m_connected = false;
};
