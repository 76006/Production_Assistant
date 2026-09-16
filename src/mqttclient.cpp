#include "mqttclient.h"

#include <QAbstractSocket>
#include <QHash>
#include <QTcpSocket>
#include <QTimer>

namespace {
constexpr quint16 keepAliveSeconds = 30;
}

MqttClient::MqttClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
    , m_keepAliveTimer(new QTimer(this))
    , m_connectTimeoutTimer(new QTimer(this))
{
    m_keepAliveTimer->setInterval(keepAliveSeconds * 500);
    m_connectTimeoutTimer->setSingleShot(true);
    m_connectTimeoutTimer->setInterval(10000);

    connect(m_socket, &QTcpSocket::connected, this, &MqttClient::sendConnectPacket);
    connect(m_socket, &QTcpSocket::readyRead, this, &MqttClient::processIncomingData);
    connect(m_socket, &QTcpSocket::disconnected, this, [this] {
        const bool wasActive = m_state != State::Disconnected;
        m_state = State::Disconnected;
        m_receiveBuffer.clear();
        m_pendingSubscriptions.clear();
        m_keepAliveTimer->stop();
        m_connectTimeoutTimer->stop();
        if (wasActive)
            emit disconnected();
    });
    connect(m_socket, &QTcpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) { reportSocketError(); });
    connect(m_keepAliveTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::Connected)
            m_socket->write(QByteArray::fromHex("c000"));
    });
    connect(m_connectTimeoutTimer, &QTimer::timeout, this, [this] {
        if (m_state != State::Connecting)
            return;
        emit errorOccurred(QStringLiteral("连接 Broker 超时（10 秒）"));
        const bool wasActive = m_state != State::Disconnected;
        m_state = State::Disconnected;
        m_socket->abort();
        if (wasActive)
            emit disconnected();
    });
}

MqttClient::State MqttClient::state() const
{
    return m_state;
}

void MqttClient::connectToBroker(const QString &host, quint16 port, const QString &clientId,
                                 const QString &username, const QString &password)
{
    if (m_state != State::Disconnected)
        disconnectFromBroker();

    m_clientId = clientId;
    m_username = username;
    m_password = password;
    m_receiveBuffer.clear();
    m_pendingSubscriptions.clear();
    m_disconnectRequested = false;
    m_state = State::Connecting;
    m_socket->connectToHost(host, port);
    m_connectTimeoutTimer->start();
}

void MqttClient::disconnectFromBroker()
{
    m_disconnectRequested = true;
    m_keepAliveTimer->stop();
    m_connectTimeoutTimer->stop();
    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->write(QByteArray::fromHex("e000"));
        m_socket->flush();
    }
    m_socket->disconnectFromHost();
    if (m_socket->state() == QAbstractSocket::UnconnectedState) {
        const bool wasActive = m_state != State::Disconnected;
        m_state = State::Disconnected;
        if (wasActive)
            emit disconnected();
    }
}

bool MqttClient::subscribe(const QString &topic)
{
    if (m_state != State::Connected || topic.isEmpty())
        return false;

    const quint16 packetId = nextPacketIdentifier();
    QByteArray body;
    body.append(static_cast<char>((packetId >> 8) & 0xff));
    body.append(static_cast<char>(packetId & 0xff));
    body.append(encodeString(topic.toUtf8()));
    body.append(char(0)); // QoS 0
    m_pendingSubscriptions.insert(packetId, topic);
    return m_socket->write(makePacket(0x82, body)) >= 0;
}

bool MqttClient::publish(const QString &topic, const QByteArray &payload, bool retain)
{
    if (m_state != State::Connected || topic.isEmpty())
        return false;

    QByteArray body = encodeString(topic.toUtf8());
    body.append(payload);
    return m_socket->write(makePacket(retain ? 0x31 : 0x30, body)) >= 0;
}

QByteArray MqttClient::encodeString(const QByteArray &value)
{
    QByteArray encoded;
    encoded.append(static_cast<char>((value.size() >> 8) & 0xff));
    encoded.append(static_cast<char>(value.size() & 0xff));
    encoded.append(value);
    return encoded;
}

QByteArray MqttClient::encodeRemainingLength(int length)
{
    QByteArray encoded;
    do {
        int digit = length % 128;
        length /= 128;
        if (length > 0)
            digit |= 0x80;
        encoded.append(static_cast<char>(digit));
    } while (length > 0);
    return encoded;
}

QByteArray MqttClient::makePacket(quint8 header, const QByteArray &body)
{
    QByteArray packet;
    packet.append(static_cast<char>(header));
    packet.append(encodeRemainingLength(body.size()));
    packet.append(body);
    return packet;
}

QString MqttClient::connectionErrorText(quint8 returnCode)
{
    switch (returnCode) {
    case 1: return QStringLiteral("Broker 不支持 MQTT 3.1.1");
    case 2: return QStringLiteral("Client ID 被拒绝");
    case 3: return QStringLiteral("Broker 当前不可用");
    case 4: return QStringLiteral("账号或密码格式错误");
    case 5: return QStringLiteral("未获连接授权");
    default: return QStringLiteral("Broker 拒绝连接，返回码 %1").arg(returnCode);
    }
}

