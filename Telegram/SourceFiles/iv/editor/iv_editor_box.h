/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>

#include <memory>

class PeerData;

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Iv::Editor {

class State;
class Widget;

struct ShowBoxDescriptor {
	enum class SubmitType {
		Send,
		Save,
	};

	not_null<Window::SessionController*> controller;
	not_null<PeerData*> peer;
	std::shared_ptr<State> state;
	QString submitLabel;
	SubmitType submitType = SubmitType::Send;
	Fn<bool()> cancelled;
	Fn<bool()> confirmed;
	Fn<void(not_null<Ui::RpWidget*>)> setupSubmitButton;
	Fn<void(not_null<Widget*>)> requestMedia;
	Fn<void(not_null<Widget*>)> requestMap;
};

void ShowBox(ShowBoxDescriptor descriptor);
void ShowBox(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);

} // namespace Iv::Editor
