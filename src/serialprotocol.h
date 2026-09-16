#pragma once

#include <QByteArray>
#include <QString>

namespace SerialProtocol {

enum class ReplyResult {
    Unknown,
    Success,
    Failure
};

// 下位机协议不同时，只需要集中修改下面这些组包函数。
QByteArray passwordCommand(const QString &password);
QByteArray uidCommand(const QString &command);
QByteArray uidCheckCommand();
QByteArray initialTimeCommand(const QString &command);
QByteArray currentTimeCommand();
QByteArray restartCommand();

ReplyResult classifyReply(const QString &reply);
QByteArray withLineEnding(const QByteArray &command);

} // namespace SerialProtocol
