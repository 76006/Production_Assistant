#include "serialprotocol.h"

#include <QDateTime>
#include <QRegularExpression>

namespace SerialProtocol {

QByteArray withLineEnding(const QByteArray &command)
{
    QByteArray result = command.trimmed();
    result.append("\r\n");
    return result;
}

QByteArray passwordCommand(const QString &password)
{
    return withLineEnding(password.toUtf8());
}

QByteArray uidCommand(const QString &command)
{
    // 先剔除输入中可能夹带的换行，再追加一个 MSH 执行命令所需的 CR（0x0D）。
    QString payload = command;
    payload.remove(QLatin1Char('\r'));
    payload.remove(QLatin1Char('\n'));
    payload.remove(QChar(0x0085));
    payload.remove(QChar(0x2028));
    payload.remove(QChar(0x2029));
    QByteArray bytes = payload.toUtf8();
    bytes.append('\r');
    return bytes;
}

QByteArray uidCheckCommand()
{
    return withLineEnding(QByteArrayLiteral("set_board_UID"));
}

QByteArray initialTimeCommand(const QString &command)
{
    return withLineEnding(command.toUtf8());
}

QByteArray currentTimeCommand()
{
    const QDateTime now = QDateTime::currentDateTime();
    const QString command = QStringLiteral("date %1 %2 %3 %4 %5 %6")
                                .arg(now.date().year())
                                .arg(now.date().month())
                                .arg(now.date().day())
                                .arg(now.time().hour())
                                .arg(now.time().minute())
                                .arg(now.time().second());
    return withLineEnding(command.toUtf8());
}

QByteArray restartCommand()
{
    return withLineEnding(QByteArrayLiteral("reboot"));
}

ReplyResult classifyReply(const QString &reply)
{
    const QString text = reply.trimmed();
    if (text.isEmpty())
        return ReplyResult::Unknown;

    static const QRegularExpression successPattern(
        QStringLiteral(R"(^\s*(OK|ACK|SUCCESS|PASS|成功)(?:\b|\s|[:：,，]|$))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression failurePattern(
        QStringLiteral(R"(^\s*(ERR(?:OR)?|FAIL(?:ED)?|NACK|NG|失败)(?:\b|\s|[:：,，]|$))"),
        QRegularExpression::CaseInsensitiveOption);
    if (successPattern.match(text).hasMatch())
        return ReplyResult::Success;
    if (failurePattern.match(text).hasMatch())
        return ReplyResult::Failure;
    return ReplyResult::Unknown;
}

} // namespace SerialProtocol
