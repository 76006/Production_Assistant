#include "mainwindow.h"

#include "mqttclient.h"
#include "serialprotocol.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSerialPortInfo>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace {
constexpr int replyTimeoutMs = 3000;
constexpr int routerReplyTimeoutMs = 5000;

int operationIndex(int operation)
{
    return operation;
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_serial(new QSerialPort(this))
    , m_replyTimer(new QTimer(this))
    , m_serialFrameTimer(new QTimer(this))
    , m_linkTestDeadlineTimer(new QTimer(this))
    , m_linkTestStepTimer(new QTimer(this))
    , m_mqtt(new MqttClient(this))
{
    buildUi();
    applyStyle();
    loadMqttSettings();
    loadUserConfiguration();

    m_replyTimer->setSingleShot(true);
    m_replyTimer->setInterval(replyTimeoutMs);
    m_serialFrameTimer->setSingleShot(true);
    m_serialFrameTimer->setInterval(50);
    m_linkTestDeadlineTimer->setSingleShot(true);
    m_linkTestStepTimer->setSingleShot(true);
    m_linkTestStepTimer->setInterval(3000);

    connect(m_refreshButton, &QPushButton::clicked, this, &MainWindow::refreshPorts);
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::toggleConnection);
    connect(m_portCombo->lineEdit(), &QLineEdit::textChanged, this, [this] {
        if (!m_serial->isOpen())
            m_connectButton->setEnabled(!currentPortName().isEmpty());
    });
    connect(m_saveConfigurationButton, &QPushButton::clicked, this,
            [this] { saveUserConfiguration(true); });
    connect(m_initialTimeInput, &QLineEdit::textChanged, this,
            [this] { saveUserConfiguration(false); });
    for (QLineEdit *input : m_routerParameterInputs) {
        connect(input, &QLineEdit::textChanged, this,
                [this] { saveUserConfiguration(false); });
    }
    connect(m_mqttSubscribeTopicInput, &QLineEdit::textChanged, this,
            [this] { saveUserConfiguration(false); });
    connect(m_mqttPublishTopicInput, &QLineEdit::textChanged, this,
            [this] { saveUserConfiguration(false); });
    connect(m_mqttHostInput, &QLineEdit::textChanged, this,
            [this] { saveUserConfiguration(false); });
    connect(m_mqttPortInput, &QSpinBox::valueChanged, this,
            [this] { saveUserConfiguration(false); });
    connect(m_mqttClientIdInput, &QLineEdit::textChanged, this,
            [this] { saveUserConfiguration(false); });
    connect(m_actionButtons[operationIndex(static_cast<int>(Operation::Password))], &QPushButton::clicked,
            this, &MainWindow::sendPassword);
    connect(m_actionButtons[operationIndex(static_cast<int>(Operation::InitialTime))], &QPushButton::clicked,
            this, &MainWindow::sendInitialTime);
    connect(m_actionButtons[operationIndex(static_cast<int>(Operation::UidCheck))], &QPushButton::clicked,
            this, &MainWindow::sendUidCheck);
    connect(m_actionButtons[operationIndex(static_cast<int>(Operation::Uid))], &QPushButton::clicked,
            this, &MainWindow::sendUidCommand);
    connect(m_actionButtons[operationIndex(static_cast<int>(Operation::Time))], &QPushButton::clicked,
            this, &MainWindow::sendCurrentTime);
    connect(m_actionButtons[operationIndex(static_cast<int>(Operation::Restart))], &QPushButton::clicked,
            this, &MainWindow::sendRestart);
    connect(m_productionBatchButton, &QPushButton::clicked,
            this, &MainWindow::startProductionConfiguration);
    for (int i = 0; i < routerCommandCount; ++i) {
        const RouterCommand command = static_cast<RouterCommand>(i);
        connect(m_routerButtons[i], &QPushButton::clicked, this,
                [this, command] { sendRouterCommand(command); });
    }
    connect(m_batchButton, &QPushButton::clicked, this, &MainWindow::startOneClickConfiguration);
    connect(m_mqttConnectButton, &QPushButton::clicked, this, &MainWindow::toggleMqttConnection);
    connect(m_mqttSubscribeButton, &QPushButton::clicked, this, &MainWindow::subscribeMqttTopic);
    connect(m_mqttPublishButton, &QPushButton::clicked, this, &MainWindow::publishMqttMessage);
    connect(m_serialMessageButton, &QPushButton::clicked, this, &MainWindow::sendSerialMessage);
    connect(m_linkTestButton, &QPushButton::clicked, this, &MainWindow::startOrStopLinkTest);
    connect(m_mqtt, &MqttClient::connected, this, [this] {
        m_lastMqttError.clear();
        setMqttUiConnected(true, QStringLiteral("已连接 Broker"));
        saveMqttSettings();
        appendMessageRecord(QStringLiteral("MQTT 状态"), QString(), QByteArrayLiteral("Broker 已连接"));
        if (!m_mqttSubscribeTopicInput->text().trimmed().isEmpty()) {
            m_mqtt->subscribe(m_mqttSubscribeTopicInput->text().trimmed());
            m_mqttStatus->setText(QStringLiteral("● 已连接，正在订阅…"));
        }
    });
    connect(m_mqtt, &MqttClient::disconnected, this, [this] {
        if (m_linkTestStep != LinkTestStep::Idle)
            finishLinkTest(false, QStringLiteral("MQTT 连接在测试期间断开"));
        const QString detail = m_lastMqttError.isEmpty()
                                   ? QStringLiteral("连接已断开")
                                   : QStringLiteral("连接失败：%1").arg(m_lastMqttError);
        setMqttUiConnected(false, detail);
        appendMessageRecord(QStringLiteral("MQTT 状态"), QString(), QByteArrayLiteral("Broker 连接已断开"));
    });
    connect(m_mqtt, &MqttClient::errorOccurred, this, [this](const QString &message) {
        m_lastMqttError = message;
        m_mqttStatus->setProperty("state", QStringLiteral("failure"));
        m_mqttStatus->setText(QStringLiteral("● MQTT 错误：%1").arg(message));
        m_mqttStatus->style()->unpolish(m_mqttStatus);
        m_mqttStatus->style()->polish(m_mqttStatus);
        appendMessageRecord(QStringLiteral("MQTT 错误"), QString(), message.toUtf8());
    });
    connect(m_mqtt, &MqttClient::subscribed, this, [this](const QString &topic) {
        m_mqttStatus->setProperty("state", QStringLiteral("success"));
        m_mqttStatus->setText(QStringLiteral("● 已连接并订阅：%1").arg(topic));
        m_mqttStatus->style()->unpolish(m_mqttStatus);
        m_mqttStatus->style()->polish(m_mqttStatus);
        appendMessageRecord(QStringLiteral("MQTT 状态"), topic, QByteArrayLiteral("订阅成功"));
        if (m_linkTestStep == LinkTestStep::PreparingSubscription
            && topic == m_mqttSubscribeTopicInput->text().trimmed()) {
            m_linkTestStepTimer->stop();
            m_linkTestStep = LinkTestStep::SerialToMqtt;
            sendNextLinkTestMessage();
        }
    });
    connect(m_mqtt, &MqttClient::messageReceived, this,
            [this](const QString &topic, const QByteArray &payload) {
                appendMessageRecord(QStringLiteral("MQTT 接收"), topic, payload);
                handleLinkTestMqttMessage(topic, payload);
            });
    connect(m_serial, &QSerialPort::readyRead, this, &MainWindow::readSerialData);
    connect(m_serial, &QSerialPort::errorOccurred, this, &MainWindow::handleSerialError);
    connect(m_replyTimer, &QTimer::timeout, this, [this] {
        if (m_pendingRouterCommand != RouterCommand::Count) {
            finishRouterCommand(false, QStringLiteral("未收到大写 OK（5 秒）"));
            return;
        }
        const QString detail = (m_pendingOperation == Operation::Uid
                                || m_pendingOperation == Operation::UidCheck)
                                   ? QStringLiteral("设备无返回（3 秒）")
                                   : QStringLiteral("等待设备响应超时（3 秒）");
        finishOperation(false, detail);
    });
    connect(m_serialFrameTimer, &QTimer::timeout, this, [this] {
        if (m_pendingOperation == Operation::UidCheck && !m_receiveBuffer.isEmpty()) {
            const QString reply = QString::fromUtf8(m_receiveBuffer).trimmed();
            m_receiveBuffer.clear();
            if (!reply.isEmpty())
                processReply(reply);
            return;
        }
        if (m_serialFrameBuffer.isEmpty())
            return;
        const QByteArray frame = m_serialFrameBuffer;
        m_serialFrameBuffer.clear();
        appendMessageRecord(QStringLiteral("串口接收"), QString(), frame);
        handleLinkTestSerialFrame(frame);
    });
    connect(m_linkTestDeadlineTimer, &QTimer::timeout, this, [this] {
        const bool passedBothDirections = m_linkTestSerialToMqttSuccesses > 0
                                          && m_linkTestMqttToSerialSuccesses > 0;
        finishLinkTest(passedBothDirections,
                       passedBothDirections
                           ? QStringLiteral("测试时间结束，双向链路均正常")
                           : QStringLiteral("测试时间结束，未完成双向闭环"));
    });
    connect(m_linkTestStepTimer, &QTimer::timeout, this, [this] {
        if (m_linkTestStep == LinkTestStep::PreparingSubscription) {
            finishLinkTest(false, QStringLiteral("准备失败：3 秒内未确认 MQTT 订阅"));
        } else if (m_linkTestStep == LinkTestStep::SerialToMqtt) {
            finishLinkTest(false, QStringLiteral("上行失败：串口已发送，但 3 秒内 MQTT 未收到"));
        } else if (m_linkTestStep == LinkTestStep::MqttToSerial) {
            finishLinkTest(false, QStringLiteral("下行失败：MQTT 已发布，但 3 秒内串口未收到"));
        }
    });

    refreshPorts();
    setConnectionUi(false);
    setMqttUiConnected(false);
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("生产助手"));
    setMinimumSize(900, 720);
    resize(1050, 900);

    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("central"));
    auto *pageLayout = new QVBoxLayout(central);
    pageLayout->setContentsMargins(32, 26, 32, 28);
    pageLayout->setSpacing(18);

    auto *title = new QLabel(QStringLiteral("生产助手"), central);
    title->setObjectName(QStringLiteral("title"));
    auto *subtitle = new QLabel(QStringLiteral("连接设备后，按生产流程依次执行操作并核对右侧结果"), central);
    subtitle->setObjectName(QStringLiteral("subtitle"));
    pageLayout->addWidget(title);
    pageLayout->addWidget(subtitle);

    auto *connectionCard = new QFrame(central);
    connectionCard->setObjectName(QStringLiteral("card"));
    auto *connectionLayout = new QHBoxLayout(connectionCard);
    connectionLayout->setContentsMargins(20, 16, 20, 16);
    connectionLayout->setSpacing(10);

    auto *portLabel = new QLabel(QStringLiteral("串口"), connectionCard);
    m_portCombo = new QComboBox(connectionCard);
    m_portCombo->setEditable(true);
    m_portCombo->setInsertPolicy(QComboBox::NoInsert);
    m_portCombo->lineEdit()->setPlaceholderText(QStringLiteral("选择或输入串口，如 COM12"));
    m_portCombo->setToolTip(QStringLiteral("可从列表选择，也可以直接输入串口名称"));
    m_portCombo->setMinimumWidth(220);
    auto *baudLabel = new QLabel(QStringLiteral("波特率"), connectionCard);
    m_baudCombo = new QComboBox(connectionCard);
    m_baudCombo->addItems({QStringLiteral("9600"), QStringLiteral("19200"), QStringLiteral("38400"),
                           QStringLiteral("57600"), QStringLiteral("115200"),
                           QStringLiteral("460800")});
    m_baudCombo->setCurrentText(QStringLiteral("115200"));
    m_refreshButton = new QPushButton(QStringLiteral("刷新"), connectionCard);
    m_refreshButton->setObjectName(QStringLiteral("secondaryButton"));
    m_connectButton = new QPushButton(QStringLiteral("连接设备"), connectionCard);
    m_connectButton->setObjectName(QStringLiteral("primaryButton"));
    m_saveConfigurationButton = new QPushButton(QStringLiteral("保存配置"), connectionCard);
    m_saveConfigurationButton->setObjectName(QStringLiteral("secondaryButton"));
    m_saveConfigurationButton->setToolTip(
        QStringLiteral("保存到程序目录：%1").arg(configurationFilePath()));
    m_connectionStatus = new QLabel(connectionCard);
    m_connectionStatus->setObjectName(QStringLiteral("connectionStatus"));

    connectionLayout->addWidget(portLabel);
    connectionLayout->addWidget(m_portCombo, 1);
    connectionLayout->addWidget(baudLabel);
    connectionLayout->addWidget(m_baudCombo);
    connectionLayout->addWidget(m_refreshButton);
    connectionLayout->addWidget(m_connectButton);
    connectionLayout->addWidget(m_saveConfigurationButton);
    connectionLayout->addSpacing(6);
    connectionLayout->addWidget(m_connectionStatus);
    pageLayout->addWidget(connectionCard);

    m_tabs = new QTabWidget(central);
    m_tabs->setObjectName(QStringLiteral("appTabs"));
    m_tabs->setDocumentMode(true);

    auto *productionPage = new QWidget(m_tabs);
    auto *productionLayout = new QVBoxLayout(productionPage);
    productionLayout->setContentsMargins(4, 14, 4, 4);

    auto *operationsCard = new QFrame(productionPage);
    operationsCard->setObjectName(QStringLiteral("card"));
    auto *grid = new QGridLayout(operationsCard);
    grid->setContentsMargins(20, 18, 20, 20);
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(12);
    grid->setColumnStretch(0, 3);
    grid->setColumnStretch(1, 5);

    auto *operationHeader = new QLabel(QStringLiteral("操作 / 输入"), operationsCard);
    auto *statusHeader = new QLabel(QStringLiteral("状态"), operationsCard);
    operationHeader->setObjectName(QStringLiteral("sectionHeader"));
    statusHeader->setObjectName(QStringLiteral("sectionHeader"));
    grid->addWidget(operationHeader, 0, 0);
    grid->addWidget(statusHeader, 0, 1);

    const QStringList actionNames = {
        QStringLiteral("发送密码"),
        QStringLiteral("配置初始时间"),
        QStringLiteral("读取 UID 设置状态"),
        QStringLiteral("发送 UID"),
        QStringLiteral("配置为当前时间"),
        QStringLiteral("重启")
    };

    for (int i = 0; i < operationCount; ++i) {
        auto *button = new QPushButton(actionNames.at(i), operationsCard);
        button->setObjectName(QStringLiteral("actionButton"));
        button->setMinimumHeight(52);
        button->setCursor(Qt::PointingHandCursor);
        m_actionButtons[i] = button;

        auto *status = new QLabel(QStringLiteral("●  等待操作"), operationsCard);
        status->setObjectName(QStringLiteral("operationStatus"));
        status->setProperty("state", QStringLiteral("idle"));
        status->setMinimumHeight(52);
        status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_statusLabels[i] = status;

        if (i == static_cast<int>(Operation::Password)
            || i == static_cast<int>(Operation::InitialTime)
            || i == static_cast<int>(Operation::Uid)) {
            auto *inputEditor = new QWidget(operationsCard);
            auto *inputLayout = new QHBoxLayout(inputEditor);
            inputLayout->setContentsMargins(0, 0, 0, 0);
            inputLayout->setSpacing(10);
            button->setMinimumWidth(142);

            auto *input = new QLineEdit(inputEditor);
            input->setObjectName(QStringLiteral("commandInput"));
            input->setMinimumHeight(50);
            input->setClearButtonEnabled(true);
            if (i == static_cast<int>(Operation::Password)) {
                m_passwordInput = input;
                input->setEchoMode(QLineEdit::Password);
                input->setPlaceholderText(QStringLiteral("首次请手动输入密码"));
            } else if (i == static_cast<int>(Operation::InitialTime)) {
                m_initialTimeInput = input;
                input->setText(QStringLiteral("date 2025 10 30 0 0 0"));
                input->setPlaceholderText(QStringLiteral("输入完整初始时间指令"));
            } else {
                m_uidInput = input;
                input->setPlaceholderText(QStringLiteral("输入要原样发送的 UID 内容"));
            }
            connect(input, &QLineEdit::returnPressed, button, &QPushButton::click);
            inputLayout->addWidget(button);
            inputLayout->addWidget(input, 1);
            grid->addWidget(inputEditor, i + 1, 0);
        } else {
            grid->addWidget(button, i + 1, 0);
        }
        grid->addWidget(status, i + 1, 1);
    }

    const int productionBatchRow = operationCount + 1;
    auto *productionDivider = new QFrame(operationsCard);
    productionDivider->setObjectName(QStringLiteral("divider"));
    productionDivider->setFrameShape(QFrame::HLine);
    grid->addWidget(productionDivider, productionBatchRow, 0, 1, 2);
    m_productionBatchButton = new QPushButton(QStringLiteral("一键配置并重启"), operationsCard);
    m_productionBatchButton->setObjectName(QStringLiteral("oneClickButton"));
    m_productionBatchButton->setMinimumHeight(52);
    m_productionBatchButton->setCursor(Qt::PointingHandCursor);
    m_productionBatchStatus = new QLabel(QStringLiteral("●  等待操作"), operationsCard);
    m_productionBatchStatus->setObjectName(QStringLiteral("operationStatus"));
    m_productionBatchStatus->setProperty("state", QStringLiteral("idle"));
    m_productionBatchStatus->setMinimumHeight(52);
    m_productionBatchStatus->setWordWrap(true);
    grid->addWidget(m_productionBatchButton, productionBatchRow + 1, 0);
    grid->addWidget(m_productionBatchStatus, productionBatchRow + 1, 1);
    productionLayout->addWidget(operationsCard);
    productionLayout->addStretch();

    auto *routerPage = new QWidget(m_tabs);
    auto *routerPageLayout = new QVBoxLayout(routerPage);
    routerPageLayout->setContentsMargins(4, 14, 4, 4);

    auto *routerCard = new QFrame(routerPage);
    routerCard->setObjectName(QStringLiteral("card"));
    auto *routerGrid = new QGridLayout(routerCard);
    routerGrid->setContentsMargins(20, 18, 20, 20);
    routerGrid->setHorizontalSpacing(14);
    routerGrid->setVerticalSpacing(12);
    routerGrid->setColumnStretch(0, 0);
    routerGrid->setColumnStretch(1, 5);
    routerGrid->setColumnStretch(2, 4);

    auto *routerCommandHeader = new QLabel(QStringLiteral("操作"), routerCard);
    auto *routerParametersHeader = new QLabel(
        QStringLiteral("参数信息（自动与左侧命令头组合）"), routerCard);
    auto *routerStatusHeader = new QLabel(QStringLiteral("状态（必须返回大写 OK）"), routerCard);
    routerCommandHeader->setObjectName(QStringLiteral("sectionHeader"));
    routerParametersHeader->setObjectName(QStringLiteral("sectionHeader"));
    routerStatusHeader->setObjectName(QStringLiteral("sectionHeader"));
    routerGrid->addWidget(routerCommandHeader, 0, 0);
    routerGrid->addWidget(routerParametersHeader, 0, 1);
    routerGrid->addWidget(routerStatusHeader, 0, 2);

    const QStringList routerButtonNames = {
        QStringLiteral("发送 GPS 配置"),
        QStringLiteral("发送服务器配置"),
        QStringLiteral("发送 MQTT 参数"),
        QStringLiteral("发送 MQTT 主题")
    };

    for (int i = 0; i < routerCommandCount; ++i) {
        const RouterCommand command = static_cast<RouterCommand>(i);
        auto *button = new QPushButton(routerButtonNames.at(i), routerCard);
        button->setObjectName(QStringLiteral("routerCommandButton"));
        button->setMinimumHeight(50);
        button->setMinimumWidth(160);
        button->setCursor(Qt::PointingHandCursor);
        m_routerButtons[i] = button;

        auto *parameterEditor = new QWidget(routerCard);
        auto *parameterLayout = new QHBoxLayout(parameterEditor);
        parameterLayout->setContentsMargins(0, 0, 0, 0);
        parameterLayout->setSpacing(0);
        auto *prefix = new QLabel(routerCommandPrefix(command), parameterEditor);
        prefix->setObjectName(QStringLiteral("commandPrefix"));
        prefix->setMinimumHeight(50);
        auto *parameters = new QLineEdit(parameterEditor);
        parameters->setObjectName(QStringLiteral("routerParameterInput"));
        parameters->setMinimumHeight(50);
        parameters->setClearButtonEnabled(true);
        parameters->setText(routerDefaultParameters(command));
        parameters->setPlaceholderText(QStringLiteral("输入命令参数"));
        connect(parameters, &QLineEdit::returnPressed, button, &QPushButton::click);
        parameterLayout->addWidget(prefix);
        parameterLayout->addWidget(parameters, 1);
        m_routerParameterInputs[i] = parameters;

        auto *status = new QLabel(QStringLiteral("●  等待操作"), routerCard);
        status->setObjectName(QStringLiteral("operationStatus"));
        status->setProperty("state", QStringLiteral("idle"));
        status->setMinimumHeight(50);
        status->setWordWrap(true);
        status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_routerStatusLabels[i] = status;

        routerGrid->addWidget(button, i + 1, 0);
        routerGrid->addWidget(parameterEditor, i + 1, 1);
        routerGrid->addWidget(status, i + 1, 2);
    }

    const int batchRow = routerCommandCount + 1;
    auto *divider = new QFrame(routerCard);
    divider->setObjectName(QStringLiteral("divider"));
    divider->setFrameShape(QFrame::HLine);
    routerGrid->addWidget(divider, batchRow, 0, 1, 3);

    m_batchButton = new QPushButton(QStringLiteral("一键配置全部指令"), routerCard);
    m_batchButton->setObjectName(QStringLiteral("oneClickButton"));
    m_batchButton->setMinimumHeight(52);
    m_batchButton->setCursor(Qt::PointingHandCursor);
    m_batchStatus = new QLabel(QStringLiteral("●  等待操作"), routerCard);
    m_batchStatus->setObjectName(QStringLiteral("operationStatus"));
    m_batchStatus->setProperty("state", QStringLiteral("idle"));
    m_batchStatus->setMinimumHeight(52);
    routerGrid->addWidget(m_batchButton, batchRow + 1, 0, 1, 2);
    routerGrid->addWidget(m_batchStatus, batchRow + 1, 2);

    routerPageLayout->addWidget(routerCard);
    routerPageLayout->addStretch();

    auto *mqttPage = new QWidget(m_tabs);
    auto *mqttPageLayout = new QVBoxLayout(mqttPage);
    mqttPageLayout->setContentsMargins(4, 14, 4, 4);
    mqttPageLayout->setSpacing(12);

    auto *mqttConnectionCard = new QFrame(mqttPage);
    mqttConnectionCard->setObjectName(QStringLiteral("card"));
    auto *mqttConnectionGrid = new QGridLayout(mqttConnectionCard);
    mqttConnectionGrid->setContentsMargins(18, 14, 18, 16);
    mqttConnectionGrid->setHorizontalSpacing(10);
    mqttConnectionGrid->setVerticalSpacing(10);
    mqttConnectionGrid->setColumnStretch(1, 3);
    mqttConnectionGrid->setColumnStretch(3, 2);
    mqttConnectionGrid->setColumnStretch(5, 3);

    auto *mqttConnectionHeader = new QLabel(QStringLiteral("MQTT Broker 配置"), mqttConnectionCard);
    mqttConnectionHeader->setObjectName(QStringLiteral("sectionHeader"));
    m_mqttStatus = new QLabel(QStringLiteral("● 未连接"), mqttConnectionCard);
    m_mqttStatus->setObjectName(QStringLiteral("mqttStatus"));
    m_mqttStatus->setProperty("state", QStringLiteral("idle"));
    mqttConnectionGrid->addWidget(mqttConnectionHeader, 0, 0, 1, 2);
    mqttConnectionGrid->addWidget(m_mqttStatus, 0, 2, 1, 5, Qt::AlignRight);

    m_mqttHostInput = new QLineEdit(mqttConnectionCard);
    m_mqttHostInput->setPlaceholderText(QStringLiteral("broker.emqx.io"));
    m_mqttPortInput = new QSpinBox(mqttConnectionCard);
    m_mqttPortInput->setRange(1, 65535);
    m_mqttPortInput->setValue(1883);
    m_mqttClientIdInput = new QLineEdit(mqttConnectionCard);
    m_mqttClientIdInput->setPlaceholderText(QStringLiteral("ProductionAssistant-PC"));
    mqttConnectionGrid->addWidget(new QLabel(QStringLiteral("Broker"), mqttConnectionCard), 1, 0);
    mqttConnectionGrid->addWidget(m_mqttHostInput, 1, 1);
    mqttConnectionGrid->addWidget(new QLabel(QStringLiteral("端口"), mqttConnectionCard), 1, 2);
    mqttConnectionGrid->addWidget(m_mqttPortInput, 1, 3);
    mqttConnectionGrid->addWidget(new QLabel(QStringLiteral("Client ID"), mqttConnectionCard), 1, 4);
    mqttConnectionGrid->addWidget(m_mqttClientIdInput, 1, 5, 1, 2);

    m_mqttUsernameInput = new QLineEdit(mqttConnectionCard);
    m_mqttUsernameInput->setPlaceholderText(QStringLiteral("可选"));
    m_mqttPasswordInput = new QLineEdit(mqttConnectionCard);
    m_mqttPasswordInput->setEchoMode(QLineEdit::Password);
    m_mqttPasswordInput->setPlaceholderText(QStringLiteral("可选，不保存"));
    m_mqttConnectButton = new QPushButton(QStringLiteral("连接 MQTT"), mqttConnectionCard);
    m_mqttConnectButton->setObjectName(QStringLiteral("primaryButton"));
    mqttConnectionGrid->addWidget(new QLabel(QStringLiteral("用户名"), mqttConnectionCard), 2, 0);
    mqttConnectionGrid->addWidget(m_mqttUsernameInput, 2, 1, 1, 2);
    mqttConnectionGrid->addWidget(new QLabel(QStringLiteral("密码"), mqttConnectionCard), 2, 3);
    mqttConnectionGrid->addWidget(m_mqttPasswordInput, 2, 4, 1, 2);
    mqttConnectionGrid->addWidget(m_mqttConnectButton, 2, 6);

    auto *mqttMessagingCard = new QFrame(mqttPage);
    mqttMessagingCard->setObjectName(QStringLiteral("card"));
    auto *mqttMessagingGrid = new QGridLayout(mqttMessagingCard);
    mqttMessagingGrid->setContentsMargins(18, 14, 18, 16);
    mqttMessagingGrid->setHorizontalSpacing(10);
    mqttMessagingGrid->setVerticalSpacing(10);
    mqttMessagingGrid->setColumnStretch(1, 1);

    auto *topicsHeader = new QLabel(QStringLiteral("主题与消息"), mqttMessagingCard);
    topicsHeader->setObjectName(QStringLiteral("sectionHeader"));
    mqttMessagingGrid->addWidget(topicsHeader, 0, 0, 1, 4);

    m_mqttSubscribeTopicInput = new QLineEdit(mqttMessagingCard);
    m_mqttSubscribeTopicInput->setPlaceholderText(QStringLiteral("接收路由器发布消息的主题"));
    m_mqttSubscribeButton = new QPushButton(QStringLiteral("订阅"), mqttMessagingCard);
    m_mqttSubscribeButton->setObjectName(QStringLiteral("secondaryButton"));
    mqttMessagingGrid->addWidget(new QLabel(QStringLiteral("订阅主题（路由器发布）"), mqttMessagingCard), 1, 0);
    mqttMessagingGrid->addWidget(m_mqttSubscribeTopicInput, 1, 1, 1, 2);
    mqttMessagingGrid->addWidget(m_mqttSubscribeButton, 1, 3);

    m_mqttPublishTopicInput = new QLineEdit(mqttMessagingCard);
    m_mqttPublishTopicInput->setPlaceholderText(QStringLiteral("向路由器发送消息的主题"));
    mqttMessagingGrid->addWidget(new QLabel(QStringLiteral("发布主题（路由器订阅）"), mqttMessagingCard), 2, 0);
    mqttMessagingGrid->addWidget(m_mqttPublishTopicInput, 2, 1, 1, 3);

    m_mqttPayloadInput = new QPlainTextEdit(mqttMessagingCard);
    m_mqttPayloadInput->setPlaceholderText(QStringLiteral("输入消息内容；可选择发布到 MQTT，或原样通过串口发送给 4G 路由器"));
    m_mqttPayloadInput->setMaximumHeight(82);
    mqttMessagingGrid->addWidget(new QLabel(QStringLiteral("消息内容"), mqttMessagingCard), 3, 0, Qt::AlignTop);
    mqttMessagingGrid->addWidget(m_mqttPayloadInput, 3, 1, 1, 3);

    auto *sendButtonLayout = new QHBoxLayout;
    m_serialLineEndingCombo = new QComboBox(mqttMessagingCard);
    m_serialLineEndingCombo->addItem(QStringLiteral("串口不加结尾"), QStringLiteral("none"));
    m_serialLineEndingCombo->addItem(QStringLiteral("串口添加 CRLF"), QStringLiteral("crlf"));
    m_serialLineEndingCombo->addItem(QStringLiteral("串口添加 LF"), QStringLiteral("lf"));
    m_serialMessageButton = new QPushButton(QStringLiteral("通过串口发送"), mqttMessagingCard);
    m_serialMessageButton->setObjectName(QStringLiteral("secondaryButton"));
    m_mqttPublishButton = new QPushButton(QStringLiteral("发布到 MQTT"), mqttMessagingCard);
    m_mqttPublishButton->setObjectName(QStringLiteral("primaryButton"));
    sendButtonLayout->addWidget(m_serialLineEndingCombo);
    sendButtonLayout->addStretch();
    sendButtonLayout->addWidget(m_serialMessageButton);
    sendButtonLayout->addWidget(m_mqttPublishButton);
    mqttMessagingGrid->addLayout(sendButtonLayout, 4, 1, 1, 3);

    auto *linkTestLayout = new QHBoxLayout;
    m_linkTestButton = new QPushButton(QStringLiteral("一键连接测试"), mqttMessagingCard);
    m_linkTestButton->setObjectName(QStringLiteral("oneClickButton"));
    m_linkTestButton->setMinimumHeight(42);
    m_linkTestDurationInput = new QSpinBox(mqttMessagingCard);
    m_linkTestDurationInput->setRange(5, 600);
    m_linkTestDurationInput->setValue(10);
    m_linkTestDurationInput->setSuffix(QStringLiteral(" s"));
    m_linkTestDurationInput->setMinimumWidth(90);
    m_linkTestStatus = new QLabel(QStringLiteral("●  等待测试"), mqttMessagingCard);
    m_linkTestStatus->setObjectName(QStringLiteral("operationStatus"));
    m_linkTestStatus->setProperty("state", QStringLiteral("idle"));
    m_linkTestStatus->setMinimumHeight(42);
    m_linkTestStatus->setWordWrap(true);
    linkTestLayout->addWidget(m_linkTestButton);
    linkTestLayout->addWidget(new QLabel(QStringLiteral("测试时间"), mqttMessagingCard));
    linkTestLayout->addWidget(m_linkTestDurationInput);
    linkTestLayout->addWidget(m_linkTestStatus, 1);
    mqttMessagingGrid->addLayout(linkTestLayout, 5, 0, 1, 4);

    auto *messageHeaderLayout = new QHBoxLayout;
    auto *messageHeader = new QLabel(QStringLiteral("MQTT 与 4G 路由器链路记录"), mqttMessagingCard);
    messageHeader->setObjectName(QStringLiteral("sectionHeader"));
    auto *clearMessageButton = new QPushButton(QStringLiteral("清空"), mqttMessagingCard);
    clearMessageButton->setObjectName(QStringLiteral("linkButton"));
    messageHeaderLayout->addWidget(messageHeader);
    messageHeaderLayout->addStretch();
    messageHeaderLayout->addWidget(clearMessageButton);
    mqttMessagingGrid->addLayout(messageHeaderLayout, 6, 0, 1, 4);

    auto *messageLegend = new QLabel(
        QStringLiteral("主题方向：电脑发布 → sendev/1/temperature；电脑订阅 ← sendev/10/temperature。\n"
                       "方向判断：有“MQTT 发出”但没有“串口收到”＝下行链路异常；"
                       "有“串口发出”但没有“MQTT 收到”＝上行链路异常。"),
        mqttMessagingCard);
    messageLegend->setObjectName(QStringLiteral("messageLegend"));
    messageLegend->setWordWrap(true);
    mqttMessagingGrid->addWidget(messageLegend, 7, 0, 1, 4);

    m_messageLog = new QPlainTextEdit(mqttMessagingCard);
    m_messageLog->setReadOnly(true);
    m_messageLog->setMaximumBlockCount(500);
    m_messageLog->setMinimumHeight(120);
    m_messageLog->setPlaceholderText(QStringLiteral("MQTT 接收、MQTT 发送和串口收发消息会显示在这里"));
    connect(clearMessageButton, &QPushButton::clicked, this, [this] { m_messageLog->clear(); });
    mqttMessagingGrid->addWidget(m_messageLog, 8, 0, 1, 4);

    mqttPageLayout->addWidget(mqttConnectionCard);
    mqttPageLayout->addWidget(mqttMessagingCard, 1);

    m_tabs->addTab(productionPage, QStringLiteral("生产配置"));
    m_tabs->addTab(routerPage, QStringLiteral("4G 路由器配置与检测"));
    m_tabs->addTab(mqttPage, QStringLiteral("MQTT 收发"));
    pageLayout->addWidget(m_tabs, 3);

    m_serialLogCard = new QFrame(central);
    m_serialLogCard->setObjectName(QStringLiteral("card"));
    auto *logLayout = new QVBoxLayout(m_serialLogCard);
    logLayout->setContentsMargins(20, 14, 20, 18);
    logLayout->setSpacing(10);
    auto *logHeaderLayout = new QHBoxLayout;
    auto *logHeader = new QLabel(QStringLiteral("串口命令调试记录（生产配置 / 4G 配置）"), m_serialLogCard);
    logHeader->setObjectName(QStringLiteral("sectionHeader"));
    auto *clearLogButton = new QPushButton(QStringLiteral("清空"), m_serialLogCard);
    clearLogButton->setObjectName(QStringLiteral("linkButton"));
    connect(clearLogButton, &QPushButton::clicked, this, [this] { m_log->clear(); });
    logHeaderLayout->addWidget(logHeader);
    logHeaderLayout->addStretch();
    logHeaderLayout->addWidget(clearLogButton);
    m_log = new QPlainTextEdit(m_serialLogCard);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(300);
    m_log->setPlaceholderText(QStringLiteral("串口收发内容会显示在这里（密码不会记录）"));
    logLayout->addLayout(logHeaderLayout);
    logLayout->addWidget(m_log, 1);
    pageLayout->addWidget(m_serialLogCard, 1);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        // MQTT 页面已有专用的双向记录，避免与全局串口命令日志重复显示。
        m_serialLogCard->setVisible(index != 2);
    });

    setCentralWidget(central);
}

