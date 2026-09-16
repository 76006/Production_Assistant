#pragma once

#include <QHash>
#include <QObject>

class QTcpSocket;
class QTimer;

class MqttClient final : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Disconnected,
        Connecting,
        Connected
    };

    explicit MqttClient(QObject *parent = nullptr);

    State state() const;
    void connectToBroker(const QString &host, quint16 port, const QString &clientId,
                         const QString &username = {}, const QString &password = {});
    void disconnectFromBroker();
    bool subscribe(const QString &topic);
    bool publish(const QString &topic, const QByteArray &payload, bool retain = false);

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &message);
    void subscribed(const QString &topic);
    void messageReceived(const QString &topic, const QByteArray &payload);

private:
    static QByteArray encodeString(const QByteArray &value);
    static QByteArray encodeRemainingLength(int length);
    static QByteArray makePacket(quint8 header, const QByteArray &body);
    static QString connectionErrorText(quint8 returnCode);

    quint16 nextPacketIdentifier();
    void sendConnectPacket();
    void processIncomingData();
    void processPacket(quint8 header, const QByteArray &body);
    void reportSocketError();

    QTcpSocket *m_socket = nullptr;
    QTimer *m_keepAliveTimer = nullptr;
    QTimer *m_connectTimeoutTimer = nullptr;
    QByteArray m_receiveBuffer;
    QString m_clientId;
    QString m_username;
    QString m_password;
    QHash<quint16, QString> m_pendingSubscriptions;
    State m_state = State::Disconnected;
    quint16 m_packetIdentifier = 0;
    bool m_disconnectRequested = false;
};
