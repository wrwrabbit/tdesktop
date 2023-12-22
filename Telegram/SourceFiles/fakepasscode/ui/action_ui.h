#ifndef TELEGRAM_ACTION_UI_H
#define TELEGRAM_ACTION_UI_H

#include <memory>
#include <ui/wrap/vertical_layout_reorder.h>

#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "fakepasscode/fake_passcode.h"
#include "fakepasscode/action.h"
#include "ui/wrap/vertical_layout.h"
#include "main/main_domain.h"

namespace Main {
class Domain;
}

namespace Window {
class SessionController;
}

class ActionUI: public Ui::RpWidget {
public:
    ActionUI(QWidget* parent, gsl::not_null<Main::Domain*> domain, size_t index);

    virtual void Create(not_null<Ui::VerticalLayout*> content,
                        Window::SessionController* controller = nullptr) = 0;

protected:
    QWidget* _parent;
    Main::Domain* _domain;
    size_t _index;
};

template<typename ActionType>
class AccountActionUI : public ActionUI {
public:
    AccountActionUI(QWidget* parent, gsl::not_null<Main::Domain*> domain, size_t passcodeIndex, int accountIndex)
        : ActionUI(parent, domain, passcodeIndex)
        , _action(nullptr)
        , _accountIndex(accountIndex) {
        if (auto* action = domain->local().GetAction(passcodeIndex, ActionType::Kind); action != nullptr) {
            _action = dynamic_cast<ActionType*>(action);
        }
    }

    virtual void Create(not_null<Ui::VerticalLayout*> content,
        Window::SessionController* controller = nullptr) = 0;

protected:
    ActionType* _action;
    int _accountIndex;
};

object_ptr<ActionUI> GetUIByAction(FakePasscode::ActionType type,
                                   gsl::not_null<Main::Domain*> domain, size_t index,
                                   QWidget* parent);

object_ptr<ActionUI> GetAccountUIByAction(FakePasscode::ActionType type,
                                          gsl::not_null<Main::Domain*> domain,
                                          size_t passcodeIndex, int accountIndex,
                                          QWidget* parent);

#endif //TELEGRAM_ACTION_UI_H
