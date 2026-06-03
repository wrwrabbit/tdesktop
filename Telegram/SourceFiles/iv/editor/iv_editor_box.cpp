/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "iv/editor/iv_editor_box.h"

#include <QtCore/QEvent>
#include <QtCore/QPointer>

#include "base/unique_qptr.h"
#include "data/data_msg_id.h"
#include "ui/image/image_location.h"
#include "data/data_types.h"
#include "iv/editor/iv_editor_state.h"
#include "iv/editor/iv_editor_widget.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/rect_part.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/tooltip.h"

#include "window/window_session_controller.h"

#include <array>
#include <memory>
#include <vector>

#include "styles/style_iv.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

namespace Iv::Editor {
namespace {

struct ToolbarButton {
	object_ptr<Ui::IconButton> widget;
	Fn<rpl::producer<TextWithEntities>()> tooltipFactory;
};

class Toolbar final : public Ui::RpWidget {
public:
	Toolbar(
		QWidget *parent,
		not_null<Widget*> editor,
		QPointer<QWidget> tooltipParent,
		Fn<void(not_null<Widget*>)> requestMedia,
		Fn<void(not_null<Widget*>)> requestMap);

	int resizeGetHeight(int width) override;

protected:
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	not_null<Ui::IconButton*> addButton(
		QString label,
		Fn<rpl::producer<TextWithEntities>()> tooltipFactory,
		const style::icon *icon,
		Fn<void()> callback);
	void addInsertButtons();
	void showHeadingMenu(not_null<Ui::IconButton*> button);
	void showTooltip(not_null<Ui::IconButton*> button);
	void hideTooltip();
	void updateTooltipGeometry();
	[[nodiscard]] ToolbarButton *buttonData(not_null<Ui::IconButton*> button);

