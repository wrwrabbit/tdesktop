/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "findmessages/find_messages_link.h"

#include "base/qthelp_regex.h"
#include "base/qthelp_url.h"
#include "data/data_peer.h"
#include "dialogs/dialogs_key.h"
#include "fakepasscode/ptg.h"
#include "findmessages/find_messages.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "ui/boxes/confirm_box.h"
#include "window/window_session_controller.h"

namespace FindMessages {
namespace {

const auto kAutoDeleteAction = u"auto-delete-messages"_q;

} // namespace

bool TryHandleFindMessagesLink(
		Window::SessionController *controller,
		const qthelp::RegularExpressionMatch &match,
		const QVariant &context) {
	if (!controller || PTG::IsFakeActive()) {
		return false;
	}
	const auto params = qthelp::url_parse_params(
		match->captured(1),
		qthelp::UrlParamNameTransform::ToLower);
	const auto history = controller->activeChatCurrent().history();
	const auto inBotChat = history
		&& (history->peer->id == peerFromUser(kBotUserId));
	if (inBotChat && params.value(u"ptg-action"_q) == kAutoDeleteAction) {
		Start(controller);
	} else {
		// An action nobody is waiting for: either it's not known to this
		// version or it came outside of the bot chat.
		controller->show(Ui::MakeInformBox(tr::lng_ptg_unknown_link_warning()));
	}
	return true;
}

} // namespace FindMessages
