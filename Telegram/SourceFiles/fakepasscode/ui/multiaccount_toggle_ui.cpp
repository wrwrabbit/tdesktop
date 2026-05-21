#include "multiaccount_toggle_ui.h"

#include "fakepasscode/log/fake_log.h"

#include "core/application.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_account.h"
#include "data/data_user.h"
#include "storage/storage_domain.h"
#include "settings/settings_common.h"
#include "ui/widgets/buttons.h"
#include "ui/vertical_list.h"
#include "styles/style_settings.h"
#include "styles/style_menu_icons.h"

MultiAccountToggleUi::MultiAccountToggleUi(QWidget *parent, gsl::not_null<Main::Domain*> domain, size_t index, int accountIndex, Description description)
    : ActionUI(parent, domain, index)
    , _description(std::move(description))
    , _accountIndex(accountIndex) {
    if (auto* action = domain->local().GetAction(index, _description.action_type); action != nullptr) {
        _action = dynamic_cast<Action*>(action);
    }
}

void MultiAccountToggleUi::Create(not_null<Ui::VerticalLayout *> content,
                                  Window::SessionController*) {
    auto *button = Settings::AddButtonWithIcon(
            content,
            _description.title(),
            st::settingsButton,
            {&st::menuIconRemove}
    )->toggleOn(rpl::single(_action != nullptr && _action->HasAction(_accountIndex)));

    button->toggledChanges(
    ) | rpl::on_next([this](bool value) {
        SetActionValue(value, FakePasscode::ToggleAction{});
    }, button->lifetime());
}

void MultiAccountToggleUi::SetActionValue(bool current_active, FakePasscode::ToggleAction value) {
    if (current_active && !_action) {
        FAKE_LOG(qsl("%1: Activate").arg(_description.name));
        _action = dynamic_cast<Action*>(
            _domain->local().AddAction(_index, _description.action_type));
    }

    if (_action) {
        FAKE_LOG(qsl("%1: Set %2 to %3").arg(_description.name).arg(_accountIndex).arg(current_active));
        if (current_active) {
            _action->UpdateOrAddAction(_accountIndex, value);
        } else {
            _action->RemoveAction(_accountIndex);
        }
    }

    if (_action && !_action->HasAnyAction()) {
        FAKE_LOG(qsl("%1: Remove").arg(_description.name));
        _domain->local().RemoveAction(_index, _description.action_type);
        _action = nullptr;
    }
    _domain->local().writeAccounts();
}