	const QPointer<Widget> _editor;
	const QPointer<QWidget> _tooltipParent;
	const Fn<void(not_null<Widget*>)> _requestMedia;
	const Fn<void(not_null<Widget*>)> _requestMap;
	std::vector<ToolbarButton> _buttons;
	base::unique_qptr<Ui::ImportantTooltip> _tooltip;
	base::unique_qptr<Ui::PopupMenu> _menu;
	Ui::IconButton *_hovered = nullptr;

};

[[nodiscard]] QString HeadingLabel(int level) {
	switch (level) {
	case 1: return tr::lng_article_insert_heading1(tr::now);
	case 2: return tr::lng_article_insert_heading2(tr::now);
	case 3: return tr::lng_article_insert_heading3(tr::now);
	case 4: return tr::lng_article_insert_heading4(tr::now);
	case 5: return tr::lng_article_insert_heading5(tr::now);
	case 6: return tr::lng_article_insert_heading6(tr::now);
	}
	return tr::lng_article_insert_heading1(tr::now);
}

[[nodiscard]] QString SubmitText(const ShowBoxDescriptor &descriptor) {
	if (!descriptor.submitLabel.isEmpty()) {
		return descriptor.submitLabel;
	}
	switch (descriptor.submitType) {
	case ShowBoxDescriptor::SubmitType::Send:
		return tr::lng_send_button(tr::now);
	case ShowBoxDescriptor::SubmitType::Save:
		return tr::lng_settings_save(tr::now);
	}
	return tr::lng_send_button(tr::now);
}

Toolbar::Toolbar(
	QWidget *parent,
	not_null<Widget*> editor,
	QPointer<QWidget> tooltipParent,
	Fn<void(not_null<Widget*>)> requestMedia,
	Fn<void(not_null<Widget*>)> requestMap)
: Ui::RpWidget(parent)
, _editor(editor.get())
, _tooltipParent(std::move(tooltipParent))
, _requestMedia(std::move(requestMedia))
, _requestMap(std::move(requestMap)) {
	setMouseTracking(true);
	addInsertButtons();
}

not_null<Ui::IconButton*> Toolbar::addButton(
		QString label,
		Fn<rpl::producer<TextWithEntities>()> tooltipFactory,
		const style::icon *icon,
		Fn<void()> callback) {
	auto button = object_ptr<Ui::IconButton>(this, st::ivEditorToolbarButton);
	const auto raw = button.data();
	raw->setAccessibleName(label);
	raw->setIconOverride(icon, icon);
	raw->setClickedCallback([=, callback = std::move(callback)] {
		hideTooltip();
		callback();
	});
	raw->installEventFilter(this);
	_buttons.push_back({
		std::move(button),
		std::move(tooltipFactory),
	});
	return raw;
}

void Toolbar::addInsertButtons() {
	const auto insert = [=](State::InsertAction action) {
		if (_editor) {
			_editor->insertBlock(action);
		}
	};
	const auto insertType = [=](State::InsertBlockType type) {
		insert({ .type = type });
	};

	const auto heading = addButton(
		tr::lng_article_insert_heading(tr::now),
		[] { return tr::lng_article_insert_heading(tr::marked); },
		&st::ivEditorToolbarHeadingIcon,
		[] {});
	heading->setClickedCallback([=] {
		hideTooltip();
		showHeadingMenu(heading);
	});
	addButton(
		tr::lng_article_insert_blockquote(tr::now),
		[] { return tr::lng_article_insert_blockquote(tr::marked); },
		&st::ivEditorToolbarBlockquoteIcon,
		[=] { insertType(State::InsertBlockType::Blockquote); });
	addButton(
		tr::lng_article_insert_code(tr::now),
		[] { return tr::lng_article_insert_code(tr::marked); },
		&st::ivEditorToolbarCodeIcon,
		[=] { insertType(State::InsertBlockType::Code); });
	addButton(
		tr::lng_article_insert_math(tr::now),
		[] { return tr::lng_article_insert_math(tr::marked); },
		&st::ivEditorToolbarMathIcon,
		[=] { insertType(State::InsertBlockType::Math); });
	addButton(
		tr::lng_article_insert_divider(tr::now),
		[] { return tr::lng_article_insert_divider(tr::marked); },
		&st::ivEditorToolbarDividerIcon,
		[=] { insertType(State::InsertBlockType::Divider); });
	addButton(
		tr::lng_article_insert_ordered_list(tr::now),
		[] { return tr::lng_article_insert_ordered_list(tr::marked); },
		&st::ivEditorToolbarOrderedListIcon,
		[=] { insertType(State::InsertBlockType::OrderedList); });
	addButton(
		tr::lng_article_insert_bullet_list(tr::now),
		[] { return tr::lng_article_insert_bullet_list(tr::marked); },
		&st::ivEditorToolbarBulletListIcon,
		[=] { insertType(State::InsertBlockType::BulletList); });
	addButton(
		tr::lng_article_insert_task_list(tr::now),
		[] { return tr::lng_article_insert_task_list(tr::marked); },
		&st::ivEditorToolbarTaskListIcon,
		[=] { insertType(State::InsertBlockType::TaskList); });
	addButton(
		tr::lng_article_insert_pullquote(tr::now),
		[] { return tr::lng_article_insert_pullquote(tr::marked); },
		&st::ivEditorToolbarPullquoteIcon,
		[=] { insertType(State::InsertBlockType::Pullquote); });
	if (_requestMedia) {
		addButton(
			tr::lng_article_insert_media(tr::now),
			[] { return tr::lng_article_insert_media(tr::marked); },
			&st::ivEditorToolbarAttachIcon,
			[=] {
				if (_editor) {
					_requestMedia(not_null<Widget*>(_editor.data()));
				}
			});
	}
	addButton(
		tr::lng_article_insert_details(tr::now),
		[] { return tr::lng_article_insert_details(tr::marked); },
		&st::ivEditorToolbarDetailsIcon,
		[=] { insertType(State::InsertBlockType::Details); });
	addButton(
		tr::lng_article_insert_table(tr::now),
		[] { return tr::lng_article_insert_table(tr::marked); },
		&st::ivEditorToolbarTableIcon,
		[=] { insertType(State::InsertBlockType::Table); });

	if (_requestMap) {
		addButton(
			tr::lng_article_insert_map(tr::now),
			[] { return tr::lng_article_insert_map(tr::marked); },
			&st::menuIconAddress,
			[=] {
				if (_editor) {
					_requestMap(not_null<Widget*>(_editor.data()));
				}
			});
	}
}

void Toolbar::showHeadingMenu(not_null<Ui::IconButton*> button) {
	_menu = base::make_unique_q<Ui::PopupMenu>(this);
	for (const auto level : std::array{ 1, 2, 3, 4, 5, 6 }) {
		_menu->addAction(
			HeadingLabel(level),
			[=] {
				if (_editor) {
					_editor->insertBlock({
						.type = State::InsertBlockType::Heading,
						.headingLevel = level,
					});
				}
			});
	}
	_menu->popup(button->mapToGlobal(QPoint(0, button->height())));
}

int Toolbar::resizeGetHeight(int width) {
	const auto padding = st::ivEditorToolbarPadding;
	const auto buttonWidth = st::ivEditorToolbarButton.width;
	const auto buttonHeight = st::ivEditorToolbarButton.height;
	const auto right = width - padding.right();
	auto left = padding.left();
	auto top = padding.top();
	for (const auto &button : _buttons) {
		if (left > padding.left() && left + buttonWidth > right) {
			left = padding.left();
			top += buttonHeight + st::ivEditorToolbarRowSkip;
		}
		button.widget->moveToLeft(left, top, width);
		left += buttonWidth + st::ivEditorToolbarButtonSkip;
	}
	updateTooltipGeometry();
	return top + buttonHeight + padding.bottom();
}

bool Toolbar::eventFilter(QObject *object, QEvent *event) {
	for (const auto &data : _buttons) {
		if (data.widget.data() != object) {
			continue;
		}
		const auto button = data.widget.data();
		if (event->type() == QEvent::Enter) {
			showTooltip(not_null<Ui::IconButton*>(button));
		} else if (event->type() == QEvent::Leave && _hovered == button) {
			hideTooltip();
		}
		break;
	}
	return Ui::RpWidget::eventFilter(object, event);
}

void Toolbar::showTooltip(not_null<Ui::IconButton*> button) {
	hideTooltip();
	const auto data = buttonData(button);
	if (!data) {
		return;
	}
	_hovered = button;
	const auto tooltipParent = _tooltipParent
		? _tooltipParent.data()
		: (parentWidget() ? parentWidget() : this);
	_tooltip.reset(Ui::CreateChild<Ui::ImportantTooltip>(
		tooltipParent,
		Ui::MakeNiceTooltipLabel(
			tooltipParent,
			data->tooltipFactory(),
			st::boxWideWidth,
			st::defaultImportantTooltipLabel),
		st::defaultImportantTooltip));
	_tooltip->setAttribute(Qt::WA_TransparentForMouseEvents);
	_tooltip->toggleFast(false);
	updateTooltipGeometry();
	_tooltip->raise();
	_tooltip->toggleAnimated(true);
}

void Toolbar::hideTooltip() {
	_hovered = nullptr;
	if (_tooltip) {
		_tooltip->toggleFast(false);
		_tooltip = nullptr;
	}
}

void Toolbar::updateTooltipGeometry() {
	if (!_tooltip || !_hovered) {
		return;
	}
	const auto tooltipParent = _tooltip->parentWidget();
	const auto geometry = Ui::MapFrom(
		tooltipParent,
		_hovered,
		_hovered->rect());
	_tooltip->pointAt(geometry, RectPart::Top | RectPart::Center);
}

ToolbarButton *Toolbar::buttonData(not_null<Ui::IconButton*> button) {
	for (auto &data : _buttons) {
		if (data.widget.data() == button.get()) {
			return &data;
		}
	}
	return nullptr;
}

void SetupBox(
		not_null<Ui::GenericBox*> box,
		ShowBoxDescriptor descriptor) {
	box->setWidth(st::boxWideWidth);
	box->setNoContentMargin(true);
	box->setCloseByEscape(false);
	box->setCloseByOutsideClick(false);

	const auto editor = box->addRow(object_ptr<Widget>(
		box,
		descriptor.controller,
		descriptor.peer,
		descriptor.state,
		std::move(descriptor.showLimitToast)),
		style::margins());
	const auto tooltipParent = box->getDelegate()->outerContainer();
	box->setPinnedToTopContent(object_ptr<Toolbar>(
		box,
		editor,
		tooltipParent,
		std::move(descriptor.requestMedia),
		std::move(descriptor.requestMap)));

	const auto weak = QPointer<Ui::GenericBox>(box.get());
	const auto submit = box->addButton(
		rpl::single(SubmitText(descriptor)),
		[=, confirmed = std::move(descriptor.confirmed)] {
			if (editor->commitInlineField()
				== State::ApplyResult::Failed) {
				return;
			}
			if ((!confirmed || confirmed()) && weak) {
				weak->closeBox();
			}
		});
	if (submit && descriptor.setupSubmitButton) {
		descriptor.setupSubmitButton(not_null<Ui::RpWidget*>(submit.data()));
	}
	box->addButton(tr::lng_cancel(), [=, cancelled = std::move(descriptor.cancelled)] {
		if ((!cancelled || cancelled()) && weak) {
			weak->closeBox();
		}
	});

	box->setFocusCallback([=] {
		editor->activateInitialNode();
	});
	box->setShowFinishedCallback([=] {
		editor->activateInitialNode();
	});
}

} // namespace

void ShowBox(ShowBoxDescriptor descriptor) {
	if (!descriptor.state) {
		descriptor.state = std::make_shared<State>();
	}
	descriptor.controller->show(Box<Ui::GenericBox>(
		SetupBox,
		std::move(descriptor)));
}

void ShowBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	auto descriptor = ShowBoxDescriptor{
		.controller = controller,
		.peer = peer,
		.state = std::make_shared<State>(),
	};
	ShowBox(std::move(descriptor));
}

} // namespace Iv::Editor
