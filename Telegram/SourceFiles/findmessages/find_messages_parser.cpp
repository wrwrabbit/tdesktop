/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "findmessages/find_messages_parser.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

namespace FindMessages {
namespace {

constexpr auto kChannelIdOffset = int64(1'000'000'000'000);

[[nodiscard]] QString StringOrEmpty(
		const QJsonObject &object,
		const QString &key) {
	const auto value = object.value(key);
	return value.isString() ? value.toString() : QString();
}

} // namespace

PeerId PeerIdFromBotApiChatId(int64 rawId) {
	const auto magnitude = std::abs(rawId);
	return (magnitude >= kChannelIdOffset)
		? peerFromChannel(ChannelId(BareId(magnitude - kChannelIdOffset)))
		: peerFromChat(ChatId(BareId(magnitude)));
}

std::optional<std::vector<ChatToClean>> ParseManifest(
		const QByteArray &json) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(json, &error);
	if (error.error != QJsonParseError::NoError || !document.isArray()) {
		return std::nullopt;
	}

	auto result = std::vector<ChatToClean>();
	for (const auto &entry : document.array()) {
		if (!entry.isObject()) {
			return std::nullopt;
		}
		const auto object = entry.toObject();
		const auto chatIdValue = object.value(u"chat_id"_q);
		const auto messageIdsValue = object.value(u"message_ids"_q);
		if (!chatIdValue.isDouble() || !messageIdsValue.isArray()) {
			return std::nullopt;
		}

		auto chat = ChatToClean();
		// QJsonDocument stores numbers as double; safe here since real
		// chat-id magnitudes stay far below 2^53.
		chat.peerId = PeerIdFromBotApiChatId(int64(chatIdValue.toDouble()));
		chat.username = StringOrEmpty(object, u"chat_username"_q);

		const auto linkedIdValue = object.value(u"linked_chat_id"_q);
		if (linkedIdValue.isDouble()) {
			chat.linkedPeerId = PeerIdFromBotApiChatId(
				int64(linkedIdValue.toDouble()));
		}
		chat.linkedUsername = StringOrEmpty(object, u"linked_chat_username"_q);

		const auto ids = messageIdsValue.toArray();
		chat.messageIds.reserve(ids.size());
		for (const auto &id : ids) {
			if (!id.isDouble()) {
				return std::nullopt;
			}
			chat.messageIds.push_back(MsgId(int64(id.toDouble())));
		}

		if (!chat.peerId || chat.messageIds.empty()) {
			continue;
		}
		result.push_back(std::move(chat));
	}
	return result;
}

} // namespace FindMessages
