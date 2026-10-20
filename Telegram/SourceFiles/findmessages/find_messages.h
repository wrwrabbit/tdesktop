/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace FindMessages {

// Entry point for the "auto-delete-messages" partisan link: shows a
// confirmation, then talks to the FindMessages bot to fetch and delete
// the user's messages across chats. Mirrors Partisan Telegram Android's
// FindMessagesController for protocol compatibility with the real bot.
void Start(not_null<Window::SessionController*> controller);

} // namespace FindMessages
