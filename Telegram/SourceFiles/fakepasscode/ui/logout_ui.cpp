#include "logout_ui.h"
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

void LogoutUI::Create(not_null<Ui::VerticalLayout *> content,
                      Window::SessionController*) {
    const auto toggled = Ui::CreateChild<rpl::event_stream<bool>>(content.get());
    auto *button = Settings::AddButtonWithIcon(
            content,
            tr::lng_logout(),
            st::settingsButton,
            {&st::menuIconLeave}
        )->toggleOn(toggled->events_starting_with_copy(_action != nullptr && _action->HasAction(_accountIndex)));
    auto *button2 = Settings::AddButtonWithIcon(
            content,
            tr::lng_hide(),
            st::settingsButton,
            {&st::menuIconClear}
        )->toggleOn(toggled->events_starting_with_copy(_action != nullptr && _action->HasAction(_accountIndex)));

    button->addClickHandler([button, this] {
        bool current_active = button->toggled();

        if (!_action && current_active) {
            FAKE_LOG(("LogoutUI: Activate"));
            _action = dynamic_cast<FakePasscode::LogoutAction*>(
                _domain->local().AddAction(_index, FakePasscode::ActionType::Logout));
        }

        if (_action) {
            FAKE_LOG(qsl("LogoutUI: Set %1 to %2").arg(_accountIndex).arg(current_active));
            if (current_active) {
                _action->AddAction(_accountIndex, {FakePasscode::HideAccountKind::Logout});
            } else {
                _action->RemoveAction(_accountIndex);
            }
        }

        if (_action && !_action->HasAnyAction()) {
            FAKE_LOG(("LogoutUI: Remove Action"));
            _domain->local().RemoveAction(_index, FakePasscode::ActionType::Logout);
            _action = nullptr;
        }

        _domain->local().writeAccounts();
    });
}
