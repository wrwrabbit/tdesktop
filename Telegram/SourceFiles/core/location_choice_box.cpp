/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/location_choice_box.h"

#include "core/application.h"
#include "core/binary_location.h"
#include "storage/storage_location_switch.h"
#include "lang/lang_keys.h"
#include "ui/layers/show.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/vertical_list.h"
#include "ui/text/text_utilities.h"
#include "logs.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtWidgets/QFileDialog>

#ifdef Q_OS_WIN
#include "platform/win/specific_win.h"
#endif // Q_OS_WIN

#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Core {
namespace {

[[nodiscard]] bool TryCopyBinary(const QString &targetDir) {
	const auto srcExe = cExeDir() + cExeName();
	const auto dstExe = targetDir + '/' + cExeName();
	LOG(("LocationBox: TryCopyBinary src='%1' dst='%2'").arg(srcExe, dstExe));
	QFile::remove(dstExe);
	QDir().mkpath(targetDir);
	if (QFile::copy(srcExe, dstExe)) {
		LOG(("LocationBox: QFile::copy succeeded"));
		return true;
	}
	LOG(("LocationBox: QFile::copy failed"));
	return false;
}

void RelaunchFrom(const QString &newExePath) {
	LOG(("LocationBox: relaunching from '%1'").arg(newExePath));
	QProcess::startDetached(newExePath, {});
	Core::Quit();
}

[[nodiscard]] QString LocationStatusText(
		Core::BinaryLocationCategory category) {
	const auto exeDir = QDir::toNativeSeparators(
		QDir(cExeDir()).absolutePath());
	switch (category) {
	case BinaryLocationCategory::SystemAppFolder:
		return tr::lng_ptg_location_installed(tr::now, lt_path, exeDir);
	case BinaryLocationCategory::RemovableDrive:
		return tr::lng_ptg_location_removable(tr::now, lt_path, exeDir);
	case BinaryLocationCategory::Downloads:
		return tr::lng_ptg_location_downloads(tr::now);
	default:
		return tr::lng_ptg_location_running_from(tr::now, lt_path, exeDir);
	}
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

void FillLocationChoiceBoxImpl(not_null<Ui::GenericBox*> box) {
	box->setTitle(tr::lng_ptg_location_move_title());

	const auto layout = box->verticalLayout();
	const auto binaryCategory = Core::ClassifyBinaryLocation();
	const auto tdataLocation = Storage::CurrentTdataLocation();
	const auto exeDir = QDir::toNativeSeparators(
		QDir(cExeDir()).absolutePath());

	Ui::AddSkip(layout);

	const auto isDownloads = (binaryCategory == BinaryLocationCategory::Downloads);
	const auto &labelStyle = isDownloads
		? st::ptgLocationWarningLabel
		: st::ptgLocationLabel;
	layout->add(
		object_ptr<Ui::FlatLabel>(
			layout,
			tr::lng_ptg_location_running_at(tr::now, lt_path, exeDir),
			labelStyle),
		st::boxRowPadding);

	const auto isSystemApp = (binaryCategory == BinaryLocationCategory::SystemAppFolder);

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
	if (!isSystemApp) {
		const auto appDataPath = QDir(psAppDataPath()).absolutePath();

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
				box->uiShow()->show(Box([=](not_null<Ui::GenericBox*> confirm) {
					confirm->setTitle(tr::lng_ptg_location_confirm_title());
					confirm->addRow(object_ptr<Ui::FlatLabel>(
						confirm,
						tr::lng_ptg_location_confirm_text(
							tr::now,
							lt_path,
							QDir::toNativeSeparators(appDataPath)),
						st::boxLabel));
					confirm->addButton(tr::lng_settings_save(), [=] {
						confirm->closeBox();
						box->closeBox();
						if (TryCopyBinary(appDataPath)) {
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
	if (!isSystemApp) {
		constexpr auto kApplicationsPath = u"/Applications/Telegram Desktop"_q;
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
							RelaunchFrom(kApplicationsPath + '/' + cExeName());
						} else {
							box->uiShow()->showToast(
								tr::lng_ptg_location_error_copy(tr::now));
						}
					});
					confirm->addButton(tr::lng_cancel(), [=] { confirm->closeBox(); });
				}));
			});
	}
#endif // Q_OS_WIN / Q_OS_MAC

	if (!isSystemApp) {
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
					QDir::homePath());
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
					confirm->setTitle(tr::lng_ptg_location_confirm_title());
					confirm->addRow(object_ptr<Ui::FlatLabel>(
						confirm,
						tr::lng_ptg_location_confirm_text(
							tr::now,
							lt_path,
							QDir::toNativeSeparators(cleanChosen)),
						st::boxLabel));
					confirm->addButton(tr::lng_settings_save(), [=] {
						confirm->closeBox();
						box->closeBox();
						if (TryCopyBinary(cleanChosen)) {
							Storage::ScheduleSwitchToCustom(cleanChosen);
							RelaunchFrom(cleanChosen + '/' + cExeName());
						} else {
							box->uiShow()->showToast(
								tr::lng_ptg_location_error_copy(tr::now));
						}
					});
					confirm->addButton(tr::lng_cancel(), [=] { confirm->closeBox(); });
				}));
			});
	}

	if (tdataLocation != Storage::TdataLocation::Home) {
		AddOptionCard(
			layout,
			tr::lng_ptg_location_card_data_home_title(tr::now),
			tr::lng_ptg_location_card_data_home_desc(tr::now),
			{
				tr::lng_ptg_location_card_data_home_pro1(tr::now),
				tr::lng_ptg_location_card_data_home_pro2(tr::now),
				u"\u2212 "_q + tr::lng_ptg_location_card_data_home_con1(tr::now),
			},
			tr::lng_ptg_location_card_data_home_btn(),
			[=] {
				box->uiShow()->show(Box([=](not_null<Ui::GenericBox*> confirm) {
					confirm->setTitle(tr::lng_ptg_location_confirm_title());
					confirm->addRow(object_ptr<Ui::FlatLabel>(
						confirm,
						tr::lng_ptg_location_confirm_text_simple(tr::now),
						st::boxLabel));
					confirm->addButton(tr::lng_settings_save(), [=] {
						confirm->closeBox();
						box->closeBox();
						Storage::ScheduleSwitchToHome();
						Core::Restart();
					});
					confirm->addButton(tr::lng_cancel(), [=] { confirm->closeBox(); });
				}));
			});
	}

	if (tdataLocation == Storage::TdataLocation::Home && !isSystemApp) {
		AddOptionCard(
			layout,
			tr::lng_ptg_location_card_portable_title(tr::now),
			tr::lng_ptg_location_card_portable_desc(tr::now),
			{
				tr::lng_ptg_location_card_portable_pro1(tr::now),
				tr::lng_ptg_location_card_portable_pro2(tr::now),
				u"\u2212 "_q + tr::lng_ptg_location_card_portable_con1(tr::now),
			},
			tr::lng_ptg_location_card_portable_btn(),
			[=] {
				box->uiShow()->show(Box([=](not_null<Ui::GenericBox*> confirm) {
					confirm->setTitle(tr::lng_ptg_location_confirm_title());
					confirm->addRow(object_ptr<Ui::FlatLabel>(
						confirm,
						tr::lng_ptg_location_confirm_text_simple(tr::now),
						st::boxLabel));
					confirm->addButton(tr::lng_settings_save(), [=] {
						confirm->closeBox();
						box->closeBox();
						Storage::ScheduleSwitchToPortable();
						Core::Restart();
					});
					confirm->addButton(tr::lng_cancel(), [=] { confirm->closeBox(); });
				}));
			});
	}

	Ui::AddSkip(layout, st::ptgLocationCardSkip);

	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

void FillLocationChoiceBox(not_null<Ui::GenericBox*> box) {
	FillLocationChoiceBoxImpl(box);
}

void ShowLocationChoiceBox(not_null<Ui::Show*> show) {
	show->showBox(Box(FillLocationChoiceBoxImpl));
}

} // namespace Core
