/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common_session.h"

namespace Settings {

class FakePasscodeAccountSection : public AbstractSection {
public:
	FakePasscodeAccountSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		size_t passcodeIndex,
		int accountIndex);

	[[nodiscard]] static Type MakeId(size_t passcodeIndex, int accountIndex);
	[[nodiscard]] Type id() const override;
	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

	const size_t _passcodeIndex;
	const int _accountIndex;
};

} // namespace Settings
