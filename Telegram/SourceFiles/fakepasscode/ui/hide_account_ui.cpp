#include "hide_account_ui.h"
#include "settings/settings_common.h"
#include "ui/widgets/buttons.h"
#include "ui/vertical_list.h"
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
    const auto toggled = Ui::CreateChild<rpl::event_stream<bool>>(content.get());
    //auto user = account->session().user();
    auto *button = Settings::AddButtonWithIcon(
            content,
            tr::lng_hide(),
            st::settingsButton,
            {&st::menuIconLeave}
        )->toggleOn(toggled->events_starting_with_copy(_action != nullptr && _action->IsHidden(_accountIndex)));

    button->addClickHandler([index = _accountIndex, button, this] {
        bool any_activate = false;
        //for (auto* check_button : account_buttons_) {
        //    if (check_button->toggled()) {
        //        any_activate = true;
        //        break;
        //    }
        //}

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
}
