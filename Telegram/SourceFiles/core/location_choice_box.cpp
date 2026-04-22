/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/location_choice_box.h"

#include "core/application.h"
#include "core/binary_location.h"
#include "core/crash_reports.h"
#include "storage/storage_location_switch.h"
#include "lang/lang_keys.h"
#include "ui/layers/show.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/vertical_list.h"
#include "ui/text/text_utilities.h"
#include "logs.h"
#include "fakepasscode/log/fake_log.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtWidgets/QFileDialog>

#include "platform/platform_specific.h"

#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Core {
namespace {

#ifdef Q_OS_MAC
[[nodiscard]] QString FindBundleRoot() {
	auto dir = QDir(cExeDir());
	dir.cdUp(); // Contents/MacOS → Contents
	dir.cdUp(); // Contents → Telegram.app
	if (dir.dirName().endsWith(u".app"_q)) {
		return dir.absolutePath();
	}
	return QString();
}

[[nodiscard]] bool CopyBundleRecursive(
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
		QFile::remove(dstFile);
		if (!QFile::copy(srcFile, dstFile)) {
			FAKE_LOG(("LocationBox: bundle copy failed '%1' -> '%2'").arg(srcFile, dstFile));
			return false;
		}
	}
	return true;
}
#endif // Q_OS_MAC

[[nodiscard]] QString DisplayDirPath(const QString &exeDir) {
#ifdef Q_OS_MAC
	auto dir = QDir(cExeDir());
	dir.cdUp();
	dir.cdUp();
	if (dir.dirName().endsWith(u".app"_q)) {
		return QDir::toNativeSeparators(dir.absolutePath());
	}
#elif defined(Q_OS_LINUX)
	const auto home = QDir::homePath();
	if (!home.isEmpty() && exeDir.startsWith(home)) {
		return u"~"_q + exeDir.mid(home.size());
	}
#endif
	return exeDir;
}

void AddOptionCard(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		const QString &description,
		std::vector<QString> bullets,
		rpl::producer<QString> buttonText,
		Fn<void()> callback) {
	Ui::AddSkip(container, st::ptgLocationCardSkip);
	Ui::AddSubsectionTitle(container, rpl::single(title));

	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			description,
			st::ptgLocationCardDesc),
		st::boxRowPadding);

	for (const auto &bullet : bullets) {
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				u"\u2022 "_q + bullet,
				st::ptgLocationCardBullet),
			style::margins(
				st::boxRowPadding.left(),
				2,
				st::boxRowPadding.right(),
				0));
	}

	Ui::AddSkip(container, st::ptgLocationSectionSkip);

	container->add(
		object_ptr<Ui::RoundButton>(
			container,
			std::move(buttonText),
			st::ptgLocationCardButton),
		st::boxRowPadding)->addClickHandler(std::move(callback));
}