void MainWindow::applyStyle()
{
    setStyleSheet(QStringLiteral(R"(
        QWidget#central {
            background: #F4F7FB;
            color: #172033;
        }
        QLabel#title {
            font-size: 26px;
            font-weight: 700;
            color: #14213D;
        }
        QLabel#subtitle {
            color: #667085;
            font-size: 13px;
            margin-bottom: 2px;
        }
        QFrame#card {
            background: #FFFFFF;
            border: 1px solid #E4E9F2;
            border-radius: 10px;
        }
        QTabWidget#appTabs::pane {
            border: none;
            background: transparent;
        }
        QTabBar::tab {
            min-width: 180px;
            min-height: 38px;
            padding: 0 18px;
            color: #667085;
            background: transparent;
            border: none;
            border-bottom: 2px solid transparent;
            font-weight: 600;
        }
        QTabBar::tab:selected {
            color: #2F6FED;
            border-bottom-color: #2F6FED;
        }
        QTabBar::tab:hover { color: #245ED0; }
        QLabel#sectionHeader {
            color: #596579;
            font-size: 13px;
            font-weight: 600;
            padding: 1px 4px 4px 4px;
        }
        QComboBox, QLineEdit, QSpinBox {
            min-height: 36px;
            padding: 0 10px;
            border: 1px solid #CFD6E4;
            border-radius: 6px;
            background: #FFFFFF;
        }
        QComboBox:hover, QComboBox:focus, QLineEdit:hover, QLineEdit:focus,
        QSpinBox:hover, QSpinBox:focus {
            border-color: #2F6FED;
        }
        QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled {
            color: #98A2B3;
            background: #F2F4F7;
        }
        QPushButton {
            min-height: 36px;
            padding: 0 16px;
            border-radius: 6px;
            font-weight: 600;
        }
        QPushButton#primaryButton {
            color: #FFFFFF;
            background: #2F6FED;
            border: 1px solid #2F6FED;
        }
        QPushButton#primaryButton:hover { background: #245ED0; }
        QPushButton#secondaryButton {
            color: #344054;
            background: #FFFFFF;
            border: 1px solid #CFD6E4;
        }
        QPushButton#secondaryButton:hover { background: #F7F9FC; }
        QPushButton#actionButton {
            text-align: left;
            padding-left: 18px;
            color: #1E4FA3;
            background: #EEF4FF;
            border: 1px solid #C8D9FA;
            font-size: 14px;
        }
        QPushButton#actionButton:hover { background: #DFEAFF; }
        QPushButton#actionButton:pressed { background: #D0E0FF; }
        QPushButton#actionButton:disabled {
            color: #98A2B3;
            background: #F2F4F7;
            border-color: #E4E7EC;
        }
        QPushButton#routerCommandButton {
            text-align: left;
            padding-left: 18px;
            color: #17466F;
            background: #EFF8FF;
            border: 1px solid #B9E0F7;
            font-family: Consolas, "Microsoft YaHei UI";
            font-size: 13px;
        }
        QPushButton#routerCommandButton:hover { background: #DFF1FC; }
        QPushButton#routerCommandButton:disabled {
            color: #98A2B3;
            background: #F2F4F7;
            border-color: #E4E7EC;
        }
        QLabel#commandPrefix {
            padding: 0 9px;
            color: #475467;
            background: #F8FAFC;
            border: 1px solid #CFD6E4;
            border-right: none;
            border-top-left-radius: 6px;
            border-bottom-left-radius: 6px;
            font-family: Consolas, "Microsoft YaHei UI";
            font-size: 12px;
        }
        QLabel#commandPrefix:disabled {
            color: #98A2B3;
            background: #F2F4F7;
        }
        QLineEdit#routerParameterInput {
            border-top-left-radius: 0;
            border-bottom-left-radius: 0;
            font-family: Consolas, "Microsoft YaHei UI";
            font-size: 12px;
        }
        QPushButton#oneClickButton {
            text-align: left;
            padding-left: 18px;
            color: #FFFFFF;
            background: #087A55;
            border: 1px solid #087A55;
            font-size: 14px;
        }
        QPushButton#oneClickButton:hover { background: #066746; }
        QPushButton#oneClickButton:disabled {
            color: #D0D5DD;
            background: #98A2B3;
            border-color: #98A2B3;
        }
        QFrame#divider {
            color: #E4E7EC;
            background: #E4E7EC;
            border: none;
            max-height: 1px;
        }
        QLineEdit#commandInput {
            padding: 0 12px;
            color: #172033;
            background: #FFFFFF;
            border: 1px solid #CFD6E4;
            border-radius: 6px;
            font-size: 13px;
        }
        QLineEdit#commandInput:hover, QLineEdit#commandInput:focus {
            border-color: #2F6FED;
        }
        QLineEdit#commandInput:disabled {
            color: #98A2B3;
            background: #F2F4F7;
        }
        QLabel#operationStatus {
            padding: 0 16px;
            border-radius: 6px;
            font-size: 14px;
        }
        QLabel#operationStatus[state="idle"] {
            color: #667085;
            background: #F8FAFC;
            border: 1px solid #E4E7EC;
        }
        QLabel#operationStatus[state="sending"] {
            color: #8A4B08;
            background: #FFF8E7;
            border: 1px solid #F5D58B;
        }
        QLabel#operationStatus[state="success"] {
            color: #087A55;
            background: #ECFDF3;
            border: 1px solid #A6F4C5;
        }
        QLabel#operationStatus[state="failure"] {
            color: #B42318;
            background: #FEF3F2;
            border: 1px solid #FECDCA;
        }
        QLabel#connectionStatus[state="offline"] { color: #B42318; font-weight: 600; }
        QLabel#connectionStatus[state="online"] { color: #087A55; font-weight: 600; }
        QLabel#mqttStatus { font-weight: 600; }
        QLabel#mqttStatus[state="idle"] { color: #667085; }
        QLabel#mqttStatus[state="sending"] { color: #8A4B08; }
        QLabel#mqttStatus[state="success"] { color: #087A55; }
        QLabel#mqttStatus[state="failure"] { color: #B42318; }
        QLabel#messageLegend {
            color: #596579;
            background: #F0F5FF;
            border: 1px solid #D6E4FF;
            border-radius: 5px;
            padding: 7px 10px;
            font-size: 12px;
        }
        QPushButton#linkButton {
            color: #667085;
            background: transparent;
            border: none;
            padding: 0 4px;
            min-height: 24px;
        }
        QPushButton#linkButton:hover { color: #2F6FED; }
        QPlainTextEdit {
            color: #344054;
            background: #F8FAFC;
            border: 1px solid #E4E7EC;
            border-radius: 6px;
            padding: 8px;
            font-family: Consolas, "Microsoft YaHei UI";
            font-size: 12px;
        }
    )"));
}

