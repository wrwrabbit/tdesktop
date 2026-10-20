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

// Handler for the "auto-delete-messages" partisan link, registered into
// Core::LocalUrlHandlers() ahead of the generic "resolve" entry.
bool TryHandleFindMessagesLink(
	Window::SessionController *controller,
	const qthelp::RegularExpressionMatch &match,
	const QVariant &context);

} // namespace FindMessages