void FillLocationChoiceBoxImpl(not_null<Ui::GenericBox*> box, bool firstRun) {
	box->setTitle(firstRun
		? tr::lng_ptg_location_setup_title()
		: tr::lng_ptg_location_move_title());

	const auto layout = box->verticalLayout();
	const auto binaryCategory = Core::ClassifyBinaryLocation();
	const auto exeDir = QDir::toNativeSeparators(
		QDir(cExeDir()).absolutePath());
	const auto displayDir = DisplayDirPath(exeDir);

	Ui::AddSkip(layout);

	const auto isDownloads = (binaryCategory == BinaryLocationCategory::Downloads);
	const auto &labelStyle = isDownloads
		? st::ptgLocationWarningLabel
		: st::ptgLocationLabel;
	layout->add(
		object_ptr<Ui::FlatLabel>(
			layout,
			tr::lng_ptg_location_running_at(tr::now, lt_path, displayDir),
			labelStyle),
		st::boxRowPadding);

	const auto isSystemApp = (binaryCategory == BinaryLocationCategory::SystemAppFolder);

	auto isInstallerManaged = false;
#ifdef Q_OS_WIN
	isInstallerManaged = QFile::exists(cExeDir() + u"uninstall.exe"_q);
	if (isInstallerManaged) {
		AddOptionCard(
			layout,
			tr::lng_ptg_location_card_make_portable_title(tr::now),
			tr::lng_ptg_location_card_make_portable_desc(tr::now),
			{
				tr::lng_ptg_location_card_make_portable_pro1(tr::now),
				tr::lng_ptg_location_card_make_portable_pro2(tr::now),
				tr::lng_ptg_location_card_make_portable_pro3(tr::now),
				tr::lng_ptg_location_card_make_portable_pro4(tr::now),
			},
			tr::lng_ptg_location_card_make_portable_btn(),
			[=] {
				RemoveInnoSetupRegistryKey();
				QFile::remove(cExeDir() + u"uninstall.exe"_q);
				RemoveStartMenuShortcut(cExeDir() + cExeName());
				box->closeBox();
				box->uiShow()->showBox(Box([firstRun](not_null<Ui::GenericBox*> newBox) {
					FillLocationChoiceBoxImpl(newBox, firstRun);
				}));
			});
	} else
#endif // Q_OS_WIN
	if (isSystemApp) {
		Ui::AddSkip(layout, st::ptgLocationSectionSkip);
		layout->add(
			object_ptr<Ui::FlatLabel>(
				layout,
				tr::lng_ptg_location_installer_info(tr::now),
				st::ptgLocationCardDesc),
			st::boxRowPadding);
	}

#ifdef Q_OS_WIN
	const auto appDataPath = QDir(psAppDataPath()).absolutePath();
	const auto isAlreadyInAppData = (QDir::cleanPath(QDir(cExeDir()).absolutePath()).toLower()
		== QDir::cleanPath(appDataPath).toLower());
	if (!isSystemApp && !isInstallerManaged && !isAlreadyInAppData) {
		AddOptionCard(
			layout,
			tr::lng_ptg_location_card_appdata_title(tr::now),
			tr::lng_ptg_location_card_appdata_desc(tr::now),
			{
				tr::lng_ptg_location_card_appdata_pro1(tr::now),
				tr::lng_ptg_location_card_appdata_pro2(tr::now),
				tr::lng_ptg_location_card_appdata_pro3(tr::now),
				u"\u2212 "_q + tr::lng_ptg_location_card_appdata_con1(tr::now),
			},
			tr::lng_ptg_location_card_appdata_btn(),
			[=] {
				const auto targetExists = QFile::exists(appDataPath + '/' + cExeName())
					|| QDir(appDataPath + u"/tdata"_q).exists();
				box->uiShow()->show(Box([=](not_null<Ui::GenericBox*> confirm) {
					confirm->setTitle(tr::lng_ptg_location_confirm_title());
					confirm->addRow(object_ptr<Ui::FlatLabel>(
						confirm,
						targetExists
							? tr::lng_ptg_location_confirm_overwrite(tr::now)
							: tr::lng_ptg_location_confirm_text(
								tr::now,
								lt_path,
								QDir::toNativeSeparators(appDataPath)),
						st::boxLabel));
					confirm->addButton(tr::lng_settings_save(), [=] {
						confirm->closeBox();
						box->closeBox();
						if (TryCopyBinary(appDataPath)) {
							CopyCompanionFiles(appDataPath);
							CreateStartMenuShortcut(appDataPath + '/' + cExeName());
							Storage::ScheduleSwitchToHomeWrittenTo(appDataPath);
							RelaunchFrom(appDataPath + '/' + cExeName());
						} else {
							box->uiShow()->show(Box([=](not_null<Ui::GenericBox*> err) {
								err->addRow(object_ptr<Ui::FlatLabel>(
									err,
									tr::lng_ptg_location_error_copy(tr::now),
									st::boxLabel));
								err->addButton(tr::lng_close(), [=] { err->closeBox(); });
							}));
						}
					});
					confirm->addButton(tr::lng_cancel(), [=] { confirm->closeBox(); });
				}));
			});
	}
#elif defined(Q_OS_MAC)
	{
		const auto kApplicationsPath = u"/Applications"_q;
		const auto applicationsWritable = QFileInfo(kApplicationsPath).isWritable();
		if (!isSystemApp && applicationsWritable) {
			AddOptionCard(
				layout,
				tr::lng_ptg_location_card_applications_title(tr::now),
				tr::lng_ptg_location_card_applications_desc(tr::now),
				{
					tr::lng_ptg_location_card_applications_pro1(tr::now),
					tr::lng_ptg_location_card_applications_pro2(tr::now),
					u"\u2212 "_q + tr::lng_ptg_location_card_applications_con1(tr::now),
				},
				tr::lng_ptg_location_card_applications_btn(),
				[=] {
					box->uiShow()->show(Box([=](not_null<Ui::GenericBox*> confirm) {
						confirm->setTitle(tr::lng_ptg_location_confirm_title());
						confirm->addRow(object_ptr<Ui::FlatLabel>(
							confirm,
							tr::lng_ptg_location_confirm_text(
								tr::now,
								lt_path,
								kApplicationsPath),
							st::boxLabel));
						confirm->addButton(tr::lng_settings_save(), [=] {
							confirm->closeBox();
							box->closeBox();
							if (TryCopyBinary(kApplicationsPath)) {
								Storage::ScheduleSwitchToHomeWrittenTo(kApplicationsPath);
								RelaunchFrom(RelaunchExePath(kApplicationsPath));
							} else {
								box->uiShow()->showToast(
									tr::lng_ptg_location_error_copy(tr::now));
							}
						});
						confirm->addButton(tr::lng_cancel(), [=] { confirm->closeBox(); });
					}));
				});
		}
	}
#else // Q_OS_LINUX
	{
		const auto homeBinPath = QDir::homePath() + u"/.local/bin"_q;
		const auto isAlreadyInHome = (QDir::cleanPath(QDir(cExeDir()).absolutePath())
			== QDir::cleanPath(homeBinPath));
		if (!isSystemApp && !isAlreadyInHome) {
			AddOptionCard(
				layout,
				tr::lng_ptg_location_card_home_title(tr::now),
				tr::lng_ptg_location_card_home_desc(tr::now),
				{
					tr::lng_ptg_location_card_home_pro1(tr::now),
					tr::lng_ptg_location_card_home_pro2(tr::now),
					tr::lng_ptg_location_card_home_pro3(tr::now),
				},
				tr::lng_ptg_location_card_home_btn(),
				[=] {
					const auto targetExists = QFile::exists(homeBinPath + '/' + cExeName());
					box->uiShow()->show(Box([=](not_null<Ui::GenericBox*> confirm) {
						confirm->setTitle(tr::lng_ptg_location_confirm_title());
						confirm->addRow(object_ptr<Ui::FlatLabel>(
							confirm,
							targetExists
								? tr::lng_ptg_location_confirm_overwrite(tr::now)
								: tr::lng_ptg_location_confirm_text(
									tr::now,
									lt_path,
									u"~/.local/bin"_q),
							st::boxLabel));
						confirm->addButton(tr::lng_settings_save(), [=] {
							confirm->closeBox();
							box->closeBox();
							if (TryCopyBinary(homeBinPath)) {
								CopyCompanionFiles(homeBinPath);
								Storage::ScheduleSwitchToHomeWrittenTo(homeBinPath);
								RelaunchFrom(RelaunchExePath(homeBinPath));
							} else {
								box->uiShow()->show(Box([=](not_null<Ui::GenericBox*> err) {
									err->addRow(object_ptr<Ui::FlatLabel>(
										err,
										tr::lng_ptg_location_error_copy(tr::now),
										st::boxLabel));
									err->addButton(tr::lng_close(), [=] { err->closeBox(); });
								}));
							}
						});
						confirm->addButton(tr::lng_cancel(), [=] { confirm->closeBox(); });
					}));
				});
		}
	}
#endif // Q_OS_WIN / Q_OS_MAC / Q_OS_LINUX

	if (!isSystemApp && !isInstallerManaged) {
		AddOptionCard(
			layout,
			tr::lng_ptg_location_card_custom_title(tr::now),
			tr::lng_ptg_location_card_custom_desc(tr::now),
			{
				tr::lng_ptg_location_card_custom_pro1(tr::now),
				tr::lng_ptg_location_card_custom_pro2(tr::now),
				tr::lng_ptg_location_card_custom_pro3(tr::now),
				u"\u2212 "_q + tr::lng_ptg_location_card_custom_con1(tr::now),
				u"\u2212 "_q + tr::lng_ptg_location_card_custom_con2(tr::now),
			},
			tr::lng_ptg_location_card_custom_btn(),
			[=] {
				const auto chosen = QFileDialog::getExistingDirectory(
					box->window(),
					tr::lng_ptg_location_card_custom_title(tr::now),
					cExeDir());
				if (chosen.isEmpty()) {
					return;
				}
				const auto cleanChosen = QDir::cleanPath(chosen);
				const auto cleanExe = QDir::cleanPath(cExeDir());
				if (cleanChosen == cleanExe) {
					box->uiShow()->showToast(
						tr::lng_ptg_location_same_dir(tr::now));
					return;
				}
				const auto sysFolder = Core::SystemAppFolderPath();
				if (!sysFolder.isEmpty()) {
					const auto cleanSys = QDir::cleanPath(sysFolder);
					if (cleanChosen.startsWith(cleanSys, Qt::CaseInsensitive)) {
						box->uiShow()->showToast(
							tr::lng_ptg_location_error_folder(tr::now));
						return;
					}
				}
				box->uiShow()->show(Box([=](not_null<Ui::GenericBox*> confirm) {
					const auto targetExists = QFile::exists(cleanChosen + '/' + cExeName())
						|| QDir(cleanChosen + u"/tdata"_q).exists();
					confirm->setTitle(tr::lng_ptg_location_confirm_title());
					confirm->addRow(object_ptr<Ui::FlatLabel>(
						confirm,
						targetExists
							? tr::lng_ptg_location_confirm_overwrite(tr::now)
							: tr::lng_ptg_location_confirm_text(
								tr::now,
								lt_path,
								QDir::toNativeSeparators(cleanChosen)),
						st::boxLabel));
					confirm->addButton(tr::lng_settings_save(), [=] {
						confirm->closeBox();
						box->closeBox();
						if (TryCopyBinary(cleanChosen)) {
#ifndef Q_OS_MAC
							CopyCompanionFiles(cleanChosen);
#endif // Q_OS_MAC
							Storage::ScheduleSwitchToCustomWrittenTo(cleanChosen);
							RelaunchFrom(RelaunchExePath(cleanChosen));
						} else {
							box->uiShow()->showToast(
								tr::lng_ptg_location_error_copy(tr::now));
						}
					});
					confirm->addButton(tr::lng_cancel(), [=] { confirm->closeBox(); });
				}));
			});
	}

	Ui::AddSkip(layout, st::ptgLocationCardSkip);

	if (firstRun) {
		box->addButton(tr::lng_ptg_location_continue_downloads(), [=] { box->closeBox(); });
	} else {
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}
}

} // namespace