void MainWindow::refreshPorts()
{
    const QString previousPort = currentPortName();
    m_portCombo->clear();

    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports) {
        QString label = info.portName();
        if (!info.description().isEmpty())
            label += QStringLiteral(" — ") + info.description();
        m_portCombo->addItem(label, info.portName());
    }

    const int previousIndex = m_portCombo->findData(previousPort);
    if (previousIndex >= 0) {
        m_portCombo->setCurrentIndex(previousIndex);
    } else if (!previousPort.isEmpty()) {
        m_portCombo->setEditText(previousPort);
    } else if (m_portCombo->count() > 0) {
        m_portCombo->setCurrentIndex(0);
    } else {
        m_portCombo->setCurrentIndex(-1);
        m_portCombo->clearEditText();
    }

    m_connectButton->setEnabled(!currentPortName().isEmpty());
}

QString MainWindow::currentPortName() const
{
    const int index = m_portCombo->currentIndex();
    if (index >= 0 && m_portCombo->currentText() == m_portCombo->itemText(index)) {
        const QString detectedPort = m_portCombo->itemData(index).toString().trimmed();
        if (!detectedPort.isEmpty())
            return detectedPort;
    }
    return m_portCombo->currentText().trimmed();
}

void MainWindow::toggleConnection()
{
    if (m_serial->isOpen()) {
        if (m_linkTestStep != LinkTestStep::Idle)
            finishLinkTest(false, QStringLiteral("串口连接在测试期间断开"));
        if (m_pendingOperation != Operation::Count)
            finishOperation(false, QStringLiteral("连接已由操作员断开"));
        else if (m_productionBatchRunning)
            finishProductionConfiguration(false, QStringLiteral("连接已由操作员断开"));
        if (m_pendingRouterCommand != RouterCommand::Count)
            finishRouterCommand(false, QStringLiteral("连接已由操作员断开"));
        else if (m_batchRunning) {
            m_batchRunning = false;
            m_batchIndex = -1;
            setBatchState(UiState::Failure, QStringLiteral("连接已由操作员断开"));
            setActionsEnabled(true);
        }
        m_serial->close();
        m_serialFrameTimer->stop();
        m_serialFrameBuffer.clear();
        m_receiveBuffer.clear();
        setConnectionUi(false);
        appendLog(QStringLiteral("系统"), QStringLiteral("串口连接已断开"));
        return;
    }

    const QString portName = currentPortName();
    if (portName.isEmpty()) {
        setConnectionUi(false, QStringLiteral("没有可用串口"));
        return;
    }

    m_serial->setPortName(portName);
    m_serial->setBaudRate(m_baudCombo->currentText().toInt());
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serial->open(QIODevice::ReadWrite)) {
        setConnectionUi(false, m_serial->errorString());
        appendLog(QStringLiteral("错误"), QStringLiteral("连接 %1 失败：%2").arg(portName, m_serial->errorString()));
        return;
    }

    setConnectionUi(true, portName);
    appendLog(QStringLiteral("系统"), QStringLiteral("已连接 %1，波特率 %2").arg(portName, m_baudCombo->currentText()));
}

