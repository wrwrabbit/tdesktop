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
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "base/weak_qptr.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_histories.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "logs.h"
#include "main/main_session.h"
#include "mtproto/facade.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/radial_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "window/window_session_controller.h"
#include "styles/style_widgets.h"

namespace FindMessages {
namespace {

constexpr auto kBotPeerId = peerFromUser(kBotUserId);
constexpr auto kMessagesChunkSize = std::size_t(100);
constexpr auto kManifestPartSize = 32 * 1024;

constexpr auto kFinalCommandWait = crl::time(10 * 1000);

constexpr auto kBotReplyWait = crl::time(60 * 1000);

constexpr auto kStepWait = crl::time(60 * 1000);

const auto kFileMarkerName = u"file"_q;
const auto kStartCommand = u"/ptg"_q;
const auto kDoneCommand = u"/ptg_done"_q;
const auto kFailCommand = u"/ptg_fail"_q;

class Controller;

class SpinnerWidget final : public Ui::RpWidget {
public:
	explicit SpinnerWidget(QWidget *parent)
	: RpWidget(parent)
	, _animation([=] { update(); }, st::defaultInfiniteRadialAnimation) {
		resize(
			width(),
			style::ConvertScale(kSpinnerSize + 2 * kSpinnerSkip));
		_animation.start();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		const auto thickness = style::ConvertScale(3);
		const auto size = style::ConvertScale(kSpinnerSize) - 2 * thickness;
		Ui::InfiniteRadialAnimation::Draw(
			p,
			_animation.computeState(),
			QPoint(
				(width() - size) / 2,
				style::ConvertScale(kSpinnerSkip) + thickness),
			QSize(size, size),
			width(),
			QPen(st::windowActiveTextFg->c),
			thickness);
	}

private:
	static constexpr auto kSpinnerSize = 40;
	static constexpr auto kSpinnerSkip = 16;

	Ui::InfiniteRadialAnimation _animation;

};

[[nodiscard]] PeerData *LoadedPeer(
		not_null<Main::Session*> session,
		const std::vector<PeerId> &candidates) {
	for (const auto id : candidates) {
		if (const auto peer = session->data().peerLoaded(id)) {
			return peer;
		}
	}
	return nullptr;
}

base::flat_map<not_null<Main::Session*>, std::shared_ptr<Controller>>
	ActiveControllers;

void SendCommand(not_null<PeerData*> bot, const QString &command) {
	const auto history = bot->owner().history(bot);
	auto message = Api::MessageToSend(Api::SendAction(history));
	message.textWithTags = { command };
	bot->session().api().sendMessage(std::move(message));
}

void RemoveLoadedMessages(
		not_null<PeerData*> peer,
		const std::vector<MsgId> &ids) {
	auto &owner = peer->owner();
	auto remove = std::vector<not_null<HistoryItem*>>();
	remove.reserve(ids.size());
	for (const auto id : ids) {
		if (const auto item = owner.message(peer, id)) {
			remove.push_back(item);
		}
	}
	if (remove.empty()) {
		return;
	}
	owner.notifyItemsAboutToBeDestroyed(remove);
	for (const auto &item : remove) {
		const auto history = item->history();
		const auto wasLast = (history->lastMessage() == item);
		const auto wasInChats = (history->chatListMessage() == item);
		item->destroy();
		if (wasLast || wasInChats) {
			history->requestChatListMessage();
		}
	}
}

class Controller final : public base::has_weak_ptr {
public:
	explicit Controller(not_null<Window::SessionController*> controller);

	void run();

private:
	void confirmed();
	void cancelled();
	void hideCommands();
	void awaitBotReply();
	void handleBotItem(not_null<HistoryItem*> item);
	void handleDocument(
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document,
		const QString &caption);
	void requestManifestPart();
	void removeManifestMessage();
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

	void watch();
	void stalled();
	void fail(const QString &reason);
	void showProgress();
	void lockProgress();
	void hideProgress();
	void teardown();

	[[nodiscard]] const ChatToClean &currentChat() const {
		return _queue[_chatIndex];
	}

	const not_null<Main::Session*> _session;
	base::weak_ptr<Window::SessionController> _controller;
	base::weak_qptr<Ui::BoxContent> _progressBox;
	FullMsgId _manifestId;
	QString _manifestCaption;
	MTPInputFileLocation _manifestLocation;
	QByteArray _manifest;
	int64 _manifestSize = 0;
	std::vector<ChatToClean> _queue;
	std::size_t _chatIndex = 0;
	std::size_t _chunkStart = 0;
	bool _anyFailed = false;
	bool _finished = false;
	bool _finishing = false;
	bool _progressLocked = false;
	bool _progressHiding = false;
	base::Timer _replyTimer;
	base::Timer _stepTimer;
	base::Timer _finalCommandTimer;
	rpl::lifetime _progressLifetime;
	rpl::lifetime _commandsLifetime;
	rpl::lifetime _lifetime;

};

Controller::Controller(not_null<Window::SessionController*> controller)
: _session(&controller->session())
, _controller(base::make_weak(controller)) {
	_stepTimer.setCallback([=] { stalled(); });
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
		.confirmText = tr::lng_continue(),
		.title = tr::lng_ptg_find_messages_title(),
	}));
}

