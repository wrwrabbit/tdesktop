/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "spellcheck/platform/platform_language.h"

namespace Ui::Text {
struct MarkedContext;
} // namespace Ui::Text

namespace Ui {

class GenericBox;

struct TranslateBoxContentArgs {
	TextWithEntities text;
	bool hasCopyRestriction = false;
	Text::MarkedContext textContext;
	rpl::producer<LanguageId> to;
	Fn<void()> chooseTo;
	Fn<void(LanguageId, Fn<void(std::optional<TextWithEntities>)>)> request;
};

void TranslateBoxContent(
	not_null<GenericBox*> box,
	TranslateBoxContentArgs &&args);

} // namespace Ui
