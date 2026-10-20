/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "findmessages/find_messages_link.h"

#include "findmessages/find_messages.h"
#include "window/window_session_controller.h"

namespace FindMessages {

bool TryHandleFindMessagesLink(
		Window::SessionController *controller,
		const qthelp::RegularExpressionMatch &match,
		const QVariant &context) {
	if (!controller) {
		return false;
	}
	Start(controller);
	return true;
}

} // namespace FindMessages
