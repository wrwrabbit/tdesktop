/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/storage_location_switch.h"

#include "settings.h"
#include "platform/platform_specific.h"
#include "logs.h"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QThread>

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
		if (relative == u"working"_q) {
			continue;
		}
		const auto dstFile = target + '/' + relative;
		QDir().mkpath(QFileInfo(dstFile).absolutePath());
		QFile::remove(dstFile);
		if (!QFile::copy(srcFile, dstFile)) {
			LOG(("LocationSwitch: CopyDirRecursive failed to copy '%1' -> '%2'").arg(srcFile, dstFile));
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

TdataLocation CurrentTdataLocation() {
	if (IsCurrentlyInHome()) {
		return TdataLocation::Home;
	} else if (IsCurrentlyPortable()) {
		return TdataLocation::Portable;
	}
	return TdataLocation::Custom;
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

bool ScheduleSwitchToHomeWrittenTo(const QString &newExeDir) {
	const auto target = psAppDataPath();
	QDir().mkpath(target);
	const auto cleanNew = QDir::cleanPath(newExeDir);
	const auto flagInExeDir = cleanNew + u"/tdata_switch_pending"_q;
	const auto flagInTarget = QDir::cleanPath(target) + u"/tdata_switch_pending"_q;
	const auto content = (cWorkingDir() + '|' + target).toUtf8();
	LOG(("LocationSwitch: ScheduleSwitchToHomeWrittenTo newExeDir='%1' target='%2'"
		" flagInExeDir='%3' flagInTarget='%4' content='%5'").arg(
		newExeDir, target, flagInExeDir, flagInTarget,
		QString::fromUtf8(content)));
	const auto tryWrite = [&](const QString &path) {
		QFile f(path);
		if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			LOG(("LocationSwitch: failed to write flag to '%1': %2").arg(path, f.errorString()));
			return false;
		}
		f.write(content);
		LOG(("LocationSwitch: wrote flag to '%1'").arg(path));
		return true;
	};
	const auto ok = tryWrite(flagInExeDir) || tryWrite(flagInTarget);
	LOG(("LocationSwitch: ScheduleSwitchToHomeWrittenTo result: %1").arg(Logs::b(ok)));
	return ok;
}

bool ScheduleSwitchToCustom(const QString &targetDir) {
	return WriteSwitchFlag(cWorkingDir(), targetDir);
}

bool ScheduleSwitchToCustomWrittenTo(const QString &newExeDir) {
	const auto target = QDir::cleanPath(newExeDir);
	QDir().mkpath(target);
	const auto flagPath = target + u"/tdata_switch_pending"_q;
	const auto content = (cWorkingDir() + '|' + newExeDir).toUtf8();
	LOG(("LocationSwitch: ScheduleSwitchToCustomWrittenTo newExeDir='%1' flag='%2'").arg(
		newExeDir, flagPath));
	QFile f(flagPath);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		LOG(("LocationSwitch: failed to write flag to '%1': %2").arg(flagPath, f.errorString()));
		return false;
	}
	f.write(content);
	LOG(("LocationSwitch: wrote flag to '%1'").arg(flagPath));
	return true;
}

