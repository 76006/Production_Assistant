#include "mainwindow.h"

#include "serialprotocol.h"

#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QTabWidget>
#include <QTimer>
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
{
    buildUi();
    applyStyle();

    m_replyTimer->setSingleShot(true);
    m_replyTimer->setInterval(replyTimeoutMs);

    connect(m_refreshButton, &QPushButton::clicked, this, &MainWindow::refreshPorts);
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::toggleConnection);
    connect(m_actionButtons[operationIndex(static_cast<int>(Operation::Password))], &QPushButton::clicked,
            this, &MainWindow::sendPassword);
    connect(m_actionButtons[operationIndex(static_cast<int>(Operation::InitialTime))], &QPushButton::clicked,
            this, &MainWindow::sendInitialTime);
    connect(m_actionButtons[operationIndex(static_cast<int>(Operation::Uid))], &QPushButton::clicked,
            this, &MainWindow::sendUidCommand);
    connect(m_actionButtons[operationIndex(static_cast<int>(Operation::Time))], &QPushButton::clicked,
            this, &MainWindow::sendCurrentTime);
    connect(m_actionButtons[operationIndex(static_cast<int>(Operation::Restart))], &QPushButton::clicked,
            this, &MainWindow::sendRestart);
    for (int i = 0; i < routerCommandCount; ++i) {
        const RouterCommand command = static_cast<RouterCommand>(i);
        connect(m_routerButtons[i], &QPushButton::clicked, this,
                [this, command] { sendRouterCommand(command); });
    }
    connect(m_batchButton, &QPushButton::clicked, this, &MainWindow::startOneClickConfiguration);
    connect(m_serial, &QSerialPort::readyRead, this, &MainWindow::readSerialData);
    connect(m_serial, &QSerialPort::errorOccurred, this, &MainWindow::handleSerialError);
    connect(m_replyTimer, &QTimer::timeout, this, [this] {
        if (m_pendingRouterCommand != RouterCommand::Count) {
            finishRouterCommand(false, QStringLiteral("未收到大写 OK（5 秒）"));
            return;
        }
        const QString detail = m_pendingOperation == Operation::Uid
                                   ? QStringLiteral("设备无返回（3 秒）")
                                   : QStringLiteral("等待设备响应超时（3 秒）");
        finishOperation(false, detail);
    });

    refreshPorts();
    setConnectionUi(false);
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("生产助手"));
    setMinimumSize(900, 720);
    resize(980, 790);

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
    m_portCombo->setMinimumWidth(220);
    auto *baudLabel = new QLabel(QStringLiteral("波特率"), connectionCard);
    m_baudCombo = new QComboBox(connectionCard);
    m_baudCombo->addItems({QStringLiteral("9600"), QStringLiteral("19200"), QStringLiteral("38400"),
                           QStringLiteral("57600"), QStringLiteral("115200")});
    m_baudCombo->setCurrentText(QStringLiteral("115200"));
    m_refreshButton = new QPushButton(QStringLiteral("刷新"), connectionCard);
    m_refreshButton->setObjectName(QStringLiteral("secondaryButton"));
    m_connectButton = new QPushButton(QStringLiteral("连接设备"), connectionCard);
    m_connectButton->setObjectName(QStringLiteral("primaryButton"));
    m_connectionStatus = new QLabel(connectionCard);
    m_connectionStatus->setObjectName(QStringLiteral("connectionStatus"));

    connectionLayout->addWidget(portLabel);
    connectionLayout->addWidget(m_portCombo, 1);
    connectionLayout->addWidget(baudLabel);
    connectionLayout->addWidget(m_baudCombo);
    connectionLayout->addWidget(m_refreshButton);
    connectionLayout->addWidget(m_connectButton);
    connectionLayout->addSpacing(6);
    connectionLayout->addWidget(m_connectionStatus);
    pageLayout->addWidget(connectionCard);

    auto *tabs = new QTabWidget(central);
    tabs->setObjectName(QStringLiteral("appTabs"));
    tabs->setDocumentMode(true);

    auto *productionPage = new QWidget(tabs);
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
        QStringLiteral("输入密码"),
        QStringLiteral("配置初始时间"),
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

        if (i == static_cast<int>(Operation::Uid)) {
            auto *uidEditor = new QWidget(operationsCard);
            auto *uidLayout = new QHBoxLayout(uidEditor);
            uidLayout->setContentsMargins(0, 0, 0, 0);
            uidLayout->setSpacing(10);
            button->setMinimumWidth(112);
            m_uidInput = new QLineEdit(uidEditor);
            m_uidInput->setObjectName(QStringLiteral("uidInput"));
            m_uidInput->setMinimumHeight(50);
            m_uidInput->setPlaceholderText(QStringLiteral("输入要原样发送的内容"));
            m_uidInput->setClearButtonEnabled(true);
            connect(m_uidInput, &QLineEdit::returnPressed, button, &QPushButton::click);
            uidLayout->addWidget(button);
            uidLayout->addWidget(m_uidInput, 1);
            grid->addWidget(uidEditor, i + 1, 0);
        } else {
            grid->addWidget(button, i + 1, 0);
        }
        grid->addWidget(status, i + 1, 1);
    }
    productionLayout->addWidget(operationsCard);
    productionLayout->addStretch();

    auto *routerPage = new QWidget(tabs);
    auto *routerPageLayout = new QVBoxLayout(routerPage);
    routerPageLayout->setContentsMargins(4, 14, 4, 4);

    auto *routerCard = new QFrame(routerPage);
    routerCard->setObjectName(QStringLiteral("card"));
    auto *routerGrid = new QGridLayout(routerCard);
    routerGrid->setContentsMargins(20, 18, 20, 20);
    routerGrid->setHorizontalSpacing(18);
    routerGrid->setVerticalSpacing(12);
    routerGrid->setColumnStretch(0, 5);
    routerGrid->setColumnStretch(1, 3);

    auto *routerCommandHeader = new QLabel(QStringLiteral("4G 路由器指令"), routerCard);
    auto *routerStatusHeader = new QLabel(QStringLiteral("状态（必须返回大写 OK）"), routerCard);
    routerCommandHeader->setObjectName(QStringLiteral("sectionHeader"));
    routerStatusHeader->setObjectName(QStringLiteral("sectionHeader"));
    routerGrid->addWidget(routerCommandHeader, 0, 0);
    routerGrid->addWidget(routerStatusHeader, 0, 1);

    for (int i = 0; i < routerCommandCount; ++i) {
        const RouterCommand command = static_cast<RouterCommand>(i);
        auto *button = new QPushButton(QStringLiteral("发送  %1").arg(routerCommandText(command)), routerCard);
        button->setObjectName(QStringLiteral("routerCommandButton"));
        button->setMinimumHeight(50);
        button->setCursor(Qt::PointingHandCursor);
        m_routerButtons[i] = button;

        auto *status = new QLabel(QStringLiteral("●  等待操作"), routerCard);
        status->setObjectName(QStringLiteral("operationStatus"));
        status->setProperty("state", QStringLiteral("idle"));
        status->setMinimumHeight(50);
        status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_routerStatusLabels[i] = status;

        routerGrid->addWidget(button, i + 1, 0);
        routerGrid->addWidget(status, i + 1, 1);
    }

    const int batchRow = routerCommandCount + 1;
    auto *divider = new QFrame(routerCard);
    divider->setObjectName(QStringLiteral("divider"));
    divider->setFrameShape(QFrame::HLine);
    routerGrid->addWidget(divider, batchRow, 0, 1, 2);

    m_batchButton = new QPushButton(QStringLiteral("一键配置全部指令"), routerCard);
    m_batchButton->setObjectName(QStringLiteral("oneClickButton"));
    m_batchButton->setMinimumHeight(52);
    m_batchButton->setCursor(Qt::PointingHandCursor);
    m_batchStatus = new QLabel(QStringLiteral("●  等待操作"), routerCard);
    m_batchStatus->setObjectName(QStringLiteral("operationStatus"));
    m_batchStatus->setProperty("state", QStringLiteral("idle"));
    m_batchStatus->setMinimumHeight(52);
    routerGrid->addWidget(m_batchButton, batchRow + 1, 0);
    routerGrid->addWidget(m_batchStatus, batchRow + 1, 1);

    routerPageLayout->addWidget(routerCard);
    routerPageLayout->addStretch();

    tabs->addTab(productionPage, QStringLiteral("生产配置"));
    tabs->addTab(routerPage, QStringLiteral("4G 路由器配置与检测"));
    pageLayout->addWidget(tabs);

    auto *logCard = new QFrame(central);
    logCard->setObjectName(QStringLiteral("card"));
    auto *logLayout = new QVBoxLayout(logCard);
    logLayout->setContentsMargins(20, 14, 20, 18);
    logLayout->setSpacing(10);
    auto *logHeaderLayout = new QHBoxLayout;
    auto *logHeader = new QLabel(QStringLiteral("收发记录"), logCard);
    logHeader->setObjectName(QStringLiteral("sectionHeader"));
    auto *clearLogButton = new QPushButton(QStringLiteral("清空"), logCard);
    clearLogButton->setObjectName(QStringLiteral("linkButton"));
    connect(clearLogButton, &QPushButton::clicked, this, [this] { m_log->clear(); });
    logHeaderLayout->addWidget(logHeader);
    logHeaderLayout->addStretch();
    logHeaderLayout->addWidget(clearLogButton);
    m_log = new QPlainTextEdit(logCard);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(300);
    m_log->setPlaceholderText(QStringLiteral("串口收发内容会显示在这里（密码不会记录）"));
    logLayout->addLayout(logHeaderLayout);
    logLayout->addWidget(m_log, 1);
    pageLayout->addWidget(logCard, 1);

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
        QComboBox {
            min-height: 36px;
            padding: 0 10px;
            border: 1px solid #CFD6E4;
            border-radius: 6px;
            background: #FFFFFF;
        }
        QComboBox:hover, QComboBox:focus {
            border-color: #2F6FED;
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
        QLineEdit#uidInput {
            padding: 0 12px;
            color: #172033;
            background: #FFFFFF;
            border: 1px solid #CFD6E4;
            border-radius: 6px;
            font-size: 13px;
        }
        QLineEdit#uidInput:hover, QLineEdit#uidInput:focus {
            border-color: #2F6FED;
        }
        QLineEdit#uidInput:disabled {
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
    const QString previousPort = m_portCombo->currentData().toString();
    m_portCombo->clear();

    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports) {
        QString label = info.portName();
        if (!info.description().isEmpty())
            label += QStringLiteral(" — ") + info.description();
        m_portCombo->addItem(label, info.portName());
    }

    const int previousIndex = m_portCombo->findData(previousPort);
    if (previousIndex >= 0)
        m_portCombo->setCurrentIndex(previousIndex);

    if (m_portCombo->count() == 0) {
        m_portCombo->addItem(QStringLiteral("未发现串口"), QString());
        m_connectButton->setEnabled(false);
    } else {
        m_connectButton->setEnabled(true);
    }
}

