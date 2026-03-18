/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

struct PollData;
class DocumentData;

namespace Ui {
class DropdownMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace HistoryView {

void FillPollAnswerMenu(
	not_null<Ui::DropdownMenu*> menu,
	not_null<PollData*> poll,
	const QByteArray &option,
	not_null<DocumentData*> document,
	FullMsgId itemId,
	not_null<Window::SessionController*> controller);

} // namespace HistoryView
