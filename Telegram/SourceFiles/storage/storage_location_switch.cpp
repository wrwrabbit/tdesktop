/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/storage_location_switch.h"

#include "settings.h"
#include "platform/platform_specific.h"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>

namespace Storage {
namespace {

[[nodiscard]] bool CopyDirRecursive(
		const QString &source,
		const QString &target) {
	const auto sourceDir = QDir(source);
	if (!sourceDir.exists()) {
		return false;
	}
	QDir().mkpath(target);
	auto it = QDirIterator(
		source,
		QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
		QDirIterator::Subdirectories);
	while (it.hasNext()) {
		it.next();
		const auto srcFile = it.filePath();
		const auto relative = sourceDir.relativeFilePath(srcFile);
		const auto dstFile = target + '/' + relative;
		QDir().mkpath(QFileInfo(dstFile).absolutePath());
		if (!QFile::copy(srcFile, dstFile)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool WriteSwitchFlag(
		const QString &sourceWorkingDir,
		const QString &targetWorkingDir) {
	const auto flagPath = SwitchFlagFilePath();
	QFile flagFile(flagPath);
	if (!flagFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	const auto content = sourceWorkingDir + '|' + targetWorkingDir;
	flagFile.write(content.toUtf8());
	flagFile.close();
	return true;
}

} // namespace

QString SwitchFlagFilePath() {
	return cExeDir() + u"tdata_switch_pending"_q;
}

bool IsCurrentlyPortable() {
	const auto working = QDir(cWorkingDir()).canonicalPath();
	const auto exe = QDir(cExeDir()).canonicalPath();
	if (working.isEmpty() || exe.isEmpty()) {
		return false;
	}
	return working == exe || working.startsWith(exe + '/');
}

bool IsCurrentlyInHome() {
	const auto home = psAppDataPath();
	if (home.isEmpty()) {
		return false;
	}
	const auto working = QDir(cWorkingDir()).canonicalPath();
	const auto homeClean = QDir(home).canonicalPath();
	if (working.isEmpty() || homeClean.isEmpty()) {
		return false;
	}
	return working == homeClean || working.startsWith(homeClean + '/');
}

bool CanSwitchToPortable() {
	return !IsCurrentlyPortable();
}

bool CanSwitchToHome() {
	return !IsCurrentlyInHome();
}

bool ScheduleSwitchToPortable() {
	return WriteSwitchFlag(cWorkingDir(), cExeDir());
}

bool ScheduleSwitchToHome() {
	return WriteSwitchFlag(cWorkingDir(), psAppDataPath());
}

bool ScheduleSwitchToCustom(const QString &targetDir) {
	return WriteSwitchFlag(cWorkingDir(), targetDir);
}

bool ApplyPendingSwitch() {
	const auto flagPath = SwitchFlagFilePath();
	QFile flagFile(flagPath);
	if (!flagFile.exists()) {
		return false;
	}
	if (!flagFile.open(QIODevice::ReadOnly)) {
		return false;
	}
	const auto content = QString::fromUtf8(flagFile.readAll()).trimmed();
	flagFile.close();

	const auto sep = content.indexOf('|');
	if (sep < 0) {
		QFile::remove(flagPath);
		return false;
	}

	const auto sourceWorkingDir = content.left(sep);
	const auto targetWorkingDir = content.mid(sep + 1);
	if (sourceWorkingDir.isEmpty() || targetWorkingDir.isEmpty()) {
		QFile::remove(flagPath);
		return false;
	}

	const auto sourceTdata = QDir::cleanPath(sourceWorkingDir) + u"/tdata"_q;
	const auto targetTdata = QDir::cleanPath(targetWorkingDir) + u"/tdata"_q;

	if (QDir(sourceTdata).exists()) {
		QDir().mkpath(targetWorkingDir);
		if (!QDir().rename(sourceTdata, targetTdata)) {
			if (!CopyDirRecursive(sourceTdata, targetTdata)) {
				QFile::remove(flagPath);
				return false;
			}
			QDir(sourceTdata).removeRecursively();
		}
	}

	QFile::remove(flagPath);
	cForceWorkingDir(targetWorkingDir);
	return true;
}

} // namespace Storage
