/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer_id.h"

namespace Window {
class SessionController;
} // namespace Window

namespace FindMessages {

inline constexpr auto kBotUserId = UserId(6092224989);

void Start(not_null<Window::SessionController*> controller);

} // namespace FindMessages
