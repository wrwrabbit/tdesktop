/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/ptg/ptg_account_section.h"

#include "settings/settings_common.h"
#include "fakepasscode/action.h"
#include "fakepasscode/fake_passcode.h"
#include "fakepasscode/ui/action_ui.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "data/data_user.h"
#include "storage/storage_domain.h"
#include "ui/vertical_list.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

struct FakePasscodeAccountSectionFactory : AbstractSectionFactory {
	FakePasscodeAccountSectionFactory(size_t pi, int ai)
	: passcodeIndex(pi), accountIndex(ai) {}

	object_ptr<AbstractSection> create(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<Ui::ScrollArea*>,
		rpl::producer<Container>) const override {
		return object_ptr<FakePasscodeAccountSection>(
			parent, controller, passcodeIndex, accountIndex);
	}

	const size_t passcodeIndex;
	const int accountIndex;
};

} // namespace

FakePasscodeAccountSection::FakePasscodeAccountSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	size_t passcodeIndex,
	int accountIndex)
: AbstractSection(parent, controller)
, _passcodeIndex(passcodeIndex)
, _accountIndex(accountIndex) {
	setupContent();
}

Type FakePasscodeAccountSection::MakeId(size_t passcodeIndex, int accountIndex) {
	return std::make_shared<FakePasscodeAccountSectionFactory>(
		passcodeIndex, accountIndex);
}

Type FakePasscodeAccountSection::id() const {
	return MakeId(_passcodeIndex, _accountIndex);
}

rpl::producer<QString> FakePasscodeAccountSection::title() {
	for (const auto &account : controller()->session().domain().accounts()) {
		if (account.index == _accountIndex) {
			return tr::lng_fakeaccountaction_title(
				lt_caption,
				rpl::single(account.account->session().user()->name()));
		}
	}
	return rpl::single(QString());
}

void FakePasscodeAccountSection::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	auto *domain = &controller()->session().domain();

	Ui::AddSubsectionTitle(content, tr::lng_fakeaccountaction_list());

	for (const auto &type : FakePasscode::kAvailableAccountActions) {
		const auto ui = GetAccountUIByAction(
			type, domain, _passcodeIndex, _accountIndex, this);
		ui->Create(content, controller());
	}

	Ui::ResizeFitChild(this, content);
}

} // namespace Settings
