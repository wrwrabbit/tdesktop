/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/iv_editor_box.h"

#include "data/data_msg_id.h"
#include "ui/image/image_location.h"
#include "data/data_types.h"
#include "iv/iv_editor_state.h"
#include "iv/iv_editor_widget.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDate>

#include "window/window_session_controller.h"

#include <memory>

namespace Iv::Editor {
namespace {

void SetupBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	box->setTitle(tr::lng_article_editor_title());
	box->setWidth(st::boxWideWidth);
	box->setNoContentMargin(true);

	const auto state = std::make_shared<State>();
	const auto editor = box->addRow(object_ptr<Widget>(
		box,
		controller,
		peer,
		state),
		style::margins());
	const auto toolbar = box->setPinnedToTopContent(
		object_ptr<Ui::VerticalLayout>(box));

	toolbar->add(object_ptr<Ui::SettingsButton>(
		toolbar,
		tr::lng_article_insert_heading1(),
		st::settingsButtonNoIcon))->setClickedCallback([=] {
		editor->insertHeading1();
	});
	toolbar->add(object_ptr<Ui::SettingsButton>(
		toolbar,
		tr::lng_article_insert_blockquote(),
		st::settingsButtonNoIcon))->setClickedCallback([=] {
		editor->insertBlockquote();
	});

	box->setFocusCallback([=] {
		editor->activateInitialNode();
	});
	box->setShowFinishedCallback([=] {
		editor->activateInitialNode();
	});
}

} // namespace

void ShowBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	controller->show(Box<Ui::GenericBox>(SetupBox, controller, peer));
}

} // namespace Iv::Editor
