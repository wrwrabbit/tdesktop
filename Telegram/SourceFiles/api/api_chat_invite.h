/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class ChannelData;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Api {

void CheckChatInvite(
	not_null<Window::SessionController*> controller,
	const QString &hash,
	ChannelData *invitePeekChannel = nullptr,
	Fn<void()> loaded = nullptr);

} // namespace Api