bool TryCopyBinary(const QString &targetDir) {
#ifdef Q_OS_MAC
	const auto bundleRoot = FindBundleRoot();
	if (!bundleRoot.isEmpty()) {
		const auto bundleName = QFileInfo(bundleRoot).fileName();
		const auto dstBundle = targetDir + '/' + bundleName;
		FAKE_LOG(("LocationBox: TryCopyBinary (bundle) src='%1' dst='%2'").arg(bundleRoot, dstBundle));
		if (QDir(dstBundle).exists()) {
			QDir(dstBundle).removeRecursively();
		}
		if (CopyBundleRecursive(bundleRoot, dstBundle)) {
			FAKE_LOG(("LocationBox: bundle copy succeeded"));
			return true;
		}
		FAKE_LOG(("LocationBox: bundle copy failed"));
		return false;
	}
#endif // Q_OS_MAC
	const auto srcExe = cExeDir() + cExeName();
	const auto dstExe = targetDir + '/' + cExeName();
	FAKE_LOG(("LocationBox: TryCopyBinary src='%1' dst='%2'").arg(srcExe, dstExe));
	QFile::remove(dstExe);
	QDir().mkpath(targetDir);
	if (QFile::copy(srcExe, dstExe)) {
		FAKE_LOG(("LocationBox: QFile::copy succeeded"));
		return true;
	}
	FAKE_LOG(("LocationBox: QFile::copy failed"));
	return false;
}

