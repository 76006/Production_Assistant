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
    Q_UNUSED(password)
    return withLineEnding(QByteArrayLiteral("mei+688"));
}

QByteArray uidCommand(const QString &command)
{
    // UID 输入内容必须逐字节原样发送，不增加任何命令头、命令尾或换行符。
    return command.toUtf8();
}

QByteArray initialTimeCommand()
{
    return withLineEnding(QByteArrayLiteral("date 2025 10 30 0 0 0"));
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
    return withLineEnding(QByteArrayLiteral("reset"));
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
