/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Storage {

enum class TdataLocation { Home, Portable, Custom };

[[nodiscard]] QString SwitchFlagFilePath();

[[nodiscard]] bool IsCurrentlyPortable();
[[nodiscard]] bool IsCurrentlyInHome();
[[nodiscard]] TdataLocation CurrentTdataLocation();

[[nodiscard]] bool CanSwitchToPortable();
[[nodiscard]] bool CanSwitchToHome();

bool ScheduleSwitchToPortable();
bool ScheduleSwitchToHome();
bool ScheduleSwitchToHomeWrittenTo(const QString &newExeDir);
bool ScheduleSwitchToCustom(const QString &targetDir);

bool ApplyPendingSwitch();

} // namespace Storage