void MainWindow::setConnectionUi(bool connected, const QString &detail)
{
    m_portCombo->setEnabled(!connected);
    m_baudCombo->setEnabled(!connected);
    m_refreshButton->setEnabled(!connected);
    m_connectButton->setText(connected ? QStringLiteral("断开连接") : QStringLiteral("连接设备"));
    m_connectionStatus->setProperty("state", connected ? QStringLiteral("online") : QStringLiteral("offline"));
    if (connected) {
        m_connectionStatus->setText(QStringLiteral("● 已连接 %1").arg(detail));
    } else if (detail.isEmpty()) {
        m_connectionStatus->setText(QStringLiteral("● 未连接"));
    } else {
        m_connectionStatus->setText(QStringLiteral("● 连接失败：%1").arg(detail));
    }
    m_connectionStatus->style()->unpolish(m_connectionStatus);
    m_connectionStatus->style()->polish(m_connectionStatus);
    m_serialMessageButton->setEnabled(connected && m_pendingOperation == Operation::Count
                                      && m_pendingRouterCommand == RouterCommand::Count
                                      && !m_batchRunning && !m_productionBatchRunning
                                      && m_linkTestStep == LinkTestStep::Idle);
}

void MainWindow::sendPassword()
{
    const QString password = m_passwordInput->text().trimmed();
    if (password.isEmpty()) {
        setOperationState(Operation::Password, UiState::Failure, QStringLiteral("密码不能为空"));
        m_passwordInput->setFocus();
        return;
    }
    sendOperation(Operation::Password, SerialProtocol::passwordCommand(password),
                  QStringLiteral("密码：******"));
}

void MainWindow::sendInitialTime()
{
    const QString command = m_initialTimeInput->text();
    if (command.trimmed().isEmpty()) {
        setOperationState(Operation::InitialTime, UiState::Failure,
                          QStringLiteral("初始时间指令不能为空"));
        m_initialTimeInput->setFocus();
        return;
    }
    const QByteArray payload = SerialProtocol::initialTimeCommand(command);
    sendOperation(Operation::InitialTime, payload, QString::fromUtf8(payload).trimmed());
}

void MainWindow::sendUidCheck()
{
    sendOperation(Operation::UidCheck, SerialProtocol::uidCheckCommand(),
                  QStringLiteral("set_board_UID"));
}

void MainWindow::sendUidCommand()
{
    const QString command = m_uidInput->text();
    if (command.isEmpty()) {
        setOperationState(Operation::Uid, UiState::Failure, QStringLiteral("发送内容不能为空"));
        m_uidInput->setFocus();
        return;
    }
    const QByteArray payload = SerialProtocol::uidCommand(command);
    sendOperation(Operation::Uid, payload, QString::fromUtf8(payload));
}

