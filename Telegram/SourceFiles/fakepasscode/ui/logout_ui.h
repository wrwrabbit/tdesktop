#ifndef TELEGRAM_LOGOUT_UI_H
#define TELEGRAM_LOGOUT_UI_H

#include "action_ui.h"
#include "fakepasscode/actions/logout.h"
#include "settings/settings_common.h"

class LogoutUI : public AccountActionUI<FakePasscode::LogoutAction>{
public:
    LogoutUI(QWidget* parent, gsl::not_null<Main::Domain*> domain, size_t passcodeIndex, int accountIndex)
        : AccountActionUI<FakePasscode::LogoutAction>(parent, domain, passcodeIndex, accountIndex)
    {}

    void Create(not_null<Ui::VerticalLayout*> content,
                Window::SessionController* controller = nullptr) override;


};

#endif //TELEGRAM_LOGOUT_UI_H
