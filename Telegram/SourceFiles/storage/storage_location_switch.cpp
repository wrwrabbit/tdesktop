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
#include "fakepasscode/log/fake_log.h"

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
			FAKE_LOG(("LocationSwitch: CopyDirRecursive failed to copy '%1' -> '%2'").arg(srcFile, dstFile));
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
	FAKE_LOG(("LocationSwitch: ScheduleSwitchToHomeWrittenTo newExeDir='%1' target='%2'"
		" flagInExeDir='%3' flagInTarget='%4' content='%5'").arg(
		newExeDir, target, flagInExeDir, flagInTarget,
		QString::fromUtf8(content)));
	const auto tryWrite = [&](const QString &path) {
		QFile f(path);
		if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			FAKE_LOG(("LocationSwitch: failed to write flag to '%1': %2").arg(path, f.errorString()));
			return false;
		}
		f.write(content);
		FAKE_LOG(("LocationSwitch: wrote flag to '%1'").arg(path));
		return true;
	};
	const auto ok = tryWrite(flagInExeDir) || tryWrite(flagInTarget);
	FAKE_LOG(("LocationSwitch: ScheduleSwitchToHomeWrittenTo result: %1").arg(Logs::b(ok)));
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
	FAKE_LOG(("LocationSwitch: ScheduleSwitchToCustomWrittenTo newExeDir='%1' flag='%2'").arg(
		newExeDir, flagPath));
	QFile f(flagPath);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		FAKE_LOG(("LocationSwitch: failed to write flag to '%1': %2").arg(flagPath, f.errorString()));
		return false;
	}
	f.write(content);
	FAKE_LOG(("LocationSwitch: wrote flag to '%1'").arg(flagPath));
	return true;
}

