/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/menu/history_view_poll_menu.h"

#include "lang/lang_keys.h"
#include "ui/widgets/dropdown_menu.h"
#include "ui/widgets/menu/menu_action.h"
#include "ui/painter.h"
#include "history/view/history_view_group_call_bar.h"
#include "boxes/sticker_set_box.h"
#include "data/data_poll.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "data/data_file_origin.h"
#include "data/stickers/data_stickers.h"
#include "api/api_polls.h"
#include "api/api_toggling_media.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"
#include "apiwrap.h"
#include "styles/style_chat.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

namespace HistoryView {
namespace {

class VotersItem final : public Ui::Menu::ItemBase {
public:
	VotersItem(
		not_null<Ui::Menu::Menu*> parent,
		const style::Menu &st,
		int votes,
		const std::vector<not_null<PeerData*>> &recentVoters);

	not_null<QAction*> action() const override;
	bool isEnabled() const override;

protected:
	int contentHeight() const override;

private:
	void paint(Painter &p);

	const not_null<QAction*> _dummyAction;
	const style::Menu &_st;
	const int _votes = 0;
	const int _height = 0;
	QImage _userpics;
	int _userpicsWidth = 0;
};

VotersItem::VotersItem(
	not_null<Ui::Menu::Menu*> parent,
	const style::Menu &st,
	int votes,
	const std::vector<not_null<PeerData*>> &recentVoters)
: ItemBase(parent, st)
, _dummyAction(Ui::CreateChild<QAction>(parent.get()))
, _st(st)
, _votes(votes)
, _height(st.itemPadding.top()
	+ st.itemStyle.font->height
	+ st.itemPadding.bottom()) {
	auto prepared = PrepareUserpicsInRow(
		recentVoters,
		st::historyCommentsUserpics);
	_userpics = std::move(prepared.image);
	_userpicsWidth = prepared.width;

	const auto votesText = tr::lng_polls_votes_count(
		tr::now,
		lt_count,
		_votes);
	const auto spacing = _userpicsWidth > 0
		? st::normalFont->spacew * 2
		: 0;
	const auto textWidth = st.itemStyle.font->width(votesText);
	const auto &textPadding = st::defaultMenu.itemPadding;
	const auto minWidth = textPadding.left()
		+ textWidth
		+ spacing
		+ _userpicsWidth
		+ st.itemPadding.right();
	setMinWidth(minWidth);

	paintRequest() | rpl::on_next([=] {
		Painter p(this);
		paint(p);
	}, lifetime());

	fitToMenuWidth();
}

not_null<QAction*> VotersItem::action() const {
	return _dummyAction;
}

bool VotersItem::isEnabled() const {
	return false;
}

int VotersItem::contentHeight() const {
	return _height;
}

void VotersItem::paint(Painter &p) {
	p.fillRect(0, 0, width(), _height, _st.itemBg);

	const auto votesText = tr::lng_polls_votes_count(
		tr::now,
		lt_count,
		_votes);

	p.setPen(_st.itemFg);
	p.setFont(_st.itemStyle.font);
	const auto &textPadding = st::defaultMenu.itemPadding;
	const auto textY = _st.itemPadding.top()
		+ _st.itemStyle.font->ascent;
	p.drawText(textPadding.left(), textY, votesText);

	if (!_userpics.isNull()) {
		const auto userpicsHeight
			= _userpics.height() / style::DevicePixelRatio();
		const auto x = width() - _st.itemPadding.right() - _userpicsWidth;
		const auto y = (_height - userpicsHeight) / 2;
		p.drawImage(x, y, _userpics);
	}
}

} // namespace

void FillPollAnswerMenu(
		not_null<Ui::DropdownMenu*> menu,
		not_null<PollData*> poll,
		const QByteArray &option,
		not_null<DocumentData*> document,
		FullMsgId itemId,
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	auto addedVotersItem = false;
	if (const auto answer = poll->answerByOption(option)) {
		const auto canVote = !option.isEmpty()
			&& !poll->closed()
			&& !poll->quiz()
			&& !poll->voted();
		if (!canVote && answer->votes > 0) {
			menu->addAction(
				base::make_unique_q<VotersItem>(
					menu->menu(),
					menu->menu()->st(),
					answer->votes,
					answer->recentVoters));
			menu->addSeparator(&st::expandedMenuSeparator);
			addedVotersItem = true;
		}
	}
	if (!option.isEmpty()
		&& !poll->closed()
		&& !poll->quiz()) {
		if (poll->voted()
			&& !poll->revotingDisabled()) {
			menu->addAction(
				tr::lng_polls_retract(tr::now),
				[=] {
					session->api().polls().sendVotes(
						itemId,
						{});
				},
				&st::menuIconRetractVote);
		} else if (!poll->voted()
			&& poll->sendingVotes.empty()) {
			menu->addAction(
				tr::lng_polls_submit_votes(tr::now),
				[=] {
					session->api().polls().sendVotes(
						itemId,
						{ option });
				},
				&st::menuIconSelect);
			if (!addedVotersItem) {
				menu->addSeparator(
					&st::expandedMenuSeparator);
			}
		}
	}
	const auto show = controller->uiShow();
	const auto isFaved
		= document->owner().stickers().isFaved(document);
	menu->addAction(
		(isFaved
			? tr::lng_faved_stickers_remove
			: tr::lng_faved_stickers_add)(tr::now),
		[=] {
			Api::ToggleFavedSticker(
				show,
				document,
				Data::FileOriginStickerSet(
					Data::Stickers::FavedSetId,
					0));
		},
		isFaved
			? &st::menuIconUnfave
			: &st::menuIconFave);
	if (const auto sticker = document->sticker()) {
		const auto setId = sticker->set;
		if (setId.id) {
			menu->addAction(
				tr::lng_view_button_stickerset(tr::now),
				[=] {
					show->show(Box<StickerSetBox>(
						show,
						setId,
						Data::StickersType::Stickers));
				},
				&st::menuIconStickers);
		}
	}
}

} // namespace HistoryView