void CopyCompanionFiles(const QString &targetDir) {
#ifdef Q_OS_WIN
	const auto updaterName = u"Updater.exe"_q;
#else
	const auto updaterName = u"Updater"_q;
#endif
	const auto updaterSrc = cExeDir() + updaterName;
	if (QFile::exists(updaterSrc)) {
		const auto updaterDst = targetDir + '/' + updaterName;
		QFile::remove(updaterDst);
		const auto copied = QFile::copy(updaterSrc, updaterDst);
		FAKE_LOG(("LocationBox: copy %1: %2").arg(updaterName, Logs::b(copied)));
	}
	const auto modulesSrc = cExeDir() + u"modules"_q;
	if (QDir(modulesSrc).exists()) {
		auto it = QDirIterator(
			modulesSrc,
			QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
			QDirIterator::Subdirectories);
		const auto srcDir = QDir(modulesSrc);
		while (it.hasNext()) {
			it.next();
			const auto relative = srcDir.relativeFilePath(it.filePath());
			const auto dstFile = targetDir + u"/modules/"_q + relative;
			QDir().mkpath(QFileInfo(dstFile).absolutePath());
			QFile::remove(dstFile);
			const auto copied = QFile::copy(it.filePath(), dstFile);
			FAKE_LOG(("LocationBox: copy modules/%1: %2").arg(relative, Logs::b(copied)));
		}
	}
}

