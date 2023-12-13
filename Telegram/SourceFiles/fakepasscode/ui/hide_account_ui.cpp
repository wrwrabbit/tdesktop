#include "hide_account_ui.h"
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
#include "styles/style_menu_icons.h"

void HideAccountUI::Create(not_null<Ui::VerticalLayout *> content,
                      Window::SessionController*) {
    Settings::AddSubsectionTitle(content, tr::lng_hide());
    const auto toggled = Ui::CreateChild<rpl::event_stream<bool>>(content.get());
    const auto& accounts = Core::App().domain().accounts();
    account_buttons_.resize(accounts.size());
    size_t idx = 0;
    for (const auto&[index, account]: accounts) {
        auto user = account->session().user();
        auto *button = Settings::AddButton(
                content,
                tr::lng_hide_account(lt_caption, rpl::single(user->name())),
                st::settingsButton,
                {&st::menuIconLeave}
            )->toggleOn(toggled->events_starting_with_copy(_action != nullptr && _action->IsHidden(index)));
        account_buttons_[idx] = button;

        button->addClickHandler([index = index, button, this] {
            bool any_activate = false;
            for (auto* check_button : account_buttons_) {
                if (check_button->toggled()) {
                    any_activate = true;
                    break;
                }
            }

            if (any_activate && !_action) {
                FAKE_LOG(("HideAccountUI: Activate"));
                _action = dynamic_cast<FakePasscode::HideAccountAction*>(
                        _domain->local().AddAction(_index, FakePasscode::ActionType::HideAccounts));
                _action->SubscribeOnLoggingOut();
            } else if (!any_activate) {
                FAKE_LOG(("HideAccountUI: Remove"));
                _domain->local().RemoveAction(_index, FakePasscode::ActionType::HideAccounts);
                _action = nullptr;
            }

            if (_action) {
                FAKE_LOG(qsl("HideAccountUI: Set %1 to %2").arg(index).arg(button->toggled()));
                _action->SetHidden(index, button->toggled());
            }
            _domain->local().writeAccounts();
        });
        ++idx;
    }
}

HideAccountUI::HideAccountUI(QWidget *parent, gsl::not_null<Main::Domain*> domain, size_t index)
: ActionUI(parent, domain, index)
, _action(nullptr)
{
    if (auto* action = domain->local().GetAction(index, FakePasscode::ActionType::HideAccounts); action != nullptr) {
        _action = dynamic_cast<FakePasscode::HideAccountAction*>(action);
    }
}
