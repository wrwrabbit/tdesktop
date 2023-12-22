#ifndef TELEGRAM_HIDE_ACCOUNT_UI_H
#define TELEGRAM_HIDE_ACCOUNT_UI_H

#include "action_ui.h"
#include "fakepasscode/actions/hide_account.h"
#include "settings/settings_common.h"

class HideAccountUI : public AccountActionUI<FakePasscode::HideAccountAction> {
public:
    HideAccountUI(QWidget* parent, gsl::not_null<Main::Domain*> domain, size_t passcodeIndex, int accountIndex)
        : AccountActionUI<FakePasscode::HideAccountAction>(parent, domain, passcodeIndex, accountIndex)
    {}

    void Create(not_null<Ui::VerticalLayout*> content,
                Window::SessionController* controller = nullptr) override;
};

#endif //TELEGRAM_HIDE_ACCOUNT_UI_H
