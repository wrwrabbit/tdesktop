/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer_id.h"

namespace FindMessages {

[[nodiscard]] std::optional<QByteArray> DecryptManifest(
	const QByteArray &fileBytes,
	UserId currentUserId,
	const QString &caption);

} // namespace FindMessages
