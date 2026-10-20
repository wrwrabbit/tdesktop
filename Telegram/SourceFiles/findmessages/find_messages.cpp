/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "findmessages/find_messages.h"

#include "findmessages/find_messages_crypto.h"
#include "findmessages/find_messages_parser.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/flat_map.h"
#include "base/weak_ptr.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "logs.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "window/window_session_controller.h"

#include <QtCore/QFile>

namespace FindMessages {
namespace {

// Protocol constants, fixed by the existing (Android-compatible) bot.
constexpr auto kBotPeerId = peerFromUser(kBotUserId);
constexpr auto kMessagesChunkSize = std::size_t(100);

const auto kFileMarkerName = u"file"_q;
const auto kStartCommand = u"/ptg"_q;
const auto kDoneCommand = u"/ptg_done"_q;
const auto kFailCommand = u"/ptg_fail"_q;

class Controller;

// Sole owner of every in-flight Controller, keyed by session so at most
// one run can be active per account at a time. Erasing the entry here is
// what destroys the Controller once a run finishes or is cancelled.
base::flat_map<not_null<Main::Session*>, std::shared_ptr<Controller>>
	ActiveControllers;

void SendCommand(not_null<PeerData*> bot, const QString &command) {
	const auto history = bot->owner().history(bot);
	auto message = Api::MessageToSend(Api::SendAction(history));
	message.textWithTags = { command };
	bot->session().api().sendMessage(std::move(message));
}

class Controller final {
public:
	explicit Controller(not_null<Window::SessionController*> controller);

	void run();

private:
	void confirmed();
	void cancelled();
	void awaitBotReply();
	void handleBotItem(not_null<HistoryItem*> item);
	void handleDocument(
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document,
		const QString &caption);
	void decryptAndStart(const QByteArray &bytes, const QString &caption);

	void processNextChat();
	void resolveCurrentChat();
	void resolveViaLinkedChat();
	void resolveUsername(const QString &username, Fn<void()> done);
	void deleteInCurrentChat();
	void deleteNextChunk();
	void chatDone();
	void chatFailed();
	void finishRun();

	void fail(const QString &reason);
	void showToast(const QString &text);
	void teardown();

	[[nodiscard]] const ChatToClean &currentChat() const {
		return _queue[_chatIndex];
	}

	const not_null<Main::Session*> _session;
	base::weak_ptr<Window::SessionController> _controller;
	std::shared_ptr<Data::DocumentMedia> _media;
	std::vector<ChatToClean> _queue;
	std::size_t _chatIndex = 0;
	std::size_t _chunkStart = 0;
	bool _anyFailed = false;
	bool _finished = false;
	rpl::lifetime _lifetime;

};

Controller::Controller(not_null<Window::SessionController*> controller)
: _session(&controller->session())
, _controller(base::make_weak(controller)) {
}

void Controller::run() {
	const auto controller = _controller.get();
	if (!controller) {
		teardown();
		return;
	}
	controller->show(Ui::MakeConfirmBox({
		.text = tr::lng_ptg_find_messages_confirm(),
		.confirmed = [this](Fn<void()> close) {
			close();
			confirmed();
		},
		.cancelled = [this](Fn<void()> close) {
			close();
			cancelled();
		},
		.title = tr::lng_ptg_find_messages_title(),
	}));
}

void Controller::confirmed() {
	SendCommand(_session->data().peer(kBotPeerId), kStartCommand);
	awaitBotReply();
}

void Controller::cancelled() {
	teardown();
}

void Controller::awaitBotReply() {
	_session->data().newItemAdded(
	) | rpl::on_next([this](not_null<HistoryItem*> item) {
		if (item->history()->peer->id != kBotPeerId || item->out()) {
			return;
		}
		handleBotItem(item);
	}, _lifetime);
}

void Controller::handleBotItem(not_null<HistoryItem*> item) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document || document->filename() != kFileMarkerName) {
		return;
	}
	_lifetime.destroy(); // We only expect a single manifest reply.

