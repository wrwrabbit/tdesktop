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
#include "data/data_poll.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/toast/toast.h"
#include "styles/style_chat.h"
#include "styles/style_polls.h"

namespace HistoryView {

AddPollOptionWidget::AddPollOptionWidget(
	not_null<QWidget*> parent,
	not_null<PollData*> poll,
	FullMsgId itemId,
	not_null<Main::Session*> session)
: RpWidget(parent)
, _poll(poll)
, _itemId(itemId)
, _session(session) {
	setupField();
	subscribeToPollUpdates();
}

void AddPollOptionWidget::setupField() {
	_field = Ui::CreateChild<Ui::InputField>(
		this,
		st::historyPollAddOptionField,
		Ui::InputField::Mode::SingleLine,
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
				_cancelledEvents.fire({});
				return base::EventFilterResult::Cancel;
			}
		}
		return base::EventFilterResult::Continue;
	});

	_field->setFocusFast();
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

	_session->api().polls().addAnswer(
		_itemId,
		{ text },
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