bool ApplyPendingSwitch() {
	const auto exeDirFlag = SwitchFlagFilePath();
	const auto appDataFlag = QDir::cleanPath(psAppDataPath())
		+ u"/tdata_switch_pending"_q;
	LOG(("LocationSwitch: ApplyPendingSwitch checking exeDirFlag='%1' (exists:%2) appDataFlag='%3' (exists:%4)").arg(
		exeDirFlag, Logs::b(QFile::exists(exeDirFlag)),
		appDataFlag, Logs::b(QFile::exists(appDataFlag))));
	const auto flagPath = QFile::exists(exeDirFlag)
		? exeDirFlag
		: QFile::exists(appDataFlag)
			? appDataFlag
			: QString();
	if (flagPath.isEmpty()) {
		LOG(("LocationSwitch: no pending switch flag found"));
		return false;
	}
	LOG(("LocationSwitch: using flag file '%1'").arg(flagPath));
	QFile flagFile(flagPath);
	if (!flagFile.exists()) {
		return false;
	}
	if (!flagFile.open(QIODevice::ReadOnly)) {
		return false;
	}
	const auto content = QString::fromUtf8(flagFile.readAll()).trimmed();
	flagFile.close();
	LOG(("LocationSwitch: flag content='%1'").arg(content));

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
	LOG(("LocationSwitch: source='%1' target='%2' sourceTdata='%3' (exists:%4) targetTdata='%5'").arg(
		sourceWorkingDir, targetWorkingDir,
		sourceTdata, Logs::b(QDir(sourceTdata).exists()),
		targetTdata));

	if (QDir(sourceTdata).exists()) {
		QDir().mkpath(targetWorkingDir);
		const auto renamed = QDir().rename(sourceTdata, targetTdata);
		LOG(("LocationSwitch: QDir::rename result: %1").arg(Logs::b(renamed)));
		if (!renamed) {
			LOG(("LocationSwitch: rename failed, trying recursive copy"));
			if (!CopyDirRecursive(sourceTdata, targetTdata)) {
				LOG(("LocationSwitch: recursive copy also failed, aborting"));
				QFile::remove(flagPath);
				return false;
			}
			LOG(("LocationSwitch: recursive copy succeeded, removing source tdata"));
			const auto removedTdata = QDir(sourceTdata).removeRecursively();
			LOG(("LocationSwitch: removeRecursively result: %1, sourceTdata still exists: %2").arg(
				Logs::b(removedTdata), Logs::b(QDir(sourceTdata).exists())));
		}
	} else {
		LOG(("LocationSwitch: sourceTdata does not exist, nothing to move"));
	}

	QFile::remove(flagPath);
	cForceWorkingDir(targetWorkingDir);
	LOG(("LocationSwitch: switch complete, working dir now '%1'").arg(targetWorkingDir));

	if (QDir::cleanPath(sourceWorkingDir) != QDir::cleanPath(targetWorkingDir)) {
		// Give the old process time to release file locks before attempting cleanup.
		QThread::msleep(1500);

		const auto cleanSource = QDir::cleanPath(sourceWorkingDir);
		for (const auto &name : {
			u"log.txt"_q,
			cExeName(),
			u"Updater.exe"_q,
			u"uninstall.exe"_q,
		}) {
			const auto filePath = cleanSource + '/' + name;
			if (!QFile::exists(filePath)) {
				LOG(("LocationSwitch: cleanup '%1': not present, skipping").arg(name));
				continue;
			}
			auto removed = false;
			for (int i = 0; !removed && i < 12; ++i) {
				QFile f(filePath);
				removed = f.remove();
				if (!removed) {
					LOG(("LocationSwitch: cleanup '%1' attempt %2 failed: %3").arg(
						name, QString::number(i + 1), f.errorString()));
					if (i + 1 < 12) {
						QThread::msleep(250);
					}
				}
			}
			LOG(("LocationSwitch: cleanup '%1': %2").arg(name, Logs::b(removed)));
		}

		const auto debugLogs = cleanSource + u"/DebugLogs"_q;
		if (QDir(debugLogs).exists()) {
			const auto removed = QDir(debugLogs).removeRecursively();
			LOG(("LocationSwitch: cleanup 'DebugLogs': %1").arg(Logs::b(removed)));
		}

		const auto modules = cleanSource + u"/modules"_q;
		if (QDir(modules).exists()) {
			const auto removed = QDir(modules).removeRecursively();
			LOG(("LocationSwitch: cleanup 'modules': %1").arg(Logs::b(removed)));
		}

		const auto workingPath = sourceTdata + u"/working"_q;
		LOG(("LocationSwitch: 'tdata/working' exists=%1 isFile=%2 isDir=%3").arg(
			Logs::b(QFile::exists(workingPath)),
			Logs::b(QFileInfo(workingPath).isFile()),
			Logs::b(QFileInfo(workingPath).isDir())));
		auto workingRemoved = false;
		for (int i = 0; !workingRemoved && i < 12; ++i) {
			QFile f(workingPath);
			workingRemoved = f.remove();
			if (!workingRemoved) {
				const auto ferr = f.errorString();
				workingRemoved = QDir(workingPath).removeRecursively();
				if (!workingRemoved) {
					LOG(("LocationSwitch: cleanup 'tdata/working' attempt %1 failed: %2").arg(
						QString::number(i + 1), ferr));
					if (i + 1 < 12) {
						QThread::msleep(250);
					}
				}
			}
		}
		LOG(("LocationSwitch: cleanup 'tdata/working': %1").arg(Logs::b(workingRemoved)));

		if (!workingRemoved && QFile::exists(workingPath)) {
			// The working file is still held by the dying old process. Rename the
			// entire source tdata directory so future startups from the old location
			// don't pick it up as a valid tdata (directory rename works on Windows
			// even when files inside are open).
			const auto tdataOld = sourceTdata + u"_old"_q;
			QDir(tdataOld).removeRecursively();
			const auto staleRenamed = QDir().rename(sourceTdata, tdataOld);
			LOG(("LocationSwitch: renamed stale sourceTdata to tdata_old: %1").arg(
				Logs::b(staleRenamed)));
			if (!staleRenamed) {
				LOG(("LocationSwitch: WARNING stale 'tdata/working' remains at '%1';"
					" future startups from '%2' may use wrong tdata").arg(
					workingPath, sourceWorkingDir));
			}
		} else {
			const auto sourceEntries = QDir(sourceTdata).entryList(
				QDir::AllEntries | QDir::NoDotAndDotDot);
			if (sourceEntries.isEmpty()) {
				const auto tdirRemoved = QDir().rmdir(sourceTdata);
				LOG(("LocationSwitch: cleanup 'tdata' dir: %1").arg(Logs::b(tdirRemoved)));
			} else {
				LOG(("LocationSwitch: sourceTdata not empty after cleanup, remaining: %1").arg(
					sourceEntries.join(u", "_q)));
			}
		}
#ifdef Q_OS_WIN
		if (QDir::cleanPath(sourceWorkingDir).toLower()
				== QDir::cleanPath(psAppDataPath()).toLower()) {
			RemoveStartMenuShortcut(cleanSource + '/' + cExeName());
		}
#endif
	}

	return true;
}

} // namespace Storage
