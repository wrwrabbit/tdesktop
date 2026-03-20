/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "ui/text/text_entity.h"

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class GenericBox;
class Show;
} // namespace Ui

namespace HistoryView::Controls {

struct ComposeAiBoxArgs {
	not_null<Main::Session*> session;
	TextWithEntities text;
	Fn<void(TextWithEntities &&)> apply;
};

void ComposeAiBox(not_null<Ui::GenericBox*> box, ComposeAiBoxArgs &&args);
void ShowComposeAiBox(std::shared_ptr<Ui::Show> show, ComposeAiBoxArgs &&args);

} // namespace HistoryView::Controls
