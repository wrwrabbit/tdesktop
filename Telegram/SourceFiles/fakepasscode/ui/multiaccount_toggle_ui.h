#ifndef TELEGRAM_MULTIACCOUNT_TOGGLE_UI_H
#define TELEGRAM_MULTIACCOUNT_TOGGLE_UI_H

#include "action_ui.h"
#include "fakepasscode/multiaccount_action.h"

namespace Ui {
    class SettingsButton;
}

class MultiAccountToggleUi : public ActionUI {
public:
    struct Description {
        QString name;
        FakePasscode::ActionType action_type;
        std::function<rpl::producer<QString>()> title;
    };

    MultiAccountToggleUi(QWidget* parent, gsl::not_null<Main::Domain*> domain, size_t index, int accountIndex, Description description);

    void Create(not_null<Ui::VerticalLayout*> content,
                Window::SessionController* controller = nullptr) override;

private:
    void SetActionValue(bool current_active, FakePasscode::ToggleAction value);

    using Action = FakePasscode::MultiAccountAction<FakePasscode::ToggleAction>;
    Description _description;
    Action* _action = nullptr;
    int _accountIndex;
};


#endif //TELEGRAM_MULTIACCOUNT_TOGGLE_UI_H