void MainWindow::sendCurrentTime()
{
    const QByteArray payload = SerialProtocol::currentTimeCommand();
    sendOperation(Operation::Time, payload, QString::fromUtf8(payload).trimmed());
}

void MainWindow::sendRestart()
{
    const auto answer = QMessageBox::question(
        this, QStringLiteral("确认重启"), QStringLiteral("确定要向设备发送重启指令吗？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    sendOperation(Operation::Restart, SerialProtocol::restartCommand(), QStringLiteral("reset"));
}

void MainWindow::sendRouterCommand(RouterCommand command)
{
    const int index = static_cast<int>(command);
    if (index < 0 || index >= routerCommandCount
        || m_routerParameterInputs[index]->text().trimmed().isEmpty()) {
        setRouterState(command, UiState::Failure, QStringLiteral("参数信息不能为空"));
        if (index >= 0 && index < routerCommandCount)
            m_routerParameterInputs[index]->setFocus();
        return;
    }
    if (!m_serial->isOpen()) {
        setRouterState(command, UiState::Failure, QStringLiteral("串口未连接"));
        return;
    }
    if (m_pendingOperation != Operation::Count || m_pendingRouterCommand != RouterCommand::Count
        || m_batchRunning || m_productionBatchRunning
        || m_linkTestStep != LinkTestStep::Idle) {
        setRouterState(command, UiState::Failure, QStringLiteral("已有指令正在等待设备响应"));
        return;
    }

    transmitRouterCommand(command);
}

void MainWindow::startOneClickConfiguration()
{
    for (int i = 0; i < routerCommandCount; ++i) {
        if (m_routerParameterInputs[i]->text().trimmed().isEmpty()) {
            const RouterCommand command = static_cast<RouterCommand>(i);
            setRouterState(command, UiState::Failure, QStringLiteral("参数信息不能为空"));
            setBatchState(UiState::Failure,
                          QStringLiteral("%1 的参数信息为空").arg(routerCommandName(command)));
            m_routerParameterInputs[i]->setFocus();
            return;
        }
    }
    if (!m_serial->isOpen()) {
        setBatchState(UiState::Failure, QStringLiteral("串口未连接"));
        return;
    }
    if (m_pendingOperation != Operation::Count || m_pendingRouterCommand != RouterCommand::Count
        || m_batchRunning || m_productionBatchRunning
        || m_linkTestStep != LinkTestStep::Idle) {
        setBatchState(UiState::Failure, QStringLiteral("已有指令正在等待设备响应"));
        return;
    }

    for (int i = 0; i < routerCommandCount; ++i)
        setRouterState(static_cast<RouterCommand>(i), UiState::Idle);

    m_batchRunning = true;
    m_batchIndex = 0;
    setBatchState(UiState::Sending, QStringLiteral("准备发送第 1/4 项"));
    setActionsEnabled(false);
    transmitRouterCommand(RouterCommand::Gps);
}

void MainWindow::transmitRouterCommand(RouterCommand command)
{
    if (!m_serial->isOpen()) {
        setRouterState(command, UiState::Failure, QStringLiteral("串口未连接"));
        if (m_batchRunning) {
            m_batchRunning = false;
            m_batchIndex = -1;
            setBatchState(UiState::Failure, QStringLiteral("串口连接已断开"));
        }
        setActionsEnabled(true);
        return;
    }

    m_serialFrameTimer->stop();
    m_serialFrameBuffer.clear();
    m_receiveBuffer.clear();
    m_pendingRouterCommand = command;
    setRouterState(command, UiState::Sending, QStringLiteral("正在发送…"));
    setActionsEnabled(false);

    const QString commandText = routerCommandText(command);
    const QByteArray payload = SerialProtocol::withLineEnding(commandText.toUtf8());
    const qint64 bytesAccepted = m_serial->write(payload);
    if (bytesAccepted < 0) {
        finishRouterCommand(false, QStringLiteral("发送失败：%1").arg(m_serial->errorString()));
        return;
    }

    appendLog(QStringLiteral("发送"), commandText);
    setRouterState(command, UiState::Sending,
                   QStringLiteral("已发送 %1，等待大写 OK…").arg(commandText));
    if (m_batchRunning) {
        setBatchState(UiState::Sending,
                      QStringLiteral("正在配置第 %1/4 项：%2")
                          .arg(m_batchIndex + 1)
                          .arg(routerCommandName(command)));
    }
    m_replyTimer->setInterval(routerReplyTimeoutMs);
    m_replyTimer->start();
}

void MainWindow::finishRouterCommand(bool success, const QString &detail)
{
    if (m_pendingRouterCommand == RouterCommand::Count)
        return;

    m_replyTimer->stop();
    const RouterCommand completed = m_pendingRouterCommand;
    m_pendingRouterCommand = RouterCommand::Count;
    setRouterState(completed, success ? UiState::Success : UiState::Failure, detail);

    if (!m_batchRunning) {
        setActionsEnabled(true);
        return;
    }

    if (!success) {
        m_batchRunning = false;
        m_batchIndex = -1;
        setBatchState(UiState::Failure,
                      QStringLiteral("%1失败，流程已停止：%2").arg(routerCommandName(completed), detail));
        setActionsEnabled(true);
        return;
    }

    ++m_batchIndex;
    if (m_batchIndex >= routerCommandCount) {
        m_batchRunning = false;
        m_batchIndex = -1;
        setBatchState(UiState::Success,
                      QStringLiteral("4 项指令均已返回大写 OK；请重启路由器使配置生效"));
        setActionsEnabled(true);
        return;
    }

    setBatchState(UiState::Sending,
                  QStringLiteral("已完成 %1/4，准备发送下一项").arg(m_batchIndex));
    QTimer::singleShot(80, this, [this] {
        if (m_batchRunning)
            transmitRouterCommand(static_cast<RouterCommand>(m_batchIndex));
    });
}

void MainWindow::sendOperation(Operation operation, const QByteArray &payload,
                               const QString &safeDescription, bool fromProductionBatch)
{
    if (!m_serial->isOpen()) {
        setOperationState(operation, UiState::Failure, QStringLiteral("串口未连接"));
        return;
    }
    if (m_pendingOperation != Operation::Count || m_pendingRouterCommand != RouterCommand::Count
        || m_batchRunning || m_linkTestStep != LinkTestStep::Idle
        || (m_productionBatchRunning && !fromProductionBatch)) {
        setOperationState(operation, UiState::Failure,
                          QStringLiteral("已有指令正在等待设备响应"));
        return;
    }

    m_serialFrameTimer->stop();
    m_serialFrameBuffer.clear();
    m_receiveBuffer.clear();
    m_pendingOperation = operation;
    setOperationState(operation, UiState::Sending, QStringLiteral("正在发送…"));
    setActionsEnabled(false);

    const qint64 bytesAccepted = m_serial->write(payload);
    if (bytesAccepted < 0) {
        finishOperation(false, QStringLiteral("发送失败：%1").arg(m_serial->errorString()));
        return;
    }

    appendLog(QStringLiteral("发送"), safeDescription);
    setOperationState(operation, UiState::Sending, QStringLiteral("已发送，等待设备响应…"));
    m_replyTimer->setInterval(replyTimeoutMs);
    m_replyTimer->start();
}

void MainWindow::finishOperation(bool success, const QString &detail)
{
    if (m_pendingOperation == Operation::Count)
        return;

    m_replyTimer->stop();
    const Operation completed = m_pendingOperation;
    m_pendingOperation = Operation::Count;
    setOperationState(completed, success ? UiState::Success : UiState::Failure, detail);

    if (success && completed == Operation::Password
        && m_passwordInput->text().trimmed() == QStringLiteral("mei+688")) {
        QSettings settings;
        settings.setValue(QStringLiteral("production/passwordConfiguredOnce"), true);
    }

    if (m_productionBatchRunning) {
        if (!success) {
            finishProductionConfiguration(
                false, QStringLiteral("%1失败，流程已停止：%2")
                           .arg(operationName(completed), detail));
            return;
        }

        ++m_productionBatchIndex;
        constexpr int productionStepCount = 5;
        if (m_productionBatchIndex >= productionStepCount) {
            finishProductionConfiguration(true, QStringLiteral("5 项操作均已成功完成"));
            return;
        }

        m_productionBatchStatus->setText(
            QStringLiteral("●  进行中  ·  已完成 %1/5，准备下一项")
                .arg(m_productionBatchIndex));
        QTimer::singleShot(80, this, [this] {
            if (m_productionBatchRunning)
                sendNextProductionConfiguration();
        });
        return;
    }

    setActionsEnabled(true);
}

void MainWindow::startProductionConfiguration()
{
    auto rejectEmpty = [this](Operation operation, QLineEdit *input, const QString &message) {
        setOperationState(operation, UiState::Failure, message);
        finishProductionConfiguration(false, message);
        input->setFocus();
    };

    if (m_passwordInput->text().trimmed().isEmpty()) {
        rejectEmpty(Operation::Password, m_passwordInput, QStringLiteral("请先输入密码"));
        return;
    }
    if (m_initialTimeInput->text().trimmed().isEmpty()) {
        rejectEmpty(Operation::InitialTime, m_initialTimeInput,
                    QStringLiteral("请先输入初始时间指令"));
        return;
    }
    if (m_uidInput->text().trimmed().isEmpty()) {
        rejectEmpty(Operation::Uid, m_uidInput, QStringLiteral("请先输入 UID 发送内容"));
        return;
    }
    if (!m_serial->isOpen()) {
        finishProductionConfiguration(false, QStringLiteral("串口未连接"));
        return;
    }
    if (m_pendingOperation != Operation::Count || m_pendingRouterCommand != RouterCommand::Count
        || m_batchRunning || m_productionBatchRunning
        || m_linkTestStep != LinkTestStep::Idle) {
        finishProductionConfiguration(false, QStringLiteral("已有任务正在执行，请稍后再试"));
        return;
    }

    const std::array<Operation, 5> sequence = {
        Operation::Password, Operation::InitialTime, Operation::Uid,
        Operation::Time, Operation::Restart
    };
    for (const Operation operation : sequence)
        setOperationState(operation, UiState::Idle);

    m_productionBatchRunning = true;
    m_productionBatchIndex = 0;
    m_productionBatchStatus->setProperty("state", QStringLiteral("sending"));
    m_productionBatchStatus->setText(QStringLiteral("●  进行中  ·  准备第 1/5 项"));
    m_productionBatchStatus->style()->unpolish(m_productionBatchStatus);
    m_productionBatchStatus->style()->polish(m_productionBatchStatus);
    setActionsEnabled(false);
    sendNextProductionConfiguration();
}

void MainWindow::sendNextProductionConfiguration()
{
    if (!m_productionBatchRunning)
        return;
    if (!m_serial->isOpen()) {
        finishProductionConfiguration(false, QStringLiteral("串口连接已断开"));
        return;
    }

    Operation operation = Operation::Count;
    QByteArray payload;
    QString description;
    switch (m_productionBatchIndex) {
    case 0:
        operation = Operation::Password;
        payload = SerialProtocol::passwordCommand(m_passwordInput->text().trimmed());
        description = QStringLiteral("密码：******");
        break;
    case 1:
        operation = Operation::InitialTime;
        payload = SerialProtocol::initialTimeCommand(m_initialTimeInput->text());
        description = QString::fromUtf8(payload).trimmed();
        break;
    case 2:
        operation = Operation::Uid;
        payload = SerialProtocol::uidCommand(m_uidInput->text());
        description = QString::fromUtf8(payload);
        break;
    case 3:
        operation = Operation::Time;
        payload = SerialProtocol::currentTimeCommand();
        description = QString::fromUtf8(payload).trimmed();
        break;
    case 4:
        operation = Operation::Restart;
        payload = SerialProtocol::restartCommand();
        description = QStringLiteral("reset");
        break;
    default:
        finishProductionConfiguration(false, QStringLiteral("一键配置步骤异常"));
        return;
    }

    m_productionBatchStatus->setText(
        QStringLiteral("●  进行中  ·  正在执行第 %1/5 项：%2")
            .arg(m_productionBatchIndex + 1)
            .arg(operationName(operation)));
    sendOperation(operation, payload, description, true);
}

void MainWindow::finishProductionConfiguration(bool success, const QString &detail)
{
    m_productionBatchRunning = false;
    m_productionBatchIndex = -1;
    m_productionBatchStatus->setProperty(
        "state", success ? QStringLiteral("success") : QStringLiteral("failure"));
    m_productionBatchStatus->setText(
        QStringLiteral("●  %1%2")
            .arg(success ? QStringLiteral("全部成功") : QStringLiteral("配置失败"),
                 detail.isEmpty() ? QString() : QStringLiteral("  ·  ") + detail));
    m_productionBatchStatus->style()->unpolish(m_productionBatchStatus);
    m_productionBatchStatus->style()->polish(m_productionBatchStatus);
    setActionsEnabled(true);
}

void MainWindow::setOperationState(Operation operation, UiState state, const QString &detail)
{
    QLabel *label = statusLabel(operation);
    if (!label)
        return;

    QString stateName;
    QString title;
    switch (state) {
    case UiState::Idle:
        stateName = QStringLiteral("idle");
        title = QStringLiteral("●  等待操作");
        break;
    case UiState::Sending:
        stateName = QStringLiteral("sending");
        title = QStringLiteral("●  进行中");
        break;
    case UiState::Success:
        stateName = QStringLiteral("success");
        title = QStringLiteral("●  成功");
        break;
    case UiState::Failure:
        stateName = QStringLiteral("failure");
        title = QStringLiteral("●  失败");
        break;
    }

    label->setProperty("state", stateName);
    label->setText(detail.isEmpty() ? title : title + QStringLiteral("  ·  ") + detail);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

void MainWindow::setRouterState(RouterCommand command, UiState state, const QString &detail)
{
    QLabel *label = routerStatusLabel(command);
    if (!label)
        return;

    QString stateName;
    QString title;
    switch (state) {
    case UiState::Idle:
        stateName = QStringLiteral("idle");
        title = QStringLiteral("●  等待操作");
        break;
    case UiState::Sending:
        stateName = QStringLiteral("sending");
        title = QStringLiteral("●  进行中");
        break;
    case UiState::Success:
        stateName = QStringLiteral("success");
        title = QStringLiteral("●  成功");
        break;
    case UiState::Failure:
        stateName = QStringLiteral("failure");
        title = QStringLiteral("●  失败");
        break;
    }

    label->setProperty("state", stateName);
    label->setText(detail.isEmpty() ? title : title + QStringLiteral("  ·  ") + detail);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

void MainWindow::setBatchState(UiState state, const QString &detail)
{
    QString stateName;
    QString title;
    switch (state) {
    case UiState::Idle:
        stateName = QStringLiteral("idle");
        title = QStringLiteral("●  等待操作");
        break;
    case UiState::Sending:
        stateName = QStringLiteral("sending");
        title = QStringLiteral("●  进行中");
        break;
    case UiState::Success:
        stateName = QStringLiteral("success");
        title = QStringLiteral("●  全部成功");
        break;
    case UiState::Failure:
        stateName = QStringLiteral("failure");
        title = QStringLiteral("●  配置失败");
        break;
    }

    m_batchStatus->setProperty("state", stateName);
    m_batchStatus->setText(detail.isEmpty() ? title : title + QStringLiteral("  ·  ") + detail);
    m_batchStatus->style()->unpolish(m_batchStatus);
    m_batchStatus->style()->polish(m_batchStatus);
}

void MainWindow::setActionsEnabled(bool enabled)
{
    for (QPushButton *button : m_actionButtons)
        button->setEnabled(enabled);
    m_passwordInput->setEnabled(enabled);
    m_initialTimeInput->setEnabled(enabled);
    m_uidInput->setEnabled(enabled);
    for (QPushButton *button : m_routerButtons)
        button->setEnabled(enabled);
    for (QLineEdit *input : m_routerParameterInputs)
        input->setEnabled(enabled);
    m_batchButton->setEnabled(enabled);
    m_productionBatchButton->setEnabled(enabled);
    m_serialMessageButton->setEnabled(enabled && m_serial->isOpen());
}

void MainWindow::toggleMqttConnection()
{
    if (m_mqtt->state() != MqttClient::State::Disconnected) {
        m_lastMqttError.clear();
        m_mqtt->disconnectFromBroker();
        return;
    }

    const QString host = m_mqttHostInput->text().trimmed();
    const QString clientId = m_mqttClientIdInput->text().trimmed();
    if (host.isEmpty() || clientId.isEmpty()) {
        m_mqttStatus->setProperty("state", QStringLiteral("failure"));
        m_mqttStatus->setText(QStringLiteral("● Broker 和 Client ID 不能为空"));
        m_mqttStatus->style()->unpolish(m_mqttStatus);
        m_mqttStatus->style()->polish(m_mqttStatus);
        return;
    }

    saveMqttSettings();
    m_lastMqttError.clear();
    m_mqttHostInput->setEnabled(false);
    m_mqttPortInput->setEnabled(false);
    m_mqttClientIdInput->setEnabled(false);
    m_mqttUsernameInput->setEnabled(false);
    m_mqttPasswordInput->setEnabled(false);
    m_mqttConnectButton->setText(QStringLiteral("取消连接"));
    m_mqttStatus->setProperty("state", QStringLiteral("sending"));
    m_mqttStatus->setText(QStringLiteral("● 正在连接 %1:%2…").arg(host).arg(m_mqttPortInput->value()));
    m_mqttStatus->style()->unpolish(m_mqttStatus);
    m_mqttStatus->style()->polish(m_mqttStatus);

    m_mqtt->connectToBroker(host, static_cast<quint16>(m_mqttPortInput->value()), clientId,
                            m_mqttUsernameInput->text(), m_mqttPasswordInput->text());
}

void MainWindow::subscribeMqttTopic()
{
    const QString topic = m_mqttSubscribeTopicInput->text().trimmed();
    if (topic.isEmpty()) {
        m_mqttStatus->setProperty("state", QStringLiteral("failure"));
        m_mqttStatus->setText(QStringLiteral("● 订阅主题不能为空"));
        m_mqttStatus->style()->unpolish(m_mqttStatus);
        m_mqttStatus->style()->polish(m_mqttStatus);
        return;
    }

    if (!m_mqtt->subscribe(topic)) {
        m_mqttStatus->setProperty("state", QStringLiteral("failure"));
        m_mqttStatus->setText(QStringLiteral("● MQTT 未连接，无法订阅"));
        m_mqttStatus->style()->unpolish(m_mqttStatus);
        m_mqttStatus->style()->polish(m_mqttStatus);
        return;
    }

    saveMqttSettings();
    m_mqttStatus->setProperty("state", QStringLiteral("sending"));
    m_mqttStatus->setText(QStringLiteral("● 正在订阅：%1").arg(topic));
    m_mqttStatus->style()->unpolish(m_mqttStatus);
    m_mqttStatus->style()->polish(m_mqttStatus);
}

void MainWindow::publishMqttMessage()
{
    const QString topic = m_mqttPublishTopicInput->text().trimmed();
    if (topic.isEmpty()) {
        appendMessageRecord(QStringLiteral("MQTT 错误"), QString(), QByteArrayLiteral("发布主题不能为空"));
        m_mqttPublishTopicInput->setFocus();
        return;
    }

    const QByteArray payload = m_mqttPayloadInput->toPlainText().toUtf8();
    if (!m_mqtt->publish(topic, payload)) {
        appendMessageRecord(QStringLiteral("MQTT 错误"), topic, QByteArrayLiteral("MQTT 未连接，消息未发送"));
        return;
    }

    saveMqttSettings();
    appendMessageRecord(QStringLiteral("MQTT 发送"), topic, payload);
}

void MainWindow::sendSerialMessage()
{
    if (!m_serial->isOpen()) {
        appendMessageRecord(QStringLiteral("串口错误"), QString(), QByteArrayLiteral("串口未连接，消息未发送"));
        return;
    }
    if (m_pendingOperation != Operation::Count || m_pendingRouterCommand != RouterCommand::Count
        || m_batchRunning) {
        appendMessageRecord(QStringLiteral("串口错误"), QString(), QByteArrayLiteral("配置指令正在执行，请稍后再发送串口消息"));
        return;
    }

    QByteArray payload = m_mqttPayloadInput->toPlainText().toUtf8();
    const QString ending = m_serialLineEndingCombo->currentData().toString();
    if (ending == QStringLiteral("crlf"))
        payload.append("\r\n");
    else if (ending == QStringLiteral("lf"))
        payload.append('\n');

    if (m_serial->write(payload) < 0) {
        appendMessageRecord(QStringLiteral("串口错误"), QString(),
                            QStringLiteral("串口发送失败：%1").arg(m_serial->errorString()).toUtf8());
        return;
    }

    saveMqttSettings();
    appendMessageRecord(QStringLiteral("串口发送"), QString(), payload);
}

void MainWindow::setMqttUiConnected(bool connected, const QString &detail)
{
    m_mqttHostInput->setEnabled(!connected);
    m_mqttPortInput->setEnabled(!connected);
    m_mqttClientIdInput->setEnabled(!connected);
    m_mqttUsernameInput->setEnabled(!connected);
    m_mqttPasswordInput->setEnabled(!connected);
    m_mqttConnectButton->setText(connected ? QStringLiteral("断开 MQTT") : QStringLiteral("连接 MQTT"));
    m_mqttSubscribeButton->setEnabled(connected);
    m_mqttPublishButton->setEnabled(connected);
    const bool failed = !connected && detail.startsWith(QStringLiteral("连接失败"));
    m_mqttStatus->setProperty("state", connected ? QStringLiteral("success")
                                                  : (failed ? QStringLiteral("failure")
                                                            : QStringLiteral("idle")));
    m_mqttStatus->setText(detail.isEmpty()
                              ? (connected ? QStringLiteral("● 已连接") : QStringLiteral("● 未连接"))
                              : QStringLiteral("● %1").arg(detail));
    m_mqttStatus->style()->unpolish(m_mqttStatus);
    m_mqttStatus->style()->polish(m_mqttStatus);
}

void MainWindow::appendMessageRecord(const QString &source, const QString &topic, const QByteArray &payload)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    QString path;
    if (source == QStringLiteral("MQTT 发送")) {
        path = QStringLiteral("【发出】【MQTT】电脑 → Broker → MQTT订阅端（4G）");
    } else if (source == QStringLiteral("MQTT 接收")) {
        path = QStringLiteral("【收到】【MQTT】MQTT发布端（4G）→ Broker → 电脑");
    } else if (source == QStringLiteral("串口发送")) {
        path = QStringLiteral("【发出】【串口】电脑 → 串口 → 4G路由器");
    } else if (source == QStringLiteral("串口接收")) {
        path = QStringLiteral("【收到】【串口】4G路由器 → 串口 → 电脑");
    } else if (source == QStringLiteral("MQTT 错误")) {
        path = QStringLiteral("【错误】【MQTT】Broker/MQTT 通道");
    } else if (source == QStringLiteral("串口错误")) {
        path = QStringLiteral("【错误】【串口】电脑/串口/4G路由器通道");
    } else if (source == QStringLiteral("MQTT 状态")) {
        path = QStringLiteral("【状态】【MQTT】电脑 ↔ Broker");
    } else if (source == QStringLiteral("测试状态")) {
        path = QStringLiteral("【测试】【双向链路】串口 ↔ 4G路由器 ↔ Broker ↔ MQTT");
    } else {
        path = QStringLiteral("【状态】%1").arg(source);
    }

    const QString topicPart = topic.isEmpty() ? QString() : QStringLiteral(" | 主题：%1").arg(topic);
    QString message = QString::fromUtf8(payload);
    if (message.isEmpty() && !payload.isEmpty())
        message = QStringLiteral("0x%1").arg(QString::fromLatin1(payload.toHex(' ')));
    if (message.isEmpty())
        message = QStringLiteral("（空消息）");
    message.replace('\r', QStringLiteral("\\r"));
    message.replace('\n', QStringLiteral("\\n"));
    m_messageLog->appendPlainText(QStringLiteral("[%1] %2%3 | 内容：%4")
                                      .arg(timestamp, path, topicPart, message));
}

QString MainWindow::configurationFilePath() const
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("ProductionAssistant.ini"));
}

void MainWindow::loadUserConfiguration()
{
    QSettings settings(configurationFilePath(), QSettings::IniFormat);

    m_initialTimeInput->setText(
        settings.value(QStringLiteral("production/initialTime"),
                       m_initialTimeInput->text()).toString());

    for (int i = 0; i < routerCommandCount; ++i) {
        const RouterCommand command = static_cast<RouterCommand>(i);
        const QString key = QStringLiteral("router/%1Parameters").arg(routerCommandName(command));
        m_routerParameterInputs[i]->setText(
            settings.value(key, m_routerParameterInputs[i]->text()).toString());
    }

    m_mqttSubscribeTopicInput->setText(
        settings.value(QStringLiteral("mqtt/subscribeTopic"),
                       m_mqttSubscribeTopicInput->text()).toString());
    m_mqttPublishTopicInput->setText(
        settings.value(QStringLiteral("mqtt/publishTopic"),
                       m_mqttPublishTopicInput->text()).toString());
    m_mqttHostInput->setText(
        settings.value(QStringLiteral("mqtt/broker"),
                       m_mqttHostInput->text()).toString());
    m_mqttPortInput->setValue(
        settings.value(QStringLiteral("mqtt/port"),
                       m_mqttPortInput->value()).toInt());
    m_mqttClientIdInput->setText(
        settings.value(QStringLiteral("mqtt/clientId"),
                       m_mqttClientIdInput->text()).toString());
}

void MainWindow::saveUserConfiguration(bool showFeedback)
{
    QSettings settings(configurationFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("production/initialTime"), m_initialTimeInput->text());
    for (int i = 0; i < routerCommandCount; ++i) {
        const RouterCommand command = static_cast<RouterCommand>(i);
        const QString key = QStringLiteral("router/%1Parameters").arg(routerCommandName(command));
        settings.setValue(key, m_routerParameterInputs[i]->text());
    }
    settings.setValue(QStringLiteral("mqtt/subscribeTopic"),
                      m_mqttSubscribeTopicInput->text());
    settings.setValue(QStringLiteral("mqtt/publishTopic"),
                      m_mqttPublishTopicInput->text());
    settings.setValue(QStringLiteral("mqtt/broker"), m_mqttHostInput->text());
    settings.setValue(QStringLiteral("mqtt/port"), m_mqttPortInput->value());
    settings.setValue(QStringLiteral("mqtt/clientId"), m_mqttClientIdInput->text());
    settings.sync();

    if (!showFeedback)
        return;

    if (settings.status() == QSettings::NoError) {
        m_saveConfigurationButton->setText(QStringLiteral("配置已保存"));
        QTimer::singleShot(1600, this, [this] {
            m_saveConfigurationButton->setText(QStringLiteral("保存配置"));
        });
        return;
    }

    m_saveConfigurationButton->setText(QStringLiteral("保存失败"));
    QMessageBox::warning(
        this, QStringLiteral("保存配置失败"),
        QStringLiteral("无法写入配置文件：\n%1").arg(configurationFilePath()));
}

void MainWindow::loadMqttSettings()
{
    QSettings settings;
    if (settings.value(QStringLiteral("production/passwordConfiguredOnce"), false).toBool())
        m_passwordInput->setText(QStringLiteral("mei+688"));
    else
        m_passwordInput->clear();

    const QString generatedClientId = QStringLiteral("ProductionAssistant-%1")
                                          .arg(QUuid::createUuid().toString(QUuid::Id128).left(8));
    m_mqttHostInput->setText(settings.value(QStringLiteral("mqtt/host"),
                                             QStringLiteral("broker.emqx.io")).toString());
    m_mqttPortInput->setValue(settings.value(QStringLiteral("mqtt/port"), 1883).toInt());
    m_mqttClientIdInput->setText(settings.value(QStringLiteral("mqtt/clientId"),
                                                 generatedClientId).toString());
    m_mqttUsernameInput->setText(settings.value(QStringLiteral("mqtt/username")).toString());
    QString subscribeTopic = settings.value(QStringLiteral("mqtt/subscribeTopic"),
                                             QStringLiteral("sendev/10/temperature")).toString();
    QString publishTopic = settings.value(QStringLiteral("mqtt/publishTopic"),
                                           QStringLiteral("sendev/1/temperature")).toString();

    // 旧版本误将 MQTOP0 的“路由器订阅,路由器发布”方向反用；仅迁移已知旧默认值，
    // 不改动操作员自行填写的其他主题。
    const int topicDirectionVersion = settings.value(QStringLiteral("mqtt/topicDirectionVersion"), 1).toInt();
    if (topicDirectionVersion < 2
        && subscribeTopic == QStringLiteral("sendev/1/temperature")
        && publishTopic == QStringLiteral("sendev/10/temperature")) {
        subscribeTopic = QStringLiteral("sendev/10/temperature");
        publishTopic = QStringLiteral("sendev/1/temperature");
    }
    settings.setValue(QStringLiteral("mqtt/topicDirectionVersion"), 2);
    m_mqttSubscribeTopicInput->setText(subscribeTopic);
    m_mqttPublishTopicInput->setText(publishTopic);
    const QString ending = settings.value(QStringLiteral("mqtt/serialLineEnding"),
                                           QStringLiteral("none")).toString();
    const int endingIndex = m_serialLineEndingCombo->findData(ending);
    if (endingIndex >= 0)
        m_serialLineEndingCombo->setCurrentIndex(endingIndex);
    m_linkTestDurationInput->setValue(
        settings.value(QStringLiteral("mqtt/linkTestDuration"), 10).toInt());
}

void MainWindow::saveMqttSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("mqtt/host"), m_mqttHostInput->text().trimmed());
    settings.setValue(QStringLiteral("mqtt/port"), m_mqttPortInput->value());
    settings.setValue(QStringLiteral("mqtt/clientId"), m_mqttClientIdInput->text().trimmed());
    settings.setValue(QStringLiteral("mqtt/username"), m_mqttUsernameInput->text());
    settings.setValue(QStringLiteral("mqtt/subscribeTopic"), m_mqttSubscribeTopicInput->text().trimmed());
    settings.setValue(QStringLiteral("mqtt/publishTopic"), m_mqttPublishTopicInput->text().trimmed());
    settings.setValue(QStringLiteral("mqtt/topicDirectionVersion"), 2);
    settings.setValue(QStringLiteral("mqtt/serialLineEnding"), m_serialLineEndingCombo->currentData());
    settings.setValue(QStringLiteral("mqtt/linkTestDuration"), m_linkTestDurationInput->value());
}

