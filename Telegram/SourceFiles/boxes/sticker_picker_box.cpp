/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/sticker_picker_box.h"

#include "chat_helpers/compose/compose_show.h"
#include "chat_helpers/stickers_list_widget.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "data/stickers/data_stickers.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"

StickerPickerBox::StickerPickerBox(
	QWidget*,
	std::shared_ptr<ChatHelpers::Show> show,
	Fn<void(not_null<DocumentData*>)> chosen)
: _show(std::move(show))
, _chosen(std::move(chosen)) {
}

void StickerPickerBox::prepare() {
	setTitle(tr::lng_stickers_pick_existing_title());

	auto descriptor = ChatHelpers::StickersListDescriptor{
		.show = _show,
		.mode = ChatHelpers::StickersListMode::UserpicBuilder,
		.paused = [] { return false; },
	};
	auto list = object_ptr<ChatHelpers::StickersListWidget>(
		this,
		std::move(descriptor));
	_list = list.data();

	_list->refreshRecent();
	_list->refreshStickers();

	_list->chosen(
	) | rpl::on_next([=](const ChatHelpers::FileChosen &chosen) {
		const auto document = chosen.document;
		if (_chosen) {
			_chosen(document);
		}
		closeBox();
	}, _list->lifetime());

	setInnerWidget(std::move(list));
	setDimensions(st::boxWideWidth, st::stickersMaxHeight);

	scrolls(
	) | rpl::on_next([=, this] {
		if (_list) {
			const auto top = scrollTop();
			_list->setVisibleTopBottom(top, top + scrollHeight());
		}
	}, lifetime());

	addButton(tr::lng_cancel(), [=] { closeBox(); });
}

void StickerPickerBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
	if (_list) {
		const auto width = this->width();
		_list->resizeToWidth(width);
		_list->setMinimalHeight(width, st::stickersMaxHeight);
	}
}