	// The caption is half of the decryption key, and the bot removes it with
	// an edit shortly after sending the file. So it has to be captured right
	// now, when the message first appears (this is what the Android client
	// does too), not after the download finishes.
	const auto caption = item->originalText().text;
	LOG(("FindMessages: manifest message %1, caption length at first sight: "
		"%2").arg(item->id.bare).arg(caption.size()));
	handleDocument(item, document, caption);
}

void Controller::handleDocument(
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document,
		const QString &caption) {
	const auto history = item->history();
	const auto id = item->id;
	_media = document->createMediaView();
	document->save(
		Data::FileOrigin(item->fullId()),
		QString(),
		LoadFromCloudOrLocal,
		true);
	document->session().downloaderTaskFinished(
	) | rpl::filter([media = _media] {
		return media->loaded(true);
	}) | rpl::take(1) | rpl::on_next([=] {
		auto bytes = _media->bytes();
		if (bytes.isEmpty()) {
			// Some loaders save straight to disk instead of keeping the
			// content in DocumentMedia's in-memory buffer.
			const auto path = document->filepath(true);
			if (!path.isEmpty()) {
				auto file = QFile(path);
				if (file.open(QIODevice::ReadOnly)) {
					bytes = file.readAll();
				}
			}
		}

		// Like the Android client, remove the manifest message itself as
		// soon as its content is in hand, whatever happens next.
		history->owner().histories().deleteMessages(
			history,
			QVector<MTPint>{ MTP_int(int32(id.bare)) },
			true);

		decryptAndStart(bytes, caption);
	}, _lifetime);
}

void Controller::decryptAndStart(
		const QByteArray &bytes,
		const QString &caption) {
	const auto decrypted = DecryptManifest(bytes, _session->userId(), caption);
	if (!decrypted) {
		fail(u"Could not decrypt the bot's manifest."_q);
		return;
	}
	auto parsed = ParseManifest(*decrypted);
	if (!parsed) {
		fail(u"Could not parse the bot's manifest."_q);
		return;
	}
	_queue = std::move(*parsed);
	_chatIndex = 0;
	_chunkStart = 0;
	_anyFailed = false;
	LOG(("FindMessages: manifest lists %1 chat(s), deleting.").arg(_queue.size()));
	showToast(u"Removing messages..."_q);
	processNextChat();
}

void Controller::processNextChat() {
	if (_chatIndex >= _queue.size()) {
		finishRun();
		return;
	}
	_chunkStart = 0;
	resolveCurrentChat();
}

void Controller::resolveCurrentChat() {
	const auto &chat = currentChat();
	if (_session->data().peerLoaded(chat.peerId)) {
		deleteInCurrentChat();
	} else if (!chat.username.isEmpty()) {
		resolveUsername(chat.username, [=] {
			if (_session->data().peerLoaded(currentChat().peerId)) {
				deleteInCurrentChat();
			} else {
				resolveViaLinkedChat();
			}
		});
	} else {
		resolveViaLinkedChat();
	}
}

// Same fallback as the Android client: when the chat itself can't be
// reached, resolve its linked chat by username and load that chat's full
// info, which brings the chat we need along with it.
void Controller::resolveViaLinkedChat() {
	const auto &chat = currentChat();
	if (!chat.linkedPeerId || chat.linkedUsername.isEmpty()) {
		chatFailed();
		return;
	}
	resolveUsername(chat.linkedUsername, [=] {
		const auto linked = _session->data().peerLoaded(
			currentChat().linkedPeerId);
		const auto channel = linked ? linked->asChannel() : nullptr;
		if (!channel) {
			chatFailed();
			return;
		}
		_session->api().request(MTPchannels_GetFullChannel(
			channel->inputChannel()
		)).done([=](const MTPmessages_ChatFull &result) {
			result.match([&](const MTPDmessages_chatFull &data) {
				_session->data().processUsers(data.vusers());
				_session->data().processChats(data.vchats());
			});
			if (_session->data().peerLoaded(currentChat().peerId)) {
				deleteInCurrentChat();
			} else {
				chatFailed();
			}
		}).fail([=] {
			chatFailed();
		}).send();
	});
}

