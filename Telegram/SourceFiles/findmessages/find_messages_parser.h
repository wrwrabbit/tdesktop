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

struct ChatToClean {
	std::vector<PeerId> peerIds;
	QString username;
	std::vector<PeerId> linkedPeerIds;
	QString linkedUsername;
	std::vector<MsgId> messageIds;
};

[[nodiscard]] std::vector<PeerId> PeerIdCandidatesFromChatId(int64 rawId);

[[nodiscard]] std::optional<std::vector<ChatToClean>> ParseManifest(
	const QByteArray &json);

} // namespace FindMessages