void MainWindow::startOrStopLinkTest()
{
    if (m_linkTestStep != LinkTestStep::Idle) {
        finishLinkTest(false, QStringLiteral("测试已由操作员停止"));
        return;
    }

    auto showCannotStart = [this](const QString &message) {
        m_linkTestStatus->setProperty("state", QStringLiteral("failure"));
        m_linkTestStatus->setText(QStringLiteral("●  无法测试  ·  %1").arg(message));
        m_linkTestStatus->style()->unpolish(m_linkTestStatus);
        m_linkTestStatus->style()->polish(m_linkTestStatus);
        appendMessageRecord(QStringLiteral("测试状态"), QString(), message.toUtf8());
    };

    if (!m_serial->isOpen()) {
        showCannotStart(QStringLiteral("请先连接串口"));
        return;
    }
    if (m_mqtt->state() != MqttClient::State::Connected) {
        showCannotStart(QStringLiteral("请先连接 MQTT Broker"));
        return;
    }
    if (m_mqttSubscribeTopicInput->text().trimmed().isEmpty()
        || m_mqttPublishTopicInput->text().trimmed().isEmpty()) {
        showCannotStart(QStringLiteral("发布主题和订阅主题不能为空"));
        return;
    }
    if (m_pendingOperation != Operation::Count || m_pendingRouterCommand != RouterCommand::Count
        || m_batchRunning || m_productionBatchRunning) {
        showCannotStart(QStringLiteral("配置指令正在执行，请稍后再测试"));
        return;
    }

    saveMqttSettings();
    m_serialFrameTimer->stop();
    m_serialFrameBuffer.clear();
    m_linkTestSessionId = QUuid::createUuid().toString(QUuid::Id128).left(8);
    m_linkTestExpectedPayload.clear();
    m_linkTestRound = 1;
    m_linkTestSerialToMqttSuccesses = 0;
    m_linkTestMqttToSerialSuccesses = 0;
    m_linkTestStartedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_linkTestStep = LinkTestStep::PreparingSubscription;
    setLinkTestControls(true);

    const int durationSeconds = m_linkTestDurationInput->value();
    m_linkTestDeadlineTimer->start(durationSeconds * 1000);
    m_linkTestStepTimer->start(3000);
    m_linkTestStatus->setProperty("state", QStringLiteral("sending"));
    m_linkTestStatus->setText(QStringLiteral("●  准备中  ·  正在确认 MQTT 订阅"));
    m_linkTestStatus->style()->unpolish(m_linkTestStatus);
    m_linkTestStatus->style()->polish(m_linkTestStatus);
    appendMessageRecord(QStringLiteral("测试状态"), QString(),
                        QStringLiteral("开始 %1 秒双向闭环测试，会轮流执行串口→MQTT和MQTT→串口")
                            .arg(durationSeconds).toUtf8());

    if (!m_mqtt->subscribe(m_mqttSubscribeTopicInput->text().trimmed()))
        finishLinkTest(false, QStringLiteral("无法订阅 MQTT 测试主题"));
}

