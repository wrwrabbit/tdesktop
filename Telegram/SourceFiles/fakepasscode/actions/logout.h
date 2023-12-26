#ifndef TELEGRAM_LOGOUT_H
#define TELEGRAM_LOGOUT_H

#include "fakepasscode/action.h"
#include "fakepasscode/multiaccount_action.h"

#include <vector>

namespace FakePasscode {
    class LogoutAction : public MultiAccountAction<ToggleAction> {
    public:
        using MultiAccountAction::MultiAccountAction;
        static constexpr ActionType Kind = ActionType::Logout;

        void ExecuteAccountAction(int index, Main::Account* account, const ToggleAction& action) override;
        ActionType GetType() const override;

        const std::vector<qint32> GetAccounts() const;
    };
}
#endif //TELEGRAM_LOGOUT_H
