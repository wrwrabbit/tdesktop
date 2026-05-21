/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/binary_location.h"

#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QStandardPaths>

#ifdef Q_OS_LINUX
#include <ksandbox.h>
#endif // Q_OS_LINUX

namespace Core {
namespace {

[[nodiscard]] QString NormalizedPath(const QString &path) {
	return QDir::cleanPath(path).toLower();
}

[[nodiscard]] bool PathStartsWith(
		const QString &path,
		const QString &prefix) {
	const auto cleanPath = NormalizedPath(path);
	const auto cleanPrefix = NormalizedPath(prefix);
	if (cleanPrefix.isEmpty()) {
		return false;
	}
	const auto withSlash = cleanPrefix.endsWith('/')
		? cleanPrefix
		: (cleanPrefix + '/');
	return cleanPath == cleanPrefix || cleanPath.startsWith(withSlash);
}

} // namespace

QString DownloadsFolderPath() {
	return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

QString SystemAppFolderPath() {
#ifdef Q_OS_WIN
	const auto pf = qEnvironmentVariable("PROGRAMFILES");
	if (!pf.isEmpty()) {
		return QDir(pf).absolutePath();
	}
#elif defined(Q_OS_MAC) // Q_OS_WIN
	return u"/Applications"_q;
#else // Q_OS_MAC
	return u"/usr"_q;
#endif // Q_OS_WIN
	return QString();
}

BinaryLocationCategory ClassifyBinaryLocation() {
	const auto exeDir = QDir(cExeDir()).absolutePath();

#ifdef Q_OS_LINUX
	if (KSandbox::isSnap() || KSandbox::isFlatpak()) {
		return BinaryLocationCategory::SystemAppFolder;
	}
#endif // Q_OS_LINUX

	const auto downloads = DownloadsFolderPath();
	if (!downloads.isEmpty() && PathStartsWith(exeDir, downloads)) {
		return BinaryLocationCategory::Downloads;
	}

#ifdef Q_OS_WIN
	{
		const auto pf = qEnvironmentVariable("PROGRAMFILES");
		const auto pf86 = qEnvironmentVariable("PROGRAMFILES(X86)");
		if ((!pf.isEmpty() && PathStartsWith(exeDir, QDir(pf).absolutePath()))
			|| (!pf86.isEmpty()
				&& PathStartsWith(exeDir, QDir(pf86).absolutePath()))) {
			return BinaryLocationCategory::SystemAppFolder;
		}
	}
#elif defined(Q_OS_MAC) // Q_OS_WIN
	if (PathStartsWith(exeDir, u"/Applications"_q)) {
		return BinaryLocationCategory::SystemAppFolder;
	}
#else // Q_OS_MAC
	if (PathStartsWith(exeDir, u"/usr"_q)
		|| PathStartsWith(exeDir, u"/opt"_q)
		|| PathStartsWith(exeDir, u"/bin"_q)
		|| PathStartsWith(exeDir, u"/sbin"_q)) {
		return BinaryLocationCategory::SystemAppFolder;
	}
#endif // Q_OS_WIN

#ifdef Q_OS_WIN
	{
		const auto rootPath = QDir::toNativeSeparators(
			QDir(exeDir).rootPath());
		const auto driveType = GetDriveTypeW(rootPath.toStdWString().c_str());
		if (driveType == DRIVE_REMOVABLE || driveType == DRIVE_CDROM) {
			return BinaryLocationCategory::RemovableDrive;
		}
	}
#endif // Q_OS_WIN

	return BinaryLocationCategory::CustomUser;
}

bool BinaryIsInDownloads() {
	return ClassifyBinaryLocation() == BinaryLocationCategory::Downloads;
}

bool BinaryIsInSystemAppFolder() {
	return ClassifyBinaryLocation() == BinaryLocationCategory::SystemAppFolder;
}

bool BinaryIsOnRemovableDrive() {
	return ClassifyBinaryLocation() == BinaryLocationCategory::RemovableDrive;
}

} // namespace Core
