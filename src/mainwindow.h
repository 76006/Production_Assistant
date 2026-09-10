#pragma once

#include <QMainWindow>
#include <QSerialPort>

#include <array>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTimer;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    enum class Operation {
        Password = 0,
        InitialTime,
        Uid,
        Time,
        Restart,
        Count
    };

    enum class UiState {
        Idle,
        Sending,
        Success,
        Failure
    };

    enum class RouterCommand {
        Gps = 0,
        Server,
        MqSet,
        MqTopic,
        Count
    };

    static constexpr int operationCount = static_cast<int>(Operation::Count);
    static constexpr int routerCommandCount = static_cast<int>(RouterCommand::Count);

    void buildUi();
    void applyStyle();
    void refreshPorts();
    void toggleConnection();
    void setConnectionUi(bool connected, const QString &detail = {});

    void sendPassword();
    void sendInitialTime();
    void sendUidCommand();
    void sendCurrentTime();
    void sendRestart();
    void sendOperation(Operation operation, const QByteArray &payload, const QString &safeDescription);
    void finishOperation(bool success, const QString &detail);
    void setOperationState(Operation operation, UiState state, const QString &detail = {});
    void sendRouterCommand(RouterCommand command);
    void startOneClickConfiguration();
    void transmitRouterCommand(RouterCommand command);
    void finishRouterCommand(bool success, const QString &detail);
    void setRouterState(RouterCommand command, UiState state, const QString &detail = {});
    void setBatchState(UiState state, const QString &detail = {});
    void setActionsEnabled(bool enabled);

    void readSerialData();
    void processReply(const QString &reply);
    void handleSerialError(QSerialPort::SerialPortError error);
    void appendLog(const QString &direction, const QString &message);
    QString operationName(Operation operation) const;
    QLabel *statusLabel(Operation operation) const;
    QString routerCommandText(RouterCommand command) const;
    QString routerCommandName(RouterCommand command) const;
    QLabel *routerStatusLabel(RouterCommand command) const;

    QSerialPort *m_serial = nullptr;
    QTimer *m_replyTimer = nullptr;
    QByteArray m_receiveBuffer;
    Operation m_pendingOperation = Operation::Count;
    RouterCommand m_pendingRouterCommand = RouterCommand::Count;
    bool m_batchRunning = false;
    int m_batchIndex = -1;

    QComboBox *m_portCombo = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_connectButton = nullptr;
    QLabel *m_connectionStatus = nullptr;
    QLineEdit *m_uidInput = nullptr;
    std::array<QPushButton *, operationCount> m_actionButtons{};
    std::array<QLabel *, operationCount> m_statusLabels{};
    std::array<QPushButton *, routerCommandCount> m_routerButtons{};
    std::array<QLabel *, routerCommandCount> m_routerStatusLabels{};
    QPushButton *m_batchButton = nullptr;
    QLabel *m_batchStatus = nullptr;
    QPlainTextEdit *m_log = nullptr;
};
