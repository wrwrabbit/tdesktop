/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "dialogs/suggestions/suggestion.h"

namespace Dialogs::TopBarSuggestions {

std::vector<Spec> AllSpecs() {
	auto result = std::vector<Spec>();
	result.push_back(MakeUnreviewedAuthSpec());
	return result;
}

} // namespace Dialogs::TopBarSuggestions
