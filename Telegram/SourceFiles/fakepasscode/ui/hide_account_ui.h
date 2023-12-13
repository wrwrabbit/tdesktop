#ifndef TELEGRAM_HIDE_ACCOUNT_UI_H
#define TELEGRAM_HIDE_ACCOUNT_UI_H

#include "action_ui.h"
#include "fakepasscode/actions/hide_account.h"
#include "settings/settings_common.h"

class HideAccountUI : public ActionUI {
public:
    HideAccountUI(QWidget* parent, gsl::not_null<Main::Domain*> domain, size_t index);

    void Create(not_null<Ui::VerticalLayout*> content,
                Window::SessionController* controller = nullptr) override;

    virtual bool IsAccountAction() const override {
        return true;
    };
private:
    FakePasscode::HideAccountAction* _action;
    std::vector<Settings::Button*> account_buttons_;
};

#endif //TELEGRAM_HIDE_ACCOUNT_UI_H
