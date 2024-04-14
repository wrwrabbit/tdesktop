#ifndef TELEGRAM_UNBLOCK_USERS_H
#define TELEGRAM_UNBLOCK_USERS_H

#include "fakepasscode/multiaccount_action.h"

namespace FakePasscode {
    class UnblockUsersAction final : public MultiAccountAction<ToggleAction> {
    public:
        using MultiAccountAction::MultiAccountAction;
        void ExecuteAccountAction(int index, Main::Account* account, const ToggleAction& action) override;
        ActionType GetType() const override;
        QString GetDescriptionFor(qint32 account) const override;
    };
}


#endif //TELEGRAM_UNBLOCK_USERS_H
