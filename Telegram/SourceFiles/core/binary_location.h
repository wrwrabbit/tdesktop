/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Core {

enum class BinaryLocationCategory {
	Downloads,
	SystemAppFolder,
	RemovableDrive,
	CustomUser,
};

[[nodiscard]] BinaryLocationCategory ClassifyBinaryLocation();
[[nodiscard]] bool BinaryIsInDownloads();
[[nodiscard]] bool BinaryIsInSystemAppFolder();
[[nodiscard]] bool BinaryIsOnRemovableDrive();

[[nodiscard]] QString SystemAppFolderPath();
[[nodiscard]] QString DownloadsFolderPath();

} // namespace Core
