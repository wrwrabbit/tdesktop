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
#include "ui/widgets/scroll_area.h"
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

	const auto wrap = Ui::CreateChild<Ui::RpWidget>(this);
	_scroll = Ui::CreateChild<Ui::ScrollArea>(wrap, st::stickersScroll);

	auto descriptor = ChatHelpers::StickersListDescriptor{
		.show = _show,
		.mode = ChatHelpers::StickersListMode::UserpicBuilder,
		.paused = [] { return false; },
	};
	_list = _scroll->setOwnedWidget(
		object_ptr<ChatHelpers::StickersListWidget>(
			_scroll,
			std::move(descriptor)));
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

	rpl::combine(
		_scroll->scrollTopValue(),
		_scroll->heightValue()
	) | rpl::on_next([=](int top, int height) {
		_list->setVisibleTopBottom(top, top + height);
	}, _list->lifetime());

	setDimensions(st::boxWideWidth, st::stickersMaxHeight);

	addButton(tr::lng_cancel(), [=] { closeBox(); });

	wrap->resize(st::boxWideWidth, st::stickersMaxHeight);
	_scroll->resize(wrap->size());
	setInnerWidget(object_ptr<Ui::RpWidget>::fromRaw(wrap));
}

void StickerPickerBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
	if (_scroll) {
		_scroll->resize(_scroll->parentWidget()->size());
		const auto width = _scroll->width();
		_list->resizeToWidth(width);
		_list->setMinimalHeight(width, _scroll->height());
	}
}
