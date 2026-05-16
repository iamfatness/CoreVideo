#include "simple-websocket.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <limits>

static QByteArray websocket_accept_key(const QByteArray &key)
{
    constexpr auto kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    return QCryptographicHash::hash(key + kGuid, QCryptographicHash::Sha1).toBase64();
}

SimpleWebSocket::SimpleWebSocket(QObject *parent) : QObject(parent)
{
    connect(&m_socket, &QTcpSocket::connected,
            this, &SimpleWebSocket::onSocketConnected);
    connect(&m_socket, &QTcpSocket::disconnected,
            this, &SimpleWebSocket::onSocketDisconnected);
    connect(&m_socket, &QTcpSocket::readyRead,
            this, &SimpleWebSocket::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred,
            this, &SimpleWebSocket::onSocketError);
}

void SimpleWebSocket::open(const QString &host, quint16 port, const QString &path)
{
    close();
    m_buffer.clear();
    m_key = QByteArray::number(QRandomGenerator::global()->generate64()).toBase64();
    m_handshakeComplete = false;
    m_connected = false;

    const QString requestPath = path.isEmpty() ? "/" : path;
    QByteArray req;
    req += "GET " + requestPath.toUtf8() + " HTTP/1.1\r\n";
    req += "Host: " + host.toUtf8() + ":" + QByteArray::number(port) + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + m_key + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    req += "\r\n";

    m_socket.connectToHost(host, port);
    connect(&m_socket, &QTcpSocket::connected, this, [this, req]() {
        m_socket.write(req);
    }, Qt::SingleShotConnection);
}

void SimpleWebSocket::close()
{
    if (m_socket.state() == QAbstractSocket::UnconnectedState) return;
    if (m_connected) sendFrame(0x8, {});
    m_socket.disconnectFromHost();
}

void SimpleWebSocket::sendTextMessage(const QByteArray &payload)
{
    if (!m_connected) return;
    sendFrame(0x1, payload);
}

void SimpleWebSocket::onSocketConnected()
{
}

void SimpleWebSocket::onSocketDisconnected()
{
    const bool wasConnected = m_connected || m_handshakeComplete;
    m_connected = false;
    m_handshakeComplete = false;
    m_buffer.clear();
    if (wasConnected) emit disconnected();
}

void SimpleWebSocket::onSocketError(QAbstractSocket::SocketError)
{
    emit errorOccurred(m_socket.errorString());
}

void SimpleWebSocket::onReadyRead()
{
    m_buffer += m_socket.readAll();
    if (!m_handshakeComplete) consumeHandshake();
    if (m_handshakeComplete) consumeFrames();
}

void SimpleWebSocket::consumeHandshake()
{
    const int end = m_buffer.indexOf("\r\n\r\n");
    if (end < 0) return;

    const QByteArray header = m_buffer.left(end + 4);
    m_buffer.remove(0, end + 4);

    if (!header.startsWith("HTTP/1.1 101") &&
        !header.startsWith("HTTP/1.0 101")) {
        emit errorOccurred(QStringLiteral("WebSocket upgrade failed"));
        m_socket.disconnectFromHost();
        return;
    }

    const QByteArray expected = websocket_accept_key(m_key);
    if (!header.toLower().contains("sec-websocket-accept: " + expected.toLower())) {
        emit errorOccurred(QStringLiteral("WebSocket accept key mismatch"));
        m_socket.disconnectFromHost();
        return;
    }

    m_handshakeComplete = true;
    m_connected = true;
    emit connected();
}

void SimpleWebSocket::consumeFrames()
{
    while (m_buffer.size() >= 2) {
        const auto b0 = static_cast<quint8>(m_buffer[0]);
        const auto b1 = static_cast<quint8>(m_buffer[1]);
        const quint8 opcode = b0 & 0x0f;
        const bool masked = (b1 & 0x80) != 0;
        quint64 len = b1 & 0x7f;
        int pos = 2;

        if (len == 126) {
            if (m_buffer.size() < pos + 2) return;
            len = (static_cast<quint8>(m_buffer[pos]) << 8) |
                  static_cast<quint8>(m_buffer[pos + 1]);
            pos += 2;
        } else if (len == 127) {
            if (m_buffer.size() < pos + 8) return;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | static_cast<quint8>(m_buffer[pos + i]);
            pos += 8;
        }

        QByteArray mask;
        if (masked) {
            if (m_buffer.size() < pos + 4) return;
            mask = m_buffer.mid(pos, 4);
            pos += 4;
        }

        if (len > static_cast<quint64>(std::numeric_limits<int>::max())) {
            emit errorOccurred(QStringLiteral("WebSocket frame too large"));
            close();
            return;
        }

        if (m_buffer.size() < pos + static_cast<int>(len)) return;
        QByteArray payload = m_buffer.mid(pos, static_cast<int>(len));
        m_buffer.remove(0, pos + static_cast<int>(len));

        if (masked) {
            for (int i = 0; i < payload.size(); ++i)
                payload[i] = payload[i] ^ mask[i % 4];
        }

        if (opcode == 0x1) {
            emit textMessageReceived(QString::fromUtf8(payload));
        } else if (opcode == 0x8) {
            m_socket.disconnectFromHost();
            return;
        } else if (opcode == 0x9) {
            sendFrame(0xA, payload);
        }
    }
}

void SimpleWebSocket::sendFrame(quint8 opcode, const QByteArray &payload)
{
    QByteArray frame;
    frame.append(static_cast<char>(0x80 | (opcode & 0x0f)));

    const quint64 len = static_cast<quint64>(payload.size());
    if (len < 126) {
        frame.append(static_cast<char>(0x80 | len));
    } else if (len <= 0xffff) {
        frame.append(static_cast<char>(0x80 | 126));
        frame.append(static_cast<char>((len >> 8) & 0xff));
        frame.append(static_cast<char>(len & 0xff));
    } else {
        frame.append(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i)
            frame.append(static_cast<char>((len >> (8 * i)) & 0xff));
    }

    QByteArray mask(4, Qt::Uninitialized);
    const quint32 rnd = QRandomGenerator::global()->generate();
    mask[0] = static_cast<char>((rnd >> 24) & 0xff);
    mask[1] = static_cast<char>((rnd >> 16) & 0xff);
    mask[2] = static_cast<char>((rnd >> 8) & 0xff);
    mask[3] = static_cast<char>(rnd & 0xff);
    frame += mask;

    QByteArray masked = payload;
    for (int i = 0; i < masked.size(); ++i)
        masked[i] = masked[i] ^ mask[i % 4];
    frame += masked;

    m_socket.write(frame);
}
