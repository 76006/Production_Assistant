#pragma once

#include <QMainWindow>
#include <QSerialPort>

#include <array>

class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTimer;
class MqttClient;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    enum class Operation {
        Password = 0,
        InitialTime,
        UidCheck,
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

    enum class LinkTestStep {
        Idle,
        PreparingSubscription,
        SerialToMqtt,
        MqttToSerial
    };

    static constexpr int operationCount = static_cast<int>(Operation::Count);
    static constexpr int routerCommandCount = static_cast<int>(RouterCommand::Count);

    void buildUi();
    void applyStyle();
    void refreshPorts();
    QString currentPortName() const;
    void toggleConnection();
    void setConnectionUi(bool connected, const QString &detail = {});

    void sendPassword();
    void sendInitialTime();
    void sendUidCheck();
    void sendUidCommand();
    void sendCurrentTime();
    void sendRestart();
    void clearAllStatuses();
    void sendOperation(Operation operation, const QByteArray &payload, const QString &safeDescription,
                       bool fromProductionBatch = false);
    void finishOperation(bool success, const QString &detail);
    void setOperationState(Operation operation, UiState state, const QString &detail = {});
    void sendRouterCommand(RouterCommand command);
    void startOneClickConfiguration();
    void transmitRouterCommand(RouterCommand command);
    void finishRouterCommand(bool success, const QString &detail);
    void setRouterState(RouterCommand command, UiState state, const QString &detail = {});
    void setBatchState(UiState state, const QString &detail = {});
    void setActionsEnabled(bool enabled);
    void startProductionConfiguration();
    void sendNextProductionConfiguration();
    void finishProductionConfiguration(bool success, const QString &detail);

    void toggleMqttConnection();
    void subscribeMqttTopic();
    void publishMqttMessage();
    void sendSerialMessage();
    void setMqttUiConnected(bool connected, const QString &detail = {});
    void appendMessageRecord(const QString &source, const QString &topic, const QByteArray &payload);
    QString configurationFilePath() const;
    void loadUserConfiguration();
    void saveUserConfiguration(bool showFeedback);
    void loadMqttSettings();
    void saveMqttSettings();
    void startOrStopLinkTest();
    void sendNextLinkTestMessage();
    void handleLinkTestMqttMessage(const QString &topic, const QByteArray &payload);
    void handleLinkTestSerialFrame(const QByteArray &frame);
    void finishLinkTest(bool success, const QString &detail);
    void setLinkTestControls(bool running);

    void readSerialData();
    void processReply(const QString &reply);
    void handleSerialError(QSerialPort::SerialPortError error);
    void appendLog(const QString &direction, const QString &message);
    QString operationName(Operation operation) const;
    QLabel *statusLabel(Operation operation) const;
    QString routerCommandPrefix(RouterCommand command) const;
    QString routerDefaultParameters(RouterCommand command) const;
    QString routerCommandText(RouterCommand command) const;
    QString routerCommandName(RouterCommand command) const;
    QLabel *routerStatusLabel(RouterCommand command) const;

    QSerialPort *m_serial = nullptr;
    QTimer *m_replyTimer = nullptr;
    QTimer *m_serialFrameTimer = nullptr;
    QTimer *m_linkTestDeadlineTimer = nullptr;
    QTimer *m_linkTestStepTimer = nullptr;
    MqttClient *m_mqtt = nullptr;
    QByteArray m_receiveBuffer;
    QByteArray m_serialFrameBuffer;
    QString m_lastMqttError;
    Operation m_pendingOperation = Operation::Count;
    RouterCommand m_pendingRouterCommand = RouterCommand::Count;
    bool m_batchRunning = false;
    int m_batchIndex = -1;
    bool m_productionBatchRunning = false;
    int m_productionBatchIndex = -1;
    LinkTestStep m_linkTestStep = LinkTestStep::Idle;
    QString m_linkTestSessionId;
    QByteArray m_linkTestExpectedPayload;
    int m_linkTestRound = 0;
    int m_linkTestSerialToMqttSuccesses = 0;
    int m_linkTestMqttToSerialSuccesses = 0;
    qint64 m_linkTestStartedAtMs = 0;

    QComboBox *m_portCombo = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_saveConfigurationButton = nullptr;
    QPushButton *m_clearStatesButton = nullptr;
    QLabel *m_connectionStatus = nullptr;
    QLineEdit *m_passwordInput = nullptr;
    QLineEdit *m_initialTimeInput = nullptr;
    QLineEdit *m_uidInput = nullptr;
    std::array<QPushButton *, operationCount> m_actionButtons{};
    std::array<QLabel *, operationCount> m_statusLabels{};
    QPushButton *m_productionBatchButton = nullptr;
    QLabel *m_productionBatchStatus = nullptr;
    std::array<QPushButton *, routerCommandCount> m_routerButtons{};
    std::array<QLineEdit *, routerCommandCount> m_routerParameterInputs{};
    std::array<QLabel *, routerCommandCount> m_routerStatusLabels{};
    QPushButton *m_batchButton = nullptr;
    QLabel *m_batchStatus = nullptr;
    QLineEdit *m_mqttHostInput = nullptr;
    QSpinBox *m_mqttPortInput = nullptr;
    QLineEdit *m_mqttClientIdInput = nullptr;
    QLineEdit *m_mqttUsernameInput = nullptr;
    QLineEdit *m_mqttPasswordInput = nullptr;
    QLineEdit *m_mqttSubscribeTopicInput = nullptr;
    QLineEdit *m_mqttPublishTopicInput = nullptr;
    QComboBox *m_serialLineEndingCombo = nullptr;
    QPushButton *m_mqttConnectButton = nullptr;
    QPushButton *m_mqttSubscribeButton = nullptr;
    QPushButton *m_mqttPublishButton = nullptr;
    QPushButton *m_serialMessageButton = nullptr;
    QPushButton *m_linkTestButton = nullptr;
    QSpinBox *m_linkTestDurationInput = nullptr;
    QLabel *m_mqttStatus = nullptr;
    QLabel *m_linkTestStatus = nullptr;
    QPlainTextEdit *m_mqttPayloadInput = nullptr;
    QPlainTextEdit *m_messageLog = nullptr;
    QTabWidget *m_tabs = nullptr;
    QFrame *m_serialLogCard = nullptr;
    QPlainTextEdit *m_log = nullptr;
};