void MainWindow::sendNextLinkTestMessage()
{
    if (m_linkTestStep == LinkTestStep::SerialToMqtt) {
        m_linkTestExpectedPayload = QStringLiteral("PA_TEST_S2M_%1_%2")
                                        .arg(m_linkTestSessionId)
                                        .arg(m_linkTestRound).toUtf8();
        QByteArray serialPayload = m_linkTestExpectedPayload;
        const QString ending = m_serialLineEndingCombo->currentData().toString();
        if (ending == QStringLiteral("crlf"))
            serialPayload.append("\r\n");
        else if (ending == QStringLiteral("lf"))
            serialPayload.append('\n');

        if (m_serial->write(serialPayload) < 0) {
            finishLinkTest(false,
                           QStringLiteral("上行失败：串口写入错误：%1").arg(m_serial->errorString()));
            return;
        }

        appendMessageRecord(QStringLiteral("串口发送"), QString(), serialPayload);
        m_linkTestStatus->setText(
            QStringLiteral("●  测试中  ·  第 %1 轮：等待串口消息到达 MQTT").arg(m_linkTestRound));
        m_linkTestStepTimer->start(3000);
        return;
    }

    if (m_linkTestStep == LinkTestStep::MqttToSerial) {
        m_linkTestExpectedPayload = QStringLiteral("PA_TEST_M2S_%1_%2")
                                        .arg(m_linkTestSessionId)
                                        .arg(m_linkTestRound).toUtf8();
        const QString topic = m_mqttPublishTopicInput->text().trimmed();
        if (!m_mqtt->publish(topic, m_linkTestExpectedPayload)) {
            finishLinkTest(false, QStringLiteral("下行失败：MQTT 发布测试消息失败"));
            return;
        }

        appendMessageRecord(QStringLiteral("MQTT 发送"), topic, m_linkTestExpectedPayload);
        m_linkTestStatus->setText(
            QStringLiteral("●  测试中  ·  第 %1 轮：等待 MQTT 消息到达串口").arg(m_linkTestRound));
        m_linkTestStepTimer->start(3000);
    }
}

