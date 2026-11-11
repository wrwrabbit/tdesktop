/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "findmessages/find_messages_crypto.h"

#include "base/openssl_help.h"

#include <algorithm>

namespace FindMessages {
namespace {

constexpr auto kIvSize = 16;
constexpr auto kKeySize = 32;
constexpr auto kAesBlockSize = 16;

} // namespace

std::optional<QByteArray> DecryptManifest(
		const QByteArray &fileBytes,
		UserId currentUserId,
		const QString &caption) {
	if (fileBytes.size() <= kIvSize
		|| ((fileBytes.size() - kIvSize) % kAesBlockSize) != 0) {
		return std::nullopt;
	}

	const auto key = openssl::Sha256(
		QByteArray::number(qint64(currentUserId.bare)),
		caption.toUtf8());

	auto iv = bytes::make_vector(
		bytes::make_span(fileBytes).subspan(0, kIvSize));
	const auto cipher = bytes::make_span(fileBytes).subspan(kIvSize);

	auto aesKey = AES_KEY();
	if (AES_set_decrypt_key(
			reinterpret_cast<const uchar*>(key.data()),
			kKeySize * CHAR_BIT,
			&aesKey) != 0) {
		return std::nullopt;
	}

	auto plain = bytes::vector(cipher.size());
	AES_cbc_encrypt(
		reinterpret_cast<const uchar*>(cipher.data()),
		reinterpret_cast<uchar*>(plain.data()),
		cipher.size(),
		&aesKey,
		reinterpret_cast<uchar*>(iv.data()),
		AES_DECRYPT);

	const auto raw = reinterpret_cast<const uchar*>(plain.data());
	auto size = int(plain.size());
	const auto padLength = int(raw[size - 1]);
	if (padLength < 1 || padLength > kAesBlockSize || padLength > size) {
		return std::nullopt;
	}
	for (auto i = 0; i != padLength; ++i) {
		if (raw[size - 1 - i] != padLength) {
			return std::nullopt;
		}
	}
	size -= padLength;
	const auto zero = std::find(raw, raw + size, uchar(0));
	return QByteArray(reinterpret_cast<const char*>(raw), int(zero - raw));
}

} // namespace FindMessages