void RelaunchFrom(const QString &newExePath) {
	FAKE_LOG(("LocationBox: relaunching from '%1'").arg(newExePath));
	CrashReports::Finish();
	QProcess::startDetached(newExePath, {});
	Core::Quit();
}

QString RelaunchExePath(const QString &targetDir) {
#ifdef Q_OS_MAC
	const auto bundleRoot = FindBundleRoot();
	if (!bundleRoot.isEmpty()) {
		const auto bundleName = QFileInfo(bundleRoot).fileName();
		return targetDir + '/' + bundleName
			+ u"/Contents/MacOS/"_q + cExeName();
	}
#endif // Q_OS_MAC
	return targetDir + '/' + cExeName();
}

void FillLocationChoiceBox(not_null<Ui::GenericBox*> box) {
	FillLocationChoiceBoxImpl(box, false);
}

void ShowLocationChoiceBox(not_null<Ui::Show*> show) {
	show->showBox(Box([](not_null<Ui::GenericBox*> box) {
		FillLocationChoiceBoxImpl(box, false);
	}));
}

void ShowLocationChoiceBoxFirstRun(not_null<Ui::Show*> show) {
	show->showBox(Box([](not_null<Ui::GenericBox*> box) {
		FillLocationChoiceBoxImpl(box, true);
	}));
}

} // namespace Core