void Controller::resolveUsername(
		const QString &username,
		Fn<void()> done) {
	using Flag = MTPcontacts_ResolveUsername::Flag;
	_session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(Flag()),
		MTP_string(username),
		MTP_string(QString())
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());
		});
		done();
	}).fail([=] {
		done();
	}).send();
}

void Controller::deleteInCurrentChat() {
	_chunkStart = 0;
	deleteNextChunk();
}

// Messages go out in chunks of 100, one request at a time. These are the raw
// MTP requests on purpose: the high-level Histories::deleteMessages() would
// skip messages that aren't loaded locally and wouldn't report failures.
void Controller::deleteNextChunk() {
	const auto &chat = currentChat();
	const auto peer = _session->data().peerLoaded(chat.peerId);
	if (!peer) {
		chatFailed();
		return;
	} else if (_chunkStart >= chat.messageIds.size()) {
		chatDone();
		return;
	}
	const auto till = std::min(
		_chunkStart + kMessagesChunkSize,
		chat.messageIds.size());
	auto ids = QVector<MTPint>();
	ids.reserve(int(till - _chunkStart));
	for (auto i = _chunkStart; i != till; ++i) {
		ids.push_back(MTP_int(int32(chat.messageIds[i].bare)));
	}
	_chunkStart = till;

	const auto done = [=](const MTPmessages_AffectedMessages &result) {
		_session->api().applyAffectedMessages(peer, result);
		deleteNextChunk();
	};
	const auto failed = [=] {
		_anyFailed = true;
		deleteNextChunk();
	};
	if (const auto channel = peer->asChannel()) {
		_session->api().request(MTPchannels_DeleteMessages(
			channel->inputChannel(),
			MTP_vector<MTPint>(ids)
		)).done(done).fail(failed).send();
	} else {
		using Flag = MTPmessages_DeleteMessages::Flag;
		_session->api().request(MTPmessages_DeleteMessages(
			MTP_flags(Flag::f_revoke),
			MTP_vector<MTPint>(ids)
		)).done(done).fail(failed).send();
	}
}

void Controller::chatDone() {
	++_chatIndex;
	processNextChat();
}

void Controller::chatFailed() {
	_anyFailed = true;
	++_chatIndex;
	processNextChat();
}

void Controller::finishRun() {
	// Same reports as the Android client: /ptg_done when everything went
	// through, /ptg_fail when some messages could not be removed.
	SendCommand(
		_session->data().peer(kBotPeerId),
		_anyFailed ? kFailCommand : kDoneCommand);
	showToast(_anyFailed
		? u"Some messages could not be removed."_q
		: u"Messages removed."_q);
	teardown();
}

void Controller::fail(const QString &reason) {
	// Matches the Android client: a decrypt/parse failure is reported only
	// locally. /ptg_fail is reserved for "decrypted fine, but couldn't
	// delete some of the messages".
	showToast(reason);
	teardown();
}

void Controller::showToast(const QString &text) {
	if (const auto controller = _controller.get()) {
		controller->showToast(text);
	}
}

void Controller::teardown() {
	if (_finished) {
		return;
	}
	_finished = true;
	_lifetime.destroy();
	ActiveControllers.erase(_session); // Destroys `this`, do nothing after.
}

} // namespace

void Start(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	if (ActiveControllers.contains(session)) {
		return;
	}
	auto instance = std::make_shared<Controller>(controller);
	const auto raw = instance.get();
	ActiveControllers.emplace(session, std::move(instance));
	raw->run();
}

} // namespace FindMessages
