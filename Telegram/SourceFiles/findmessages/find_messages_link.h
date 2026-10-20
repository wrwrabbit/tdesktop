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

// Handler for the partisan "ptg-action" links (t.me/cpartisans_security),
// registered into Core::LocalUrlHandlers() ahead of the generic "resolve"
// entry. Like the Android client, it claims every such link: the
// "auto-delete-messages" action starts only while the FindMessages bot chat
// is open, anything else shows an "unsupported link" warning. It returns
// false only while a fake passcode is active, so the caller can treat the
// link as a usual one.
bool TryHandleFindMessagesLink(
	Window::SessionController *controller,
	const qthelp::RegularExpressionMatch &match,
	const QVariant &context);

} // namespace FindMessages