void MainWindow::handleLinkTestMqttMessage(const QString &topic, const QByteArray &payload)
{
    if (m_linkTestStep != LinkTestStep::SerialToMqtt
        || topic != m_mqttSubscribeTopicInput->text().trimmed()
        || !payload.contains(m_linkTestExpectedPayload)) {
        return;
    }

    m_linkTestStepTimer->stop();
    ++m_linkTestSerialToMqttSuccesses;
    appendMessageRecord(QStringLiteral("测试状态"), topic,
                        QStringLiteral("第 %1 轮上行通过：串口 → MQTT").arg(m_linkTestRound).toUtf8());
    m_linkTestStep = LinkTestStep::MqttToSerial;
    QTimer::singleShot(100, this, [this] {
        if (m_linkTestStep == LinkTestStep::MqttToSerial)
            sendNextLinkTestMessage();
    });
}

void MainWindow::handleLinkTestSerialFrame(const QByteArray &frame)
{
    if (m_linkTestStep != LinkTestStep::MqttToSerial
        || !frame.contains(m_linkTestExpectedPayload)) {
        return;
    }

    m_linkTestStepTimer->stop();
    ++m_linkTestMqttToSerialSuccesses;
    appendMessageRecord(QStringLiteral("测试状态"), QString(),
                        QStringLiteral("第 %1 轮下行通过：MQTT → 串口").arg(m_linkTestRound).toUtf8());
    ++m_linkTestRound;
    m_linkTestStep = LinkTestStep::SerialToMqtt;
    QTimer::singleShot(100, this, [this] {
        if (m_linkTestStep == LinkTestStep::SerialToMqtt)
            sendNextLinkTestMessage();
    });
}

void MainWindow::finishLinkTest(bool success, const QString &detail)
{
    if (m_linkTestStep == LinkTestStep::Idle)
        return;

    const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - m_linkTestStartedAtMs;
    m_linkTestDeadlineTimer->stop();
    m_linkTestStepTimer->stop();
    m_linkTestStep = LinkTestStep::Idle;
    m_linkTestExpectedPayload.clear();
    setLinkTestControls(false);

    const QString summary = QStringLiteral("%1；上行 %2 次，下行 %3 次，用时 %4 s")
                                .arg(detail)
                                .arg(m_linkTestSerialToMqttSuccesses)
                                .arg(m_linkTestMqttToSerialSuccesses)
                                .arg(elapsedMs / 1000.0, 0, 'f', 1);
    m_linkTestStatus->setProperty("state", success ? QStringLiteral("success")
                                                   : QStringLiteral("failure"));
    m_linkTestStatus->setText(QStringLiteral("●  %1  ·  %2")
                                  .arg(success ? QStringLiteral("双向正常")
                                               : QStringLiteral("测试失败"),
                                       summary));
    m_linkTestStatus->style()->unpolish(m_linkTestStatus);
    m_linkTestStatus->style()->polish(m_linkTestStatus);
    appendMessageRecord(QStringLiteral("测试状态"), QString(),
                        QStringLiteral("%1：%2")
                            .arg(success ? QStringLiteral("测试通过") : QStringLiteral("测试失败"), summary)
                            .toUtf8());
}

void MainWindow::setLinkTestControls(bool running)
{
    setActionsEnabled(!running);
    m_connectButton->setEnabled(!running
                                && (m_serial->isOpen()
                                    || !currentPortName().isEmpty()));
    m_mqttConnectButton->setEnabled(!running);
    m_mqttSubscribeButton->setEnabled(!running && m_mqtt->state() == MqttClient::State::Connected);
    m_mqttPublishButton->setEnabled(!running && m_mqtt->state() == MqttClient::State::Connected);
    m_mqttSubscribeTopicInput->setEnabled(!running);
    m_mqttPublishTopicInput->setEnabled(!running);
    m_mqttPayloadInput->setEnabled(!running);
    m_serialLineEndingCombo->setEnabled(!running);
    m_linkTestDurationInput->setEnabled(!running);
    m_linkTestButton->setText(running ? QStringLiteral("停止连接测试")
                                      : QStringLiteral("一键连接测试"));
}

void MainWindow::readSerialData()
{
    const QByteArray incoming = m_serial->readAll();
    const bool configurationReplyExpected = m_pendingOperation != Operation::Count
                                            || m_pendingRouterCommand != RouterCommand::Count
                                            || m_batchRunning;
    if (!configurationReplyExpected) {
        m_serialFrameBuffer.append(incoming);
        // 串口是字节流：连续到达的字节先合并，静默 50ms 后再视为一帧。
        m_serialFrameTimer->start();
        return;
    }
    m_receiveBuffer.append(incoming);

    qsizetype lineEnd = -1;
    while ((lineEnd = m_receiveBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_receiveBuffer.left(lineEnd).trimmed();
        m_receiveBuffer.remove(0, lineEnd + 1);
        if (!line.isEmpty())
            processReply(QString::fromUtf8(line));
    }

    // 同时兼容只返回 OK/ERR 且不带换行符的设备。
    const QString possibleReply = QString::fromUtf8(m_receiveBuffer).trimmed();
    const bool uidReturnedOk = m_pendingOperation == Operation::Uid
                               && possibleReply.contains(QStringLiteral("ok"), Qt::CaseInsensitive);
    if (uidReturnedOk
        || SerialProtocol::classifyReply(possibleReply) != SerialProtocol::ReplyResult::Unknown) {
        m_receiveBuffer.clear();
        processReply(possibleReply);
    } else if (m_pendingOperation == Operation::UidCheck && !possibleReply.isEmpty()) {
        // UID 查询结果不一定包含 OK，也可能没有换行；静默 50ms 后按完整结果处理。
        m_serialFrameTimer->start();
    }
}

void MainWindow::processReply(const QString &reply)
{
    QString safeReply = reply;
    if (m_pendingOperation == Operation::Password && !m_passwordInput->text().trimmed().isEmpty())
        safeReply.replace(m_passwordInput->text().trimmed(), QStringLiteral("******"),
                          Qt::CaseInsensitive);

    appendLog(QStringLiteral("接收"), safeReply);
    if (m_pendingRouterCommand != RouterCommand::Count) {
        if (reply.trimmed() == QStringLiteral("OK")) {
            finishRouterCommand(true, QStringLiteral("收到大写 OK"));
        } else if (SerialProtocol::classifyReply(reply) == SerialProtocol::ReplyResult::Failure) {
            finishRouterCommand(false, safeReply);
        }
        return;
    }

    if (m_pendingOperation == Operation::Count)
        return;

    const SerialProtocol::ReplyResult result = SerialProtocol::classifyReply(reply);
    if (m_pendingOperation == Operation::UidCheck) {
        const QString value = reply.trimmed();
        if (value.compare(QStringLiteral("set_board_UID"), Qt::CaseInsensitive) == 0)
            return;
        if (result == SerialProtocol::ReplyResult::Failure)
            finishOperation(false, safeReply);
        else if (!value.isEmpty())
            finishOperation(true, QStringLiteral("设备返回：%1").arg(safeReply));
        return;
    }

    const bool uidReturnedOk = m_pendingOperation == Operation::Uid
                               && reply.contains(QStringLiteral("ok"), Qt::CaseInsensitive);
    if (uidReturnedOk || result == SerialProtocol::ReplyResult::Success)
        finishOperation(true, safeReply);
    else if (result == SerialProtocol::ReplyResult::Failure)
        finishOperation(false, safeReply);
}

void MainWindow::handleSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError)
        return;

    if (error == QSerialPort::ResourceError || error == QSerialPort::DeviceNotFoundError
        || error == QSerialPort::PermissionError) {
        const QString errorText = m_serial->errorString();
        if (m_linkTestStep != LinkTestStep::Idle)
            finishLinkTest(false, QStringLiteral("串口异常：%1").arg(errorText));
        if (m_pendingOperation != Operation::Count)
            finishOperation(false, QStringLiteral("串口异常：%1").arg(errorText));
        else if (m_productionBatchRunning)
            finishProductionConfiguration(false, QStringLiteral("串口异常：%1").arg(errorText));
        if (m_pendingRouterCommand != RouterCommand::Count)
            finishRouterCommand(false, QStringLiteral("串口异常：%1").arg(errorText));
        else if (m_batchRunning) {
            m_batchRunning = false;
            m_batchIndex = -1;
            setBatchState(UiState::Failure, QStringLiteral("串口异常：%1").arg(errorText));
            setActionsEnabled(true);
        }
        if (m_serial->isOpen())
            m_serial->close();
        setConnectionUi(false, errorText);
        appendLog(QStringLiteral("错误"), errorText);
    }
}

void MainWindow::appendLog(const QString &direction, const QString &message)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    QString label;
    if (direction == QStringLiteral("发送"))
        label = QStringLiteral("【发出】【串口】电脑 → 下位机");
    else if (direction == QStringLiteral("接收"))
        label = QStringLiteral("【收到】【串口】下位机 → 电脑");
    else if (direction == QStringLiteral("错误"))
        label = QStringLiteral("【错误】【串口】");
    else
        label = QStringLiteral("【系统】【串口】");

    QString oneLineMessage = message;
    oneLineMessage.replace('\r', QStringLiteral("\\r"));
    oneLineMessage.replace('\n', QStringLiteral("\\n"));
    m_log->appendPlainText(QStringLiteral("[%1] %2 | %3").arg(timestamp, label, oneLineMessage));
}

QString MainWindow::operationName(Operation operation) const
{
    switch (operation) {
    case Operation::Password: return QStringLiteral("输入密码");
    case Operation::InitialTime: return QStringLiteral("配置初始时间");
    case Operation::UidCheck: return QStringLiteral("读取 UID 设置状态");
    case Operation::Uid: return QStringLiteral("修改 UID");
    case Operation::Time: return QStringLiteral("配置时间");
    case Operation::Restart: return QStringLiteral("重启");
    case Operation::Count: break;
    }
    return QStringLiteral("其他操作");
}

QLabel *MainWindow::statusLabel(Operation operation) const
{
    const int index = static_cast<int>(operation);
    if (index < 0 || index >= operationCount)
        return nullptr;
    return m_statusLabels[index];
}

QString MainWindow::routerCommandPrefix(RouterCommand command) const
{
    switch (command) {
    case RouterCommand::Gps:
        return QStringLiteral("AT*GPSCFG=");
    case RouterCommand::Server:
        return QStringLiteral("AT*SERVER0=");
    case RouterCommand::MqSet:
        return QStringLiteral("AT*MQSET0=");
    case RouterCommand::MqTopic:
        return QStringLiteral("AT*MQTOP0=");
    case RouterCommand::Count:
        break;
    }
    return {};
}

QString MainWindow::routerDefaultParameters(RouterCommand command) const
{
    switch (command) {
    case RouterCommand::Gps:
        return QStringLiteral("4,5,0");
    case RouterCommand::Server:
        return QStringLiteral("9,broker.emqx.io,1883");
    case RouterCommand::MqSet:
        return QStringLiteral("00000001,,");
    case RouterCommand::MqTopic:
        return QStringLiteral("sendev/1/temperature,sendev/10/temperature");
    case RouterCommand::Count:
        break;
    }
    return {};
}

QString MainWindow::routerCommandText(RouterCommand command) const
{
    const int index = static_cast<int>(command);
    const QString parameters = index >= 0 && index < routerCommandCount
                                   && m_routerParameterInputs[index]
                                   ? m_routerParameterInputs[index]->text().trimmed()
                                   : routerDefaultParameters(command);
    return routerCommandPrefix(command) + parameters;
}

QString MainWindow::routerCommandName(RouterCommand command) const
{
    switch (command) {
    case RouterCommand::Gps: return QStringLiteral("GPSCFG");
    case RouterCommand::Server: return QStringLiteral("SERVER0");
    case RouterCommand::MqSet: return QStringLiteral("MQSET0");
    case RouterCommand::MqTopic: return QStringLiteral("MQTOP0");
    case RouterCommand::Count: break;
    }
    return QStringLiteral("未知指令");
}

QLabel *MainWindow::routerStatusLabel(RouterCommand command) const
{
    const int index = static_cast<int>(command);
    if (index < 0 || index >= routerCommandCount)
        return nullptr;
    return m_routerStatusLabels[index];
}