quint16 MqttClient::nextPacketIdentifier()
{
    ++m_packetIdentifier;
    if (m_packetIdentifier == 0)
        ++m_packetIdentifier;
    return m_packetIdentifier;
}

void MqttClient::sendConnectPacket()
{
    QByteArray body;
    body.append(encodeString(QByteArrayLiteral("MQTT")));
    body.append(char(4)); // MQTT 3.1.1

    quint8 flags = 0x02; // Clean Session
    if (!m_username.isEmpty())
        flags |= 0x80;
    if (!m_password.isEmpty())
        flags |= 0x40;
    body.append(static_cast<char>(flags));
    body.append(static_cast<char>((keepAliveSeconds >> 8) & 0xff));
    body.append(static_cast<char>(keepAliveSeconds & 0xff));
    body.append(encodeString(m_clientId.toUtf8()));
    if (!m_username.isEmpty())
        body.append(encodeString(m_username.toUtf8()));
    if (!m_password.isEmpty())
        body.append(encodeString(m_password.toUtf8()));

    if (m_socket->write(makePacket(0x10, body)) < 0) {
        emit errorOccurred(QStringLiteral("MQTT CONNECT 数据发送失败"));
        m_socket->disconnectFromHost();
    }
}

void MqttClient::processIncomingData()
{
    m_receiveBuffer.append(m_socket->readAll());

    while (m_receiveBuffer.size() >= 2) {
        int remainingLength = 0;
        int multiplier = 1;
        int offset = 1;
        bool lengthComplete = false;

        for (int count = 0; count < 4; ++count) {
            if (offset >= m_receiveBuffer.size())
                return;
            const quint8 digit = static_cast<quint8>(m_receiveBuffer.at(offset++));
            remainingLength += (digit & 0x7f) * multiplier;
            if ((digit & 0x80) == 0) {
                lengthComplete = true;
                break;
            }
            multiplier *= 128;
        }

        if (!lengthComplete) {
            emit errorOccurred(QStringLiteral("收到无效的 MQTT 数据包长度"));
            m_socket->disconnectFromHost();
            return;
        }

        if (m_receiveBuffer.size() < offset + remainingLength)
            return;

        const quint8 header = static_cast<quint8>(m_receiveBuffer.at(0));
        const QByteArray body = m_receiveBuffer.mid(offset, remainingLength);
        m_receiveBuffer.remove(0, offset + remainingLength);
        processPacket(header, body);
    }
}

void MqttClient::processPacket(quint8 header, const QByteArray &body)
{
    const quint8 packetType = header >> 4;
    if (packetType == 2) { // CONNACK
        if (body.size() < 2) {
            emit errorOccurred(QStringLiteral("收到无效的 CONNACK"));
            m_socket->disconnectFromHost();
            return;
        }
        const quint8 returnCode = static_cast<quint8>(body.at(1));
        if (returnCode != 0) {
            emit errorOccurred(connectionErrorText(returnCode));
            m_socket->disconnectFromHost();
            return;
        }
        m_state = State::Connected;
        m_connectTimeoutTimer->stop();
        m_keepAliveTimer->start();
        emit connected();
        return;
    }

    if (packetType == 3) { // PUBLISH
        if (body.size() < 2)
            return;
        const int topicLength = (static_cast<quint8>(body.at(0)) << 8)
                                | static_cast<quint8>(body.at(1));
        if (topicLength < 0 || body.size() < 2 + topicLength)
            return;

        const QString topic = QString::fromUtf8(body.mid(2, topicLength));
        int payloadOffset = 2 + topicLength;
        const quint8 qos = (header >> 1) & 0x03;
        if (qos > 0) {
            if (body.size() < payloadOffset + 2)
                return;
            const quint16 packetId = (static_cast<quint8>(body.at(payloadOffset)) << 8)
                                     | static_cast<quint8>(body.at(payloadOffset + 1));
            payloadOffset += 2;
            if (qos == 1) {
                QByteArray acknowledgement;
                acknowledgement.append(static_cast<char>((packetId >> 8) & 0xff));
                acknowledgement.append(static_cast<char>(packetId & 0xff));
                m_socket->write(makePacket(0x40, acknowledgement));
            }
        }
        emit messageReceived(topic, body.mid(payloadOffset));
        return;
    }

    if (packetType == 9 && body.size() >= 3) { // SUBACK
        const quint16 packetId = (static_cast<quint8>(body.at(0)) << 8)
                                 | static_cast<quint8>(body.at(1));
        const QString topic = m_pendingSubscriptions.take(packetId);
        const quint8 result = static_cast<quint8>(body.at(2));
        if (result == 0x80)
            emit errorOccurred(QStringLiteral("Broker 拒绝订阅主题：%1").arg(topic));
        else
            emit subscribed(topic);
    }
}

void MqttClient::reportSocketError()
{
    if (m_disconnectRequested)
        return;

    emit errorOccurred(m_socket->errorString());
    const bool wasActive = m_state != State::Disconnected;
    m_state = State::Disconnected;
    m_keepAliveTimer->stop();
    m_connectTimeoutTimer->stop();
    m_pendingSubscriptions.clear();
    m_socket->abort();
    if (wasActive)
        emit disconnected();
}
