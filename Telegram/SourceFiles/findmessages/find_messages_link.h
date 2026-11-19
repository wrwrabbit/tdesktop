/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace qthelp {
class RegularExpressionMatch;
} // namespace qthelp

namespace Window {
class SessionController;
} // namespace Window

namespace FindMessages {

bool TryHandleFindMessagesLink(
	Window::SessionController *controller,
	const qthelp::RegularExpressionMatch &match,
	const QVariant &context);

} // namespace FindMessages
