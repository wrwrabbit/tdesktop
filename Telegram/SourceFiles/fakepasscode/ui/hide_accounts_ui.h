#ifndef TELEGRAM_HIDE_ACCOUNTS_UI_H
#define TELEGRAM_HIDE_ACCOUNTS_UI_H

#include "action_ui.h"
#include "fakepasscode/actions/hide_accounts.h"
#include "settings/settings_common.h"

class HideAccountsUI : public ActionUI {
public:
    HideAccountsUI(QWidget* parent, gsl::not_null<Main::Domain*> domain, size_t index);

    void Create(not_null<Ui::VerticalLayout*> content,
                Window::SessionController* controller = nullptr) override;

private:
    FakePasscode::HideAccountsAction* _hide;
    std::vector<Settings::Button*> account_buttons_;
};

#endif //TELEGRAM_HIDE_ACCOUNTS_UI_H
