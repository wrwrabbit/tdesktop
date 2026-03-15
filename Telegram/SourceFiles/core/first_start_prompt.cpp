/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/first_start_prompt.h"

#include "core/binary_location.h"
#include "storage/storage_location_switch.h"
#include "core/application.h"
#include "settings.h"
#include "lang/lang_keys.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif // Q_OS_WIN

namespace Core {
namespace {

[[nodiscard]] QString PromptMarkerPath() {
	return cExeDir() + u".ptg_location_prompt_shown"_q;
}

[[nodiscard]] bool PromptAlreadyShown() {
	return QFile::exists(PromptMarkerPath());
}

void MarkPromptShown() {
	QFile file(PromptMarkerPath());
	[[maybe_unused]] const auto opened = file.open(QIODevice::WriteOnly);
}

bool TryCopyBinaryTo(const QString &targetDir) {
	const auto srcExe = cExeDir() + cExeName();
	const auto dstExe = targetDir + '/' + cExeName();
	if (QDir().mkpath(targetDir) && QFile::copy(srcExe, dstExe)) {
		return true;
	}
#ifdef Q_OS_WIN
	const auto srcNative = QDir::toNativeSeparators(srcExe);
	const auto dstNative = QDir::toNativeSeparators(dstExe);
	const auto mkdirArgs = u"/c mkdir \"%1\""_q
		.arg(QDir::toNativeSeparators(targetDir));
	const auto copyArgs = u"/c copy /Y \"%1\" \"%2\""_q
		.arg(srcNative, dstNative);

	SHELLEXECUTEINFOW mkdirSei = {};
	mkdirSei.cbSize = sizeof(mkdirSei);
	mkdirSei.lpVerb = L"runas";
	mkdirSei.lpFile = L"cmd.exe";
	const auto mkdirW = mkdirArgs.toStdWString();
	mkdirSei.lpParameters = mkdirW.c_str();
	mkdirSei.nShow = SW_HIDE;
	mkdirSei.fMask = SEE_MASK_NOCLOSEPROCESS;
	ShellExecuteExW(&mkdirSei);
	if (mkdirSei.hProcess) {
		WaitForSingleObject(mkdirSei.hProcess, 5000);
		CloseHandle(mkdirSei.hProcess);
	}

	SHELLEXECUTEINFOW copySei = {};
	copySei.cbSize = sizeof(copySei);
	copySei.lpVerb = L"runas";
	copySei.lpFile = L"cmd.exe";
	const auto copyW = copyArgs.toStdWString();
	copySei.lpParameters = copyW.c_str();
	copySei.nShow = SW_HIDE;
	copySei.fMask = SEE_MASK_NOCLOSEPROCESS;
	if (ShellExecuteExW(&copySei)) {
		if (copySei.hProcess) {
			WaitForSingleObject(copySei.hProcess, 10000);
			CloseHandle(copySei.hProcess);
		}
		return QFile::exists(dstExe);
	}
#endif // Q_OS_WIN
	return false;
}

void RelaunchFrom(const QString &newExePath) {
	QProcess::startDetached(newExePath, {});
	Core::Quit();
}

void ShowDownloadsDialog() {
	const auto dialog = new QDialog;
	dialog->setWindowTitle(
		tr::lng_first_start_downloads_title(tr::now));
	dialog->setWindowFlags(
		dialog->windowFlags() & ~Qt::WindowContextHelpButtonHint);

	const auto layout = new QVBoxLayout(dialog);
	const auto label = new QLabel(
		tr::lng_first_start_downloads_body(tr::now));
	label->setWordWrap(true);
	label->setMinimumWidth(380);
	layout->addWidget(label);
	layout->addSpacing(12);

#ifdef Q_OS_WIN
	const auto programFilesPath = [] {
		auto pf = qEnvironmentVariable("PROGRAMFILES");
		if (pf.isEmpty()) {
			pf = u"C:\\Program Files"_q;
		}
		return QDir(pf).absolutePath() + u"/Telegram Desktop"_q;
	}();
	const auto installBtn = new QPushButton(
		tr::lng_first_start_move_to_programs(tr::now));
	layout->addWidget(installBtn);
	QObject::connect(installBtn, &QPushButton::clicked, dialog, [=] {
		dialog->hide();
		if (TryCopyBinaryTo(programFilesPath)) {
			Storage::ScheduleSwitchToHome();
			const auto newExe = programFilesPath + '/' + cExeName();
			MarkPromptShown();
			RelaunchFrom(newExe);
		} else {
			dialog->show();
			QMessageBox::warning(
				dialog,
				QString(),
				u"Could not copy Telegram to Program Files. "
				 "Please copy it manually."_q);
		}
	});
#elif defined(Q_OS_MAC) // Q_OS_WIN
	const auto applicationsPath = u"/Applications/Telegram.app"_q;
	const auto moveBtn = new QPushButton(
		tr::lng_first_start_move_to_applications(tr::now));
	layout->addWidget(moveBtn);
	QObject::connect(moveBtn, &QPushButton::clicked, dialog, [=] {
		dialog->accept();
		QMessageBox::information(
			nullptr,
			tr::lng_first_start_move_to_applications(tr::now),
			u"Please drag Telegram from Downloads to your Applications folder."_q);
		MarkPromptShown();
	});
#endif // Q_OS_MAC

	const auto chooseFolderBtn = new QPushButton(
		tr::lng_first_start_choose_folder(tr::now));
	layout->addWidget(chooseFolderBtn);
	QObject::connect(chooseFolderBtn, &QPushButton::clicked, dialog, [=] {
		const auto chosen = QFileDialog::getExistingDirectory(
			dialog,
			tr::lng_first_start_choose_folder(tr::now));
		if (chosen.isEmpty()) {
			return;
		}
		dialog->hide();
		if (TryCopyBinaryTo(chosen)) {
			Storage::ScheduleSwitchToHome();
			const auto newExe = chosen + '/' + cExeName();
			MarkPromptShown();
			RelaunchFrom(newExe);
		} else if (QFile::exists(chosen + '/' + cExeName())) {
			Storage::ScheduleSwitchToHome();
			const auto newExe = chosen + '/' + cExeName();
			MarkPromptShown();
			RelaunchFrom(newExe);
		} else {
			dialog->show();
			QMessageBox::warning(
				dialog,
				QString(),
				u"Could not copy Telegram to the selected folder."_q);
		}
	});

	const auto continueBtn = new QPushButton(
		tr::lng_first_start_continue_downloads(tr::now));
	layout->addWidget(continueBtn);
	QObject::connect(continueBtn, &QPushButton::clicked, dialog, [=] {
		MarkPromptShown();
		dialog->accept();
	});

	[[maybe_unused]] const auto result1 = dialog->exec();
	delete dialog;
}

void ShowRemovableDialog() {
	const auto dialog = new QDialog;
	dialog->setWindowTitle(
		tr::lng_first_start_removable_title(tr::now));
	dialog->setWindowFlags(
		dialog->windowFlags() & ~Qt::WindowContextHelpButtonHint);

	const auto layout = new QVBoxLayout(dialog);
	const auto label = new QLabel(
		tr::lng_first_start_removable_body(tr::now));
	label->setWordWrap(true);
	label->setMinimumWidth(380);
	layout->addWidget(label);
	layout->addSpacing(12);

	const auto portableBtn = new QPushButton(
		tr::lng_first_start_use_portable(tr::now));
	layout->addWidget(portableBtn);
	QObject::connect(portableBtn, &QPushButton::clicked, dialog, [=] {
		if (!Storage::IsCurrentlyPortable()) {
			Storage::ScheduleSwitchToPortable();
		}
		MarkPromptShown();
		dialog->accept();
		if (!Storage::IsCurrentlyPortable()) {
			Core::Restart();
		}
	});

	const auto systemBtn = new QPushButton(
		tr::lng_first_start_use_system(tr::now));
	layout->addWidget(systemBtn);
	QObject::connect(systemBtn, &QPushButton::clicked, dialog, [=] {
		if (!Storage::IsCurrentlyInHome()) {
			Storage::ScheduleSwitchToHome();
		}
		MarkPromptShown();
		dialog->accept();
		if (!Storage::IsCurrentlyInHome()) {
			Core::Restart();
		}
	});

	[[maybe_unused]] const auto result2 = dialog->exec();
	delete dialog;
}

} // namespace

void ShowFirstStartPromptIfNeeded() {
	if (PromptAlreadyShown()) {
		return;
	}
	const auto category = ClassifyBinaryLocation();
	if (category == BinaryLocationCategory::Downloads) {
		ShowDownloadsDialog();
	} else if (category == BinaryLocationCategory::RemovableDrive) {
		ShowRemovableDialog();
	}
}

} // namespace Core
