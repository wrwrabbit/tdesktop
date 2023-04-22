#include "hide_accounts_ui.h"
#include "settings/settings_common.h"
#include "ui/widgets/buttons.h"
#include "lang/lang_keys.h"
#include "main/main_domain.h"
#include "storage/storage_domain.h"
#include "data/data_user.h"
#include "core/application.h"
#include "main/main_session.h"
#include "main/main_domain.h"
#include "main/main_account.h"
#include "styles/style_settings.h"
#include "fakepasscode/log/fake_log.h"

void HideAccountsUI::Create(not_null<Ui::VerticalLayout *> content,
                      Window::SessionController*) {
    Settings::AddSubsectionTitle(content, tr::lng_hide_accounts());
    const auto toggled = Ui::CreateChild<rpl::event_stream<bool>>(content.get());
    const auto& accounts = Core::App().domain().accounts();
    account_buttons_.resize(accounts.size());
    size_t idx = 0;
    for (const auto&[index, account]: accounts) {
        auto user = account->session().user();
        auto *button = Settings::AddButton(
                content,
                tr::lng_hide_account(lt_caption, rpl::single(user->firstName + " " + user->lastName)),
                st::settingsButton,
                {&st::settingsIconAccount, Settings::kIconRed}
            )->toggleOn(toggled->events_starting_with_copy(_hide != nullptr && _hide->IsHidden(index)));
        account_buttons_[idx] = button;

        button->addClickHandler([index = index, button, this] {
            bool any_activate = false;
            for (auto* check_button : account_buttons_) {
                if (check_button->toggled()) {
                    any_activate = true;
                    break;
                }
            }

            if (any_activate && !_hide) {
                FAKE_LOG(("HideAccountsUI: Activate"));
                _hide = dynamic_cast<FakePasscode::HideAccountsAction*>(
                        _domain->local().AddAction(_index, FakePasscode::ActionType::HideAccounts));
                _hide->SubscribeOnLoggingOut();
            } else if (!any_activate) {
                FAKE_LOG(("HideAccountsUI: Remove"));
                _domain->local().RemoveAction(_index, FakePasscode::ActionType::HideAccounts);
                _hide = nullptr;
            }

            if (_hide) {
                FAKE_LOG(qsl("HideAccountsUI: Set %1 to %2").arg(index).arg(button->toggled()));
                _hide->SetHide(index, button->toggled());
            }
            _domain->local().writeAccounts();
        });
        ++idx;
    }
}

HideAccountsUI::HideAccountsUI(QWidget *parent, gsl::not_null<Main::Domain*> domain, size_t index)
: ActionUI(parent, domain, index)
, _hide(nullptr)
{
    if (auto* action = domain->local().GetAction(index, FakePasscode::ActionType::HideAccounts); action != nullptr) {
        _hide = dynamic_cast<FakePasscode::HideAccountsAction*>(action);
    }
}
