/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Storage {

[[nodiscard]] QString SwitchFlagFilePath();

[[nodiscard]] bool IsCurrentlyPortable();
[[nodiscard]] bool IsCurrentlyInHome();

[[nodiscard]] bool CanSwitchToPortable();
[[nodiscard]] bool CanSwitchToHome();

bool ScheduleSwitchToPortable();
bool ScheduleSwitchToHome();
bool ScheduleSwitchToCustom(const QString &targetDir);

bool ApplyPendingSwitch();

} // namespace Storage
