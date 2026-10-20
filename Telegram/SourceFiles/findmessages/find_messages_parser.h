/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer_id.h"
#include "data/data_msg_id.h"

namespace FindMessages {

// A single chat mentioned in the bot's manifest: messages to delete there,
// plus enough info to resolve the chat if it isn't loaded locally yet.
struct ChatToClean {
	PeerId peerId = 0;
	QString username;
	PeerId linkedPeerId = 0;
	QString linkedUsername;
	std::vector<MsgId> messageIds;
};

// The bot encodes chat ids using the classic Bot API convention:
// supergroups/channels as -100<id>, basic groups as -<id>. Only the
// magnitude matters for recovering the real bare id.
[[nodiscard]] PeerId PeerIdFromBotApiChatId(int64 rawId);

[[nodiscard]] std::optional<std::vector<ChatToClean>> ParseManifest(
	const QByteArray &json);

} // namespace FindMessages