void Controller::confirmed() {
	showProgress();
	hideCommands();
	SendCommand(_session->data().peer(kBotPeerId), kStartCommand);
	awaitBotReply();

	_replyTimer.setCallback([=] {
		fail(u"The bot did not send a manifest in time."_q);
	});
	_replyTimer.callOnce(kBotReplyWait);
}

void Controller::hideCommands() {
	const auto hide = [session = _session](FullMsgId id) {
		crl::on_main(session.get(), [=] {
			if (const auto item = session->data().message(id)) {
				item->destroy();
			}
		});
	};
	const auto isCommand = [](not_null<HistoryItem*> item) {
		return !item->media() && item->originalText().text.startsWith('/');
	};

	_session->data().itemIdChanged(
	) | rpl::filter([](const Data::Session::IdChange &change) {
		return change.newId.peer == kBotPeerId;
	}) | rpl::on_next([=](const Data::Session::IdChange &change) {
		const auto item = _session->data().message(change.newId);
		if (!item || !item->out() || !isCommand(item)) {
			return;
		}
		const auto text = item->originalText().text;
		const auto isFinal = (text == kDoneCommand) || (text == kFailCommand);
		hide(change.newId);
		if (isFinal) {
			teardown();
		}
	}, _commandsLifetime);

	_session->data().newItemAdded(
	) | rpl::filter([=](not_null<HistoryItem*> item) {
		return (item->history()->peer->id == kBotPeerId)
			&& !item->out()
			&& isCommand(item);
	}) | rpl::on_next([=](not_null<HistoryItem*> item) {
		hide(item->fullId());
	}, _commandsLifetime);

	_session->data().itemDataChanges(
	) | rpl::filter([=](not_null<HistoryItem*> item) {
		return (item->history()->peer->id == kBotPeerId)
			&& item->out()
			&& item->hasFailed()
			&& isCommand(item)
			&& (item->originalText().text == kStartCommand);
	}) | rpl::on_next([=](not_null<HistoryItem*> item) {
		hide(item->fullId());
		fail(u"The start command could not be sent."_q);
	}, _commandsLifetime);
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
	_lifetime.destroy();
	_replyTimer.cancel();
	lockProgress();

	const auto caption = item->originalText().text;
	LOG(("FindMessages: manifest message %1, caption length at first sight: "
		"%2").arg(item->id.bare).arg(caption.size()));
	handleDocument(item, document, caption);
}

void Controller::handleDocument(
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document,
		const QString &caption) {
	_manifestId = item->fullId();
	_manifestCaption = caption;
	_manifestSize = document->size;
	_manifest = QByteArray();

	auto accessHash = std::optional<MTPlong>();
	document->mtpInput().match([&](const MTPDinputDocument &data) {
		accessHash = data.vaccess_hash();
	}, [](const auto &) {
	});
	if (!accessHash || _manifestSize <= 0) {
		removeManifestMessage();
		fail(u"Could not locate the bot's manifest."_q);
		return;
	}
	_manifestLocation = MTP_inputDocumentFileLocation(
		MTP_long(document->id),
		*accessHash,
		MTP_bytes(document->fileReference()),
		MTP_string());
	requestManifestPart();
}

void Controller::requestManifestPart() {
	watch();
	_session->api().request(MTPupload_GetFile(
		MTP_flags(0),
		_manifestLocation,
		MTP_long(_manifest.size()),
		MTP_int(kManifestPartSize)
	)).done(crl::guard(this, [=](const MTPupload_File &result) {
		const auto part = result.match([&](const MTPDupload_file &data) {
			return std::make_optional(data.vbytes().v);
		}, [](const MTPDupload_fileCdnRedirect &) {
			return std::optional<QByteArray>();
		});
		if (!part || part->isEmpty()) {
			removeManifestMessage();
			fail(u"Could not download the bot's manifest."_q);
			return;
		}
		_manifest.append(*part);
		if (_manifest.size() < _manifestSize) {
			requestManifestPart();
			return;
		}
		removeManifestMessage();
		decryptAndStart(_manifest, _manifestCaption);
	})).fail(crl::guard(this, [=](const MTP::Error &) {
		removeManifestMessage();
		fail(u"Could not download the bot's manifest."_q);
	})).toDC(MTP::downloadDcId(_session->mainDcId(), 0)).send();
}

void Controller::removeManifestMessage() {
	_session->data().histories().deleteMessages({ _manifestId }, true);
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
	processNextChat();
}

void Controller::processNextChat() {
	if (_finishing) {
		return;
	}
	if (_chatIndex >= _queue.size()) {
		finishRun();
		return;
	}
	_chunkStart = 0;
	resolveCurrentChat();
}

void Controller::resolveCurrentChat() {
	const auto &chat = currentChat();
	if (LoadedPeer(_session, chat.peerIds)) {
		deleteInCurrentChat();
	} else if (!chat.username.isEmpty()) {
		resolveUsername(chat.username, [=] {
			if (LoadedPeer(_session, currentChat().peerIds)) {
				deleteInCurrentChat();
			} else {
				resolveViaLinkedChat();
			}
		});
	} else {
		resolveViaLinkedChat();
	}
}

