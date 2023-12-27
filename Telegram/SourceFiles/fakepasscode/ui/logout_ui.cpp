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
    FakePasscode::HideAccountKind::HideAccountEnum value 
        = (_action != nullptr) ? _action->GetData(_accountIndex).Kind : FakePasscode::HideAccountKind::None;

    const auto tgl_logout = Ui::CreateChild<rpl::event_stream<bool>>(content.get());
    auto btn_logout = Settings::AddButtonWithIcon(
            content,
            tr::lng_logout(),
            st::settingsButton,
            {&st::menuIconLeave}
    )->toggleOn(tgl_logout->events_starting_with_copy(value == FakePasscode::HideAccountKind::Logout));
    const auto tgl_hide = Ui::CreateChild<rpl::event_stream<bool>>(content.get());
    auto btn_hide = Settings::AddButtonWithIcon(
            content,
            tr::lng_hide(),
            st::settingsButton,
            {&st::menuIconClear}
    )->toggleOn(tgl_hide->events_starting_with_copy(value == FakePasscode::HideAccountKind::HideAccount));
    
    auto clickHandler = [this, btn_logout, tgl_logout, btn_hide, tgl_hide] {
        bool is_logout = btn_logout->toggled();
        bool is_hide = btn_hide->toggled();
        bool current_active = is_logout || is_hide;

        FakePasscode::HideAccountKind value;
        if (is_logout) {
            value = { FakePasscode::HideAccountKind::Logout };
        } else if (is_hide) {
            value = { FakePasscode::HideAccountKind::HideAccount };
        } else {
            value = { FakePasscode::HideAccountKind::None };
        }

        if (!_action && current_active) {
            FAKE_LOG(("LogoutUI: Activate"));
            _action = dynamic_cast<FakePasscode::LogoutAction*>(
                _domain->local().AddAction(_index, FakePasscode::ActionType::Logout));
        }

        if (_action) {
            FAKE_LOG(qsl("LogoutUI: Set %1 to %2").arg(_accountIndex).arg(value.Kind));
            if (current_active) {
                _action->UpdateOrAddAction(_accountIndex, value);
            }
            else {
                _action->RemoveAction(_accountIndex);
            }
        }

        if (_action && !_action->HasAnyAction()) {
            FAKE_LOG(("LogoutUI: Remove Action"));
            _domain->local().RemoveAction(_index, FakePasscode::ActionType::Logout);
            _action = nullptr;
        }

        tgl_logout->fire(value.Kind == FakePasscode::HideAccountKind::Logout);
        tgl_hide->fire(value.Kind == FakePasscode::HideAccountKind::HideAccount);

        _domain->local().writeAccounts();
    };

    btn_logout->addClickHandler(clickHandler);
    btn_hide->addClickHandler(clickHandler);
}
