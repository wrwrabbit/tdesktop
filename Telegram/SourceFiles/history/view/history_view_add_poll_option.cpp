/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_add_poll_option.h"

#include "api/api_polls.h"
#include "apiwrap.h"
#include "base/event_filter.h"
#include "chat_helpers/tabbed_panel.h"
#include "chat_helpers/tabbed_selector.h"
#include "core/file_utilities.h"
#include "data/data_peer.h"
#include "data/data_poll.h"
#include "data/data_user.h"
#include "data/data_session.h"
#include "data/stickers/data_custom_emoji.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "poll/poll_media_upload.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/toast/toast.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_polls.h"

namespace HistoryView {

AddPollOptionWidget::AddPollOptionWidget(
	not_null<QWidget*> parent,
	not_null<PollData*> poll,
	FullMsgId itemId,
	not_null<Window::SessionController*> controller)
: RpWidget(parent)
, _poll(poll)
, _itemId(itemId)
, _controller(controller)
, _session(&controller->session())
, _mediaState(std::make_shared<PollMediaUpload::PollMediaState>()) {
	const auto item = _session->data().message(_itemId);
	const auto peer = item ? item->history()->peer.get() : nullptr;
	if (peer) {
		_uploader = std::make_unique<PollMediaUpload::PollMediaUploader>(
			PollMediaUpload::PollMediaUploader::Args{
				.session = _session,
				.peer = peer,
				.showError = [=](const QString &text) {
					Ui::Toast::Show(parentWidget(), text);
				},
			});
	}
	setupField();
	setupEmojiPanel();
	setupAttach();
	subscribeToPollUpdates();
}

void AddPollOptionWidget::setupField() {
	_field = Ui::CreateChild<Ui::InputField>(
		this,
		st::historyPollAddOptionField,
		Ui::InputField::Mode::NoNewlines,
		tr::lng_polls_add_option_placeholder());

	_emoji = Ui::CreateChild<Ui::IconButton>(
		this,
		st::historyPollAddOptionEmoji);

	_attach = Ui::CreateChild<Ui::IconButton>(
		this,
		st::historyPollAddOptionAttach);

	_field->setMaxLength(100);

	const auto field = _field;
	const auto emoji = _emoji;
	const auto attach = _attach;
	sizeValue(
	) | rpl::on_next([field, emoji, attach](QSize size) {
		field->setGeometry(0, 0, size.width(), size.height());
		const auto bsize = st::historyPollAddOptionButtonSize;
		const auto by = (size.height() - bsize) / 2;
		emoji->moveToLeft(st::historyPollAddOptionEmojiLeft, by);
		attach->moveToRight(0, by, size.width());
	}, _field->lifetime());

	_field->submits(
	) | rpl::on_next([=] {
		triggerSubmit();
	}, _field->lifetime());

	base::install_event_filter(_field, [=](not_null<QEvent*> event) {
		if (event->type() == QEvent::KeyPress) {
			if (static_cast<QKeyEvent*>(event.get())->key() == Qt::Key_Escape) {
				if (_emojiPanel && !_emojiPanel->isHidden()) {
					_emojiPanel->hideAnimated();
				} else {
					_cancelledEvents.fire({});
				}
				return base::EventFilterResult::Cancel;
			}
		}
		return base::EventFilterResult::Continue;
	});

	_field->setFocusFast();
}

void AddPollOptionWidget::setupEmojiPanel() {
	using Selector = ChatHelpers::TabbedSelector;

	const auto parent = parentWidget();
	_emojiPanel = base::make_unique_q<ChatHelpers::TabbedPanel>(
		parent,
		_controller,
		object_ptr<Selector>(
			nullptr,
			_controller->uiShow(),
			Window::GifPauseReason::Layer,
			Selector::Mode::EmojiOnly));

	const auto panel = _emojiPanel.get();
	panel->setDesiredHeightValues(
		1.,
		st::emojiPanMinHeight / 2,
		st::emojiPanMinHeight);
	panel->hide();
	panel->selector()->setCurrentPeer(_session->user().get());

	_emoji->installEventFilter(panel);
	_emoji->addClickHandler([=] {
		const auto button = QRect(
			_emoji->mapTo(parent, QPoint()),
			_emoji->size());
		const auto isDropDown = button.y() < parent->height() / 2;
		panel->setDropDown(isDropDown);
		if (isDropDown) {
			panel->moveTopRight(
				button.y() + button.height(),
				button.x() + button.width());
		} else {
			panel->moveBottomRight(
				button.y(),
				button.x() + button.width());
		}
		panel->toggleAnimated();
	});

	panel->selector()->emojiChosen(
	) | rpl::on_next([=](ChatHelpers::EmojiChosen data) {
		Ui::InsertEmojiAtCursor(_field->textCursor(), data.emoji);
	}, panel->lifetime());

	panel->selector()->customEmojiChosen(
	) | rpl::on_next([=](ChatHelpers::FileChosen data) {
		Data::InsertCustomEmoji(_field, data.document);
	}, panel->lifetime());
}

void AddPollOptionWidget::setupAttach() {
	if (!_uploader) {
		return;
	}
	_attach->addClickHandler([=] {
		_uploader->choosePhotoOrVideo(this, _mediaState);
	});
	_uploader->installDropToField(_field, _mediaState, false);
}

void AddPollOptionWidget::subscribeToPollUpdates() {
	_session->data().pollUpdates(
	) | rpl::filter([=](not_null<PollData*> poll) {
		return (poll == _poll);
	}) | rpl::on_next([=](not_null<PollData*> poll) {
		if (poll->closed()
			|| (int(poll->answers.size())
				>= _session->appConfig().pollOptionsLimit())) {
			_cancelledEvents.fire({});
		}
	}, lifetime());

	_session->data().itemRemoved(
	) | rpl::filter([=](not_null<const HistoryItem*> item) {
		return (item->fullId() == _itemId);
	}) | rpl::on_next([=] {
		_cancelledEvents.fire({});
	}, lifetime());
}

void AddPollOptionWidget::triggerSubmit() {
	const auto text = _field->getLastText().trimmed();
	if (text.isEmpty()) {
		return;
	}
	if (int(_poll->answers.size())
		>= _session->appConfig().pollOptionsLimit()) {
		Ui::Toast::Show(
			parentWidget(),
			tr::lng_polls_max_options_reached(tr::now));
		return;
	}

	_field->setEnabled(false);

	const auto media = _mediaState ? _mediaState->media : PollMedia();
	_session->api().polls().addAnswer(
		_itemId,
		{ text },
		media,
		[=] { _submittedEvents.fire({}); },
		[=](QString error) {
			_field->setEnabled(true);
			Ui::Toast::Show(
				parentWidget(),
				mapErrorToText(error));
		});
}

QString AddPollOptionWidget::mapErrorToText(const QString &error) {
	if (error == u"POLL_ANSWERS_TOO_MUCH"_q) {
		return tr::lng_polls_max_options_reached(tr::now);
	} else if (error == u"POLL_ANSWER_DUPLICATE"_q) {
		return tr::lng_polls_add_option_duplicate(tr::now);
	} else if (error.startsWith(u"POLL"_q) && error.contains(u"CLOSE"_q)) {
		return tr::lng_polls_add_option_closed(tr::now);
	}
	return tr::lng_polls_add_option_error(tr::now);
}

void AddPollOptionWidget::updatePosition(QPoint topLeft, int w) {
	setGeometry(
		topLeft.x(),
		topLeft.y() + st::historyPollAddOptionTop,
		w,
		st::historyPollAddOptionField.heightMin);
}

rpl::producer<> AddPollOptionWidget::submitted() const {
	return _submittedEvents.events();
}

rpl::producer<> AddPollOptionWidget::cancelled() const {
	return _cancelledEvents.events();
}

} // namespace HistoryView