void Controller::resolveViaLinkedChat() {
	const auto &chat = currentChat();
	if (chat.linkedPeerIds.empty() || chat.linkedUsername.isEmpty()) {
		chatFailed();
		return;
	}
	resolveUsername(chat.linkedUsername, [=] {
		const auto linked = LoadedPeer(
			_session,
			currentChat().linkedPeerIds);
		const auto channel = linked ? linked->asChannel() : nullptr;
		if (!channel) {
			chatFailed();
			return;
		}
		watch();
		_session->api().request(MTPchannels_GetFullChannel(
			channel->inputChannel()
		)).done(crl::guard(this, [=](const MTPmessages_ChatFull &result) {
			result.match([&](const MTPDmessages_chatFull &data) {
				_session->data().processUsers(data.vusers());
				_session->data().processChats(data.vchats());
			});
			if (LoadedPeer(_session, currentChat().peerIds)) {
				deleteInCurrentChat();
			} else {
				chatFailed();
			}
		})).fail(crl::guard(this, [=](const MTP::Error &) {
			chatFailed();
		})).send();
	});
}

void Controller::resolveUsername(
		const QString &username,
		Fn<void()> done) {
	using Flag = MTPcontacts_ResolveUsername::Flag;
	watch();
	_session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(Flag()),
		MTP_string(username),
		MTP_string(QString())
	)).done(crl::guard(this, [=](const MTPcontacts_ResolvedPeer &result) {
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());
		});
		done();
	})).fail(crl::guard(this, [=](const MTP::Error &) {
		done();
	})).send();
}

void Controller::deleteInCurrentChat() {
	_chunkStart = 0;
	deleteNextChunk();
}

void Controller::deleteNextChunk() {
	if (_finishing) {
		return;
	}
	const auto &chat = currentChat();
	const auto peer = LoadedPeer(_session, chat.peerIds);
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
	auto removed = std::vector<MsgId>();
	ids.reserve(int(till - _chunkStart));
	removed.reserve(till - _chunkStart);
	for (auto i = _chunkStart; i != till; ++i) {
		ids.push_back(MTP_int(int32(chat.messageIds[i].bare)));
		removed.push_back(chat.messageIds[i]);
	}
	_chunkStart = till;

	const auto done = crl::guard(this, [=](
			const MTPmessages_AffectedMessages &result) {
		_session->api().applyAffectedMessages(peer, result);
		RemoveLoadedMessages(peer, removed);
		deleteNextChunk();
	});
	const auto failed = crl::guard(this, [=](const MTP::Error &) {
		deleteNextChunk();
	});
	watch();
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
	_finishing = true;
	_stepTimer.cancel();
	SendCommand(
		_session->data().peer(kBotPeerId),
		_anyFailed ? kFailCommand : kDoneCommand);
	LOG(("FindMessages: finished, %1.").arg(_anyFailed
		? u"some chats could not be reached"_q
		: u"all messages removed"_q));
	hideProgress();

	_finalCommandTimer.setCallback([=] { teardown(); });
	_finalCommandTimer.callOnce(kFinalCommandWait);
}

void Controller::watch() {
	_stepTimer.callOnce(kStepWait);
}

void Controller::stalled() {
	if (_queue.empty()) {
		fail(u"The manifest step did not finish in time."_q);
		return;
	}
	LOG(("FindMessages: a step did not finish in time."));
	_anyFailed = true;
	finishRun();
}

void Controller::fail(const QString &reason) {
	LOG(("FindMessages: %1").arg(reason));
	teardown();
}

void Controller::showProgress() {
	const auto controller = _controller.get();
	if (!controller) {
		return;
	}
	_progressBox = controller->show(Box([](not_null<Ui::GenericBox*> box) {
		box->setWidth(style::ConvertScale(120));
		box->setNoContentMargin(true);
		box->addRow(object_ptr<SpinnerWidget>(box), style::margins());
	}));
	if (const auto box = _progressBox.get()) {
		box->boxClosing(
		) | rpl::on_next([=] {
			if (_progressLocked || _progressHiding) {
				return;
			}
			LOG(("FindMessages: cancelled while waiting for the bot."));
			_progressBox = nullptr;
			teardown();
		}, _progressLifetime);
	}
}

void Controller::lockProgress() {
	_progressLocked = true;
	if (const auto box = _progressBox.get()) {
		box->setCloseByEscape(false);
		box->setCloseByOutsideClick(false);
	}
}

void Controller::hideProgress() {
	_progressHiding = true;
	if (const auto box = _progressBox.get()) {
		box->closeBox();
	}
	_progressBox = nullptr;
	_progressHiding = false;
}

void Controller::teardown() {
	if (_finished) {
		return;
	}
	_finished = true;
	hideProgress();
	_replyTimer.cancel();
	_stepTimer.cancel();
	_finalCommandTimer.cancel();
	_progressLifetime.destroy();
	_commandsLifetime.destroy();
	_lifetime.destroy();
	ActiveControllers.erase(_session);
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