bool ApplyPendingSwitch() {
	const auto exeDirFlag = SwitchFlagFilePath();
	const auto appDataFlag = QDir::cleanPath(psAppDataPath())
		+ u"/tdata_switch_pending"_q;
	FAKE_LOG(("LocationSwitch: ApplyPendingSwitch checking exeDirFlag='%1' (exists:%2) appDataFlag='%3' (exists:%4)").arg(
		exeDirFlag, Logs::b(QFile::exists(exeDirFlag)),
		appDataFlag, Logs::b(QFile::exists(appDataFlag))));
	const auto flagPath = QFile::exists(exeDirFlag)
		? exeDirFlag
		: QFile::exists(appDataFlag)
			? appDataFlag
			: QString();
	if (flagPath.isEmpty()) {
		FAKE_LOG(("LocationSwitch: no pending switch flag found"));
		return false;
	}
	FAKE_LOG(("LocationSwitch: using flag file '%1'").arg(flagPath));
	QFile flagFile(flagPath);
	if (!flagFile.exists()) {
		return false;
	}
	if (!flagFile.open(QIODevice::ReadOnly)) {
		return false;
	}
	const auto content = QString::fromUtf8(flagFile.readAll()).trimmed();
	flagFile.close();
	FAKE_LOG(("LocationSwitch: flag content='%1'").arg(content));

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
	FAKE_LOG(("LocationSwitch: source='%1' target='%2' sourceTdata='%3' (exists:%4) targetTdata='%5'").arg(
		sourceWorkingDir, targetWorkingDir,
		sourceTdata, Logs::b(QDir(sourceTdata).exists()),
		targetTdata));

	if (QDir(sourceTdata).exists()) {
		QDir().mkpath(targetWorkingDir);
		const auto renamed = QDir().rename(sourceTdata, targetTdata);
		FAKE_LOG(("LocationSwitch: QDir::rename result: %1").arg(Logs::b(renamed)));
		if (!renamed) {
			FAKE_LOG(("LocationSwitch: rename failed, trying recursive copy"));
			if (QDir(targetTdata).exists()) {
				const auto removed = QDir(targetTdata).removeRecursively();
				FAKE_LOG(("LocationSwitch: removed existing targetTdata: %1").arg(Logs::b(removed)));
			}
			if (!CopyDirRecursive(sourceTdata, targetTdata)) {
				FAKE_LOG(("LocationSwitch: recursive copy also failed, aborting"));
				QFile::remove(flagPath);
				return false;
			}
			FAKE_LOG(("LocationSwitch: recursive copy succeeded"));
			// Remove all source tdata files now (they are safely at target).
			// removeRecursively will delete everything except the locked 'working'
			// file — that is removed in the cleanup phase below.
			QDir(sourceTdata).removeRecursively();
		}
	} else {
		FAKE_LOG(("LocationSwitch: sourceTdata does not exist, nothing to move"));
	}

	QFile::remove(flagPath);
	cForceWorkingDir(targetWorkingDir);
	FAKE_LOG(("LocationSwitch: switch complete, working dir now '%1'").arg(targetWorkingDir));

	if (QDir::cleanPath(sourceWorkingDir) != QDir::cleanPath(targetWorkingDir)) {
		// Give the old process time to release file locks before cleanup.
		QThread::msleep(1500);

		const auto cleanSource = QDir::cleanPath(sourceWorkingDir);

		// These files are released well before the old process exits.
#ifdef Q_OS_WIN
		const auto updaterName = u"Updater.exe"_q;
#else
		const auto updaterName = u"Updater"_q;
#endif
		const auto filesToClean = {
			u"log.txt"_q,
			updaterName,
#ifdef Q_OS_WIN
			u"unins000.exe"_q,
			u"unins000.dat"_q,
#endif
		};
		for (const auto &name : filesToClean) {
			const auto filePath = cleanSource + '/' + name;
			if (QFile::exists(filePath)) {
				FAKE_LOG(("LocationSwitch: cleanup '%1': %2").arg(
					name, Logs::b(QFile::remove(filePath))));
			}
		}

		// Telegram.exe is locked until the process image is released.
		// Retry until the old process fully exits.
		{
			const auto exePath = cleanSource + '/' + cExeName();
			auto removed = false;
			for (int i = 0; !removed && i < 10; ++i) {
				if (i > 0) {
					QThread::msleep(250);
				}
				removed = QFile::remove(exePath);
			}
			FAKE_LOG(("LocationSwitch: cleanup '%1': %2").arg(cExeName(), Logs::b(removed)));
		}

		for (const auto &dirName : { u"DebugLogs"_q, u"modules"_q }) {
			const auto dirPath = cleanSource + '/' + dirName;
			if (QDir(dirPath).exists()) {
				FAKE_LOG(("LocationSwitch: cleanup '%1': %2").arg(
					dirName, Logs::b(QDir(dirPath).removeRecursively())));
			}
		}

		// tdata contains only 'working' at this point: all other files were
		// removed by removeRecursively() above (or the directory is already
		// gone if rename succeeded). 'working' was released by the old process
		// in CrashReports::Finish() before startDetached, so it should be
		// deletable on the first attempt after the 1.5s wait.
		if (QDir(sourceTdata).exists()) {
			const auto workingPath = sourceTdata + u"/working"_q;
			auto workingRemoved = !QFile::exists(workingPath);
			for (int i = 0; !workingRemoved && i < 10; ++i) {
				if (i > 0) {
					QThread::msleep(250);
				}
				workingRemoved = QFile::remove(workingPath);
			}
			FAKE_LOG(("LocationSwitch: cleanup 'tdata/working': %1").arg(Logs::b(workingRemoved)));
			if (workingRemoved) {
				// Directory is now empty; rmdir succeeds without iterating.
				FAKE_LOG(("LocationSwitch: cleanup 'tdata': %1").arg(
					Logs::b(QDir().rmdir(sourceTdata))));
			} else {
				// 'working' is still locked; rename the directory so future
				// startups from the old location don't find valid tdata there.
				const auto tdataOld = sourceTdata + u"_old"_q;
				QDir(tdataOld).removeRecursively();
				FAKE_LOG(("LocationSwitch: rename stale tdata to tdata_old: %1").arg(
					Logs::b(QDir().rename(sourceTdata, tdataOld))));
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
