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
#include "logs.h"

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

[[nodiscard]] bool ConfirmReplaceIfNeeded(QWidget *parent, const QString &targetDir) {
	const auto dstExe = targetDir + '/' + cExeName();
	if (!QFile::exists(dstExe)) {
		LOG(("FirstStart: no existing binary at '%1', skipping replace confirm").arg(dstExe));
		return true;
	}
	LOG(("FirstStart: existing binary found at '%1', asking user").arg(dstExe));
	const auto result = QMessageBox::question(
		parent,
		tr::lng_install_existing_title(tr::now),
		tr::lng_install_existing_body(
			tr::now,
			lt_path,
			QDir::toNativeSeparators(targetDir)),
		QMessageBox::Yes | QMessageBox::No,
		QMessageBox::No);
	LOG(("FirstStart: user chose to replace: %1").arg(Logs::b(result == QMessageBox::Yes)));
	return result == QMessageBox::Yes;
}

bool TryCopyBinaryTo(const QString &targetDir) {
	const auto srcExe = cExeDir() + cExeName();
	const auto dstExe = targetDir + '/' + cExeName();
	LOG(("FirstStart: TryCopyBinaryTo src='%1' dst='%2'").arg(srcExe, dstExe));
	const auto removedOld = QFile::remove(dstExe);
	LOG(("FirstStart: removed old binary: %1").arg(Logs::b(removedOld)));
	const auto madeDir = QDir().mkpath(targetDir);
	LOG(("FirstStart: mkpath('%1'): %2").arg(targetDir, Logs::b(madeDir)));
	if (madeDir) {
		const auto copied = QFile::copy(srcExe, dstExe);
		LOG(("FirstStart: QFile::copy result: %1, dst exists: %2").arg(
			Logs::b(copied),
			Logs::b(QFile::exists(dstExe))));
		if (copied) {
			return true;
		}
	}
#ifdef Q_OS_WIN
	const auto srcNative = QDir::toNativeSeparators(srcExe);
	const auto dstNative = QDir::toNativeSeparators(dstExe);

	const auto combinedArgs = u"/c mkdir \"%1\" 2>nul & copy /Y \"%2\" \"%3\""_q
		.arg(
			QDir::toNativeSeparators(targetDir),
			srcNative,
			dstNative);

	SHELLEXECUTEINFOW sei = {};
	sei.cbSize = sizeof(sei);
	sei.lpVerb = L"runas";
	sei.lpFile = L"cmd.exe";
	const auto combinedW = combinedArgs.toStdWString();
	sei.lpParameters = combinedW.c_str();
	sei.nShow = SW_HIDE;
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	LOG(("FirstStart: launching elevated cmd: %1").arg(combinedArgs));
	const auto launched = ShellExecuteExW(&sei);
	LOG(("FirstStart: ShellExecuteExW launched: %1, hProcess: %2").arg(
		Logs::b(launched != 0),
		Logs::b(sei.hProcess != nullptr)));
	if (launched) {
		if (sei.hProcess) {
			const auto waitResult = WaitForSingleObject(sei.hProcess, 15000);
			LOG(("FirstStart: WaitForSingleObject result: %1").arg(waitResult));
			CloseHandle(sei.hProcess);
		}
		const auto dstExists = QFile::exists(dstExe);
		LOG(("FirstStart: elevated copy done, dst exists: %1").arg(Logs::b(dstExists)));
		return dstExists;
	}
	LOG(("FirstStart: ShellExecuteExW failed, GetLastError: %1").arg(GetLastError()));
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
		LOG(("FirstStart: user clicked 'Install to Program Files', target='%1'").arg(programFilesPath));
		dialog->hide();
		if (!ConfirmReplaceIfNeeded(nullptr, programFilesPath)) {
			LOG(("FirstStart: user cancelled replace, back to dialog"));
			dialog->show();
			return;
		}
		if (TryCopyBinaryTo(programFilesPath)) {
			LOG(("FirstStart: copy succeeded, scheduling switch and relaunching"));
			Storage::ScheduleSwitchToHomeWrittenTo(programFilesPath);
			const auto newExe = programFilesPath + '/' + cExeName();
			MarkPromptShown();
			RelaunchFrom(newExe);
		} else {
			LOG(("FirstStart: copy failed for Program Files"));
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
			LOG(("FirstStart: choose folder cancelled"));
			return;
		}
		LOG(("FirstStart: user chose folder '%1'").arg(chosen));
		dialog->hide();
		if (!ConfirmReplaceIfNeeded(nullptr, chosen)) {
			LOG(("FirstStart: user cancelled replace in chosen folder, back to dialog"));
			dialog->show();
			return;
		}
		if (TryCopyBinaryTo(chosen)) {
			LOG(("FirstStart: copy to chosen folder succeeded"));
			Storage::ScheduleSwitchToHomeWrittenTo(chosen);
			const auto newExe = chosen + '/' + cExeName();
			MarkPromptShown();
			RelaunchFrom(newExe);
		} else if (QFile::exists(chosen + '/' + cExeName())) {
			LOG(("FirstStart: copy failed but binary already exists in chosen folder, relaunching"));
			Storage::ScheduleSwitchToHomeWrittenTo(chosen);
			const auto newExe = chosen + '/' + cExeName();
			MarkPromptShown();
			RelaunchFrom(newExe);
		} else {
			LOG(("FirstStart: copy to chosen folder failed, binary does not exist there either"));
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
		LOG(("FirstStart: prompt already shown ('%1' exists), skipping").arg(PromptMarkerPath()));
		return;
	}
	const auto category = ClassifyBinaryLocation();
	LOG(("FirstStart: binary location category: %1").arg(static_cast<int>(category)));
	if (category == BinaryLocationCategory::Downloads) {
		ShowDownloadsDialog();
	} else if (category == BinaryLocationCategory::RemovableDrive) {
		ShowRemovableDialog();
	}
}

} // namespace Core