void MainWindow::toggleConnection()
{
    if (m_serial->isOpen()) {
        if (m_pendingOperation != Operation::Count)
            finishOperation(false, QStringLiteral("连接已由操作员断开"));
        if (m_pendingRouterCommand != RouterCommand::Count)
            finishRouterCommand(false, QStringLiteral("连接已由操作员断开"));
        else if (m_batchRunning) {
            m_batchRunning = false;
            m_batchIndex = -1;
            setBatchState(UiState::Failure, QStringLiteral("连接已由操作员断开"));
            setActionsEnabled(true);
        }
        m_serial->close();
        m_receiveBuffer.clear();
        setConnectionUi(false);
        appendLog(QStringLiteral("系统"), QStringLiteral("串口连接已断开"));
        return;
    }

    const QString portName = m_portCombo->currentData().toString();
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
}

void MainWindow::sendPassword()
{
    sendOperation(Operation::Password, SerialProtocol::passwordCommand(QString()), QStringLiteral("mei+***"));
}

void MainWindow::sendInitialTime()
{
    const QByteArray payload = SerialProtocol::initialTimeCommand();
    sendOperation(Operation::InitialTime, payload, QString::fromUtf8(payload).trimmed());
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
    if (!m_serial->isOpen()) {
        setRouterState(command, UiState::Failure, QStringLiteral("串口未连接"));
        return;
    }
    if (m_pendingOperation != Operation::Count || m_pendingRouterCommand != RouterCommand::Count
        || m_batchRunning) {
        setRouterState(command, UiState::Failure, QStringLiteral("已有指令正在等待设备响应"));
        return;
    }

    transmitRouterCommand(command);
}

