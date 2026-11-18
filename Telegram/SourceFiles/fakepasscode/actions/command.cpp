#include "command.h"
#include "fakepasscode/log/fake_log.h"

#include <QProcess>

#ifdef Q_OS_WIN
#include <QRegularExpression>
#include <windows.h>
#endif

void FakePasscode::CommandAction::Execute() {
    FAKE_LOG(qsl("Execute command: %1").arg(command_));

#ifdef Q_OS_WIN
	static const auto shellPrefix = QRegularExpression(
		uR"re(^\s*(?:"((?:[^"]*[\\/])?cmd\.exe)"|((?:[^\s"]*[\\/])?cmd\.exe))(?=\s|$))re"_q,
		QRegularExpression::CaseInsensitiveOption);
	const auto shell = shellPrefix.match(command_);
	auto process = QProcess();
	if (shell.hasMatch()) {
		const auto quoted = shell.capturedStart(1) >= 0;
		process.setProgram(shell.captured(quoted ? 1 : 2));
		process.setNativeArguments(command_.mid(shell.capturedEnd()).trimmed());
	} else {
		process.setProgram(u"cmd.exe"_q);
		process.setNativeArguments(u"/d /s /c \""_q + command_ + u'"');
	}
	process.setCreateProcessArgumentsModifier([](auto *args) {
		args->flags |= CREATE_NO_WINDOW;
	});
	const auto started = process.startDetached();
#else
	QString executed_command = command_;
    auto started = QProcess::startDetached("bash", {"-c", executed_command});
#endif // Q_OS_WIN

    FAKE_LOG(qsl("Execute command: %1 executed %2").arg(command_).arg(started));
}

QByteArray FakePasscode::CommandAction::Serialize() const {
    QByteArray result;
    QDataStream stream(&result, QIODevice::ReadWrite);
    stream << static_cast<qint32>(ActionType::Command)
           << command_.toUtf8();
    return result;
}

FakePasscode::ActionType FakePasscode::CommandAction::GetType() const {
    return ActionType::Command;
}

FakePasscode::CommandAction::CommandAction(QByteArray inner_data)
: command_(QString::fromUtf8(inner_data)) {
    FAKE_LOG(qsl("Create command from QByteArray of size: %1").arg(inner_data.size()));
}

FakePasscode::CommandAction::CommandAction(QString command)
: command_(std::move(command)) {
}

const QString &FakePasscode::CommandAction::GetCommand() const {
    return command_;
}

void FakePasscode::CommandAction::SetCommand(QString command) {
    command_ = std::move(command);
}
