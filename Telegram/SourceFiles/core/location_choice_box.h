/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Ui {
class Show;
} // namespace Ui

namespace Core {

[[nodiscard]] bool TryCopyBinary(const QString &targetDir);
void CopyCompanionFiles(const QString &targetDir);
void RelaunchFrom(const QString &newExePath);
[[nodiscard]] QString RelaunchExePath(const QString &targetDir);

void FillLocationChoiceBox(not_null<Ui::GenericBox*> box);
void ShowLocationChoiceBox(not_null<Ui::Show*> show);
void ShowLocationChoiceBoxFirstRun(not_null<Ui::Show*> show);

} // namespace Core
