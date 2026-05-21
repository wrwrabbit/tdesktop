/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common_session.h"

namespace Settings {

class FakePasscodeSection : public AbstractSection {
public:
	FakePasscodeSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		size_t passcodeIndex);

	[[nodiscard]] static Type MakeId(size_t passcodeIndex);
	[[nodiscard]] Type id() const override;
	[[nodiscard]] rpl::producer<QString> title() override;
	[[nodiscard]] rpl::producer<> sectionShowBack() override;
	void showFinished() override;

private:
	void setupContent();

	const size_t _passcodeIndex;
	rpl::event_stream<> _showBackRequests;
};

} // namespace Settings
