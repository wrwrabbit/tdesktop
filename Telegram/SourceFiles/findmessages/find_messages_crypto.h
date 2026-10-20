/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer_id.h"

namespace FindMessages {

// File layout produced by the bot: 16-byte IV followed by an
// AES-256-CBC ciphertext. Key = SHA256(currentUserId + caption), where
// caption is the text that came with the same message as the file.
// After decryption the real payload ends at the first zero byte.
[[nodiscard]] std::optional<QByteArray> DecryptManifest(
	const QByteArray &fileBytes,
	UserId currentUserId,
	const QString &caption);

} // namespace FindMessages
