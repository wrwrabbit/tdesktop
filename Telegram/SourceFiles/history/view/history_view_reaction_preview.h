/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class DocumentData;

namespace Ui {
class DropdownMenu;
} // namespace Ui

namespace Data {
struct ReactionId;
} // namespace Data

namespace Window {
class SessionController;
} // namespace Window

namespace HistoryView {

bool ShowStickerPreview(
	not_null<Window::SessionController*> controller,
	FullMsgId origin,
	not_null<DocumentData*> document,
	Fn<void(not_null<Ui::DropdownMenu*>)> fillMenu = nullptr);

bool ShowReactionPreview(
	not_null<Window::SessionController*> controller,
	FullMsgId origin,
	Data::ReactionId reactionId,
	bool emojiPreview = false);

} // namespace HistoryView
