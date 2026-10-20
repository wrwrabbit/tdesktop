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
	std::vector<PeerId> peerIds;
	QString username;
	std::vector<PeerId> linkedPeerIds;
	QString linkedUsername;
	std::vector<MsgId> messageIds;
};

// The bot sends bare chat ids with no type information (Android just
// negates them to get its dialog id), so a single id can stand for either
// a channel or a basic group. Candidates come in the order they should be
// tried: channel first, as the bot works with public chats. An id carrying
// the Bot API "-100" prefix is unambiguously a channel.
[[nodiscard]] std::vector<PeerId> PeerIdCandidatesFromChatId(int64 rawId);

[[nodiscard]] std::optional<std::vector<ChatToClean>> ParseManifest(
	const QByteArray &json);

} // namespace FindMessages
