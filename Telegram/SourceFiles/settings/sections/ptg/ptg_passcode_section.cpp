/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/ptg/ptg_passcode_section.h"

#include "settings/sections/ptg/ptg_account_section.h"
#include "settings/settings_common.h"
#include "fakepasscode/action.h"
#include "fakepasscode/fake_passcode.h"
#include "fakepasscode/ptg.h"
#include "fakepasscode/ui/action_ui.h"
#include "fakepasscode/ui/fakepasscode_box.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "storage/storage_domain.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_window.h"

namespace Settings {
namespace {

struct FakePasscodeSectionFactory : AbstractSectionFactory {
	explicit FakePasscodeSectionFactory(size_t i) : passcodeIndex(i) {}

	object_ptr<AbstractSection> create(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<Ui::ScrollArea*>,
		rpl::producer<Container>) const override {
		return object_ptr<FakePasscodeSection>(
			parent, controller, passcodeIndex);
	}

	const size_t passcodeIndex;
};

[[nodiscard]] object_ptr<Ui::SettingsButton> MakeAccountButton(
		QWidget *parent,
		not_null<Main::Account*> account) {
	const auto session = &account->session();
	const auto user = session->user();

	auto text = rpl::single(
		user->name()
	) | rpl::then(session->changes().realtimeNameUpdates(
		user
	) | rpl::map([=] {
		return user->name();
	}));
	auto result = object_ptr<Ui::SettingsButton>(
		parent,
		rpl::duplicate(text),
		st::mainMenuAddAccountButton);
	const auto raw = result.data();

	struct State {
		explicit State(QWidget *parent) : userpic(parent) {
			userpic.setAttribute(Qt::WA_TransparentForMouseEvents);
		}
		Ui::RpWidget userpic;
		Ui::PeerUserpicView view;
	};
	const auto state = raw->lifetime().make_state<State>(raw);

	const auto userpicSkip = 2 * st::mainMenuAccountLine + st::lineWidth;
	const auto userpicSize = st::mainMenuAccountSize + userpicSkip * 2;
	raw->heightValue(
	) | rpl::on_next([=](int height) {
		const auto left = st::mainMenuAddAccountButton.iconLeft
			+ (st::settingsIconAdd.width() - userpicSize) / 2;
		const auto top = (height - userpicSize) / 2;
		state->userpic.setGeometry(left, top, userpicSize, userpicSize);
	}, state->userpic.lifetime());

	state->userpic.paintRequest(
	) | rpl::on_next([=] {
		auto p = Painter(&state->userpic);
		const auto size = st::mainMenuAccountSize;
		const auto line = st::mainMenuAccountLine;
		const auto skip = 2 * line + st::lineWidth;
		const auto full = size + skip * 2;
		user->paintUserpicLeft(p, state->view, skip, skip, full, size);
	}, state->userpic.lifetime());

	return result;
}

[[nodiscard]] QString GetAccountActionsDescription(
		not_null<Main::Domain*> domain,
		size_t passcodeIndex,
		int accountIndex) {
	QString result;
	for (const auto &type : FakePasscode::kAvailableAccountActions) {
		if (domain->local().ContainsAction(passcodeIndex, type)) {
			const auto act = domain->local().GetAction(passcodeIndex, type);
			const auto accact = dynamic_cast<FakePasscode::AccountAction*>(act);
			if (accact) {
				const auto part = accact->GetDescriptionFor(accountIndex);
				if (!part.isEmpty()) {
					if (!result.isEmpty()) {
						result += u", "_q;
					}
					result += part;
				}
			}
		}
	}
	return result;
}

} // namespace

FakePasscodeSection::FakePasscodeSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	size_t passcodeIndex)
: AbstractSection(parent, controller)
, _passcodeIndex(passcodeIndex) {
	setupContent();
}

Type FakePasscodeSection::MakeId(size_t passcodeIndex) {
	return std::make_shared<FakePasscodeSectionFactory>(passcodeIndex);
}

Type FakePasscodeSection::id() const {
	return MakeId(_passcodeIndex);
}

rpl::producer<QString> FakePasscodeSection::title() {
	return tr::lng_fakepasscode(
		lt_caption,
		controller()->session().domain().local().GetFakePasscodeName(
			_passcodeIndex));
}

rpl::producer<> FakePasscodeSection::sectionShowBack() {
	return _showBackRequests.events();
}

void FakePasscodeSection::showFinished() {
	AbstractSection::showFinished();
	PTG::FireFakePasscodeUpdates();
}

void FakePasscodeSection::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	auto *domain = &controller()->session().domain();

	Ui::AddSubsectionTitle(content, tr::lng_fakeaccountaction_list());
	const auto accounts = Core::App().domain().orderedAccountsEx();
	for (const auto &record : accounts) {
		const auto texts = content->lifetime().make_state<rpl::event_stream<QString>>();
		const auto button = content->add(
			MakeAccountButton(content, record.account));

		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				texts->events(),
				st::defaultSettingsRightLabel),
			style::margins(
				st::boxRowPadding.left(),
				0,
				st::boxRowPadding.right(),
				st::defaultVerticalListSkip * 2));

		const auto accountIndex = record.index;
		const auto updateText = [=] {
			texts->fire(GetAccountActionsDescription(
				domain, _passcodeIndex, accountIndex));
		};
		updateText();

		PTG::GetFakePasscodeUpdates(
		) | rpl::on_next(updateText, content->lifetime());

		button->addClickHandler([=] {
			showOther(FakePasscodeAccountSection::MakeId(
				_passcodeIndex, accountIndex));
		});
	}

	Ui::AddDivider(content);
	Ui::AddSubsectionTitle(
		content,
		tr::lng_fakeglobalaction_list(),
		style::margins(0, st::defaultVerticalListSkip, 0, 0));
	for (const auto &record : FakePasscode::kAvailableGlobalActions) {
		const auto ui = GetUIByAction(
			record.Type, domain, _passcodeIndex, this);
		ui->Create(content, controller());
		if (record.HasDivider) {
			Ui::AddDivider(content);
		}
	}

	Ui::AddSubsectionTitle(content, tr::lng_fakepassaction_list());
	Settings::AddButtonWithIcon(
		content,
		tr::lng_fakepasscode_change(),
		st::settingsButton,
		{ &st::menuIconEdit }
	)->addClickHandler([=] {
		controller()->show(Box<FakePasscodeBox>(
			controller(), false, false, _passcodeIndex));
	});
	Settings::AddButtonWithIcon(
		content,
		tr::lng_remove_fakepasscode(),
		st::settingsAttentionButtonWithIcon,
		{ &st::menuIconRemove }
	)->addClickHandler([=] {
		domain->local().RemoveFakePasscode(_passcodeIndex);
		_showBackRequests.fire({});
	});

	Ui::ResizeFitChild(this, content);
}

} // namespace Settings