void MainWindow::startOneClickConfiguration()
{
    if (!m_serial->isOpen()) {
        setBatchState(UiState::Failure, QStringLiteral("串口未连接"));
        return;
    }
    if (m_pendingOperation != Operation::Count || m_pendingRouterCommand != RouterCommand::Count
        || m_batchRunning) {
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
    setRouterState(command, UiState::Sending, QStringLiteral("已发送，等待大写 OK…"));
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
        setBatchState(UiState::Success, QStringLiteral("4 项指令均已返回大写 OK"));
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

void MainWindow::sendOperation(Operation operation, const QByteArray &payload, const QString &safeDescription)
{
    if (!m_serial->isOpen()) {
        setOperationState(operation, UiState::Failure, QStringLiteral("串口未连接"));
        return;
    }
    if (m_pendingOperation != Operation::Count || m_pendingRouterCommand != RouterCommand::Count
        || m_batchRunning) {
        setOperationState(operation, UiState::Failure,
                          QStringLiteral("已有指令正在等待设备响应"));
        return;
    }

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
    m_uidInput->setEnabled(enabled);
    for (QPushButton *button : m_routerButtons)
        button->setEnabled(enabled);
    m_batchButton->setEnabled(enabled);
}

void MainWindow::readSerialData()
{
    m_receiveBuffer.append(m_serial->readAll());

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
    }
}

void MainWindow::processReply(const QString &reply)
{
    QString safeReply = reply;
    if (m_pendingOperation == Operation::Password)
        safeReply.replace(QStringLiteral("mei+688"), QStringLiteral("mei+***"), Qt::CaseInsensitive);

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
        if (m_pendingOperation != Operation::Count)
            finishOperation(false, QStringLiteral("串口异常：%1").arg(errorText));
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
    m_log->appendPlainText(QStringLiteral("[%1] %2  %3").arg(timestamp, direction, message));
}

QString MainWindow::operationName(Operation operation) const
{
    switch (operation) {
    case Operation::Password: return QStringLiteral("输入密码");
    case Operation::InitialTime: return QStringLiteral("配置初始时间");
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

QString MainWindow::routerCommandText(RouterCommand command) const
{
    switch (command) {
    case RouterCommand::Gps:
        return QStringLiteral("AT*GPSCFG=4,5,0");
    case RouterCommand::Server:
        return QStringLiteral("AT*SERVER0=9,broker.emqx.io,1883");
    case RouterCommand::MqSet:
        return QStringLiteral("AT*MQSET0=00000001,,");
    case RouterCommand::MqTopic:
        return QStringLiteral("AT*MQTOP0=sendev/1/temperature,sendev/10/temperature");
    case RouterCommand::Count:
        break;
    }
    return {};
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
