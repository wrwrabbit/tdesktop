#ifndef TELEGRAM_LOGOUT_H
#define TELEGRAM_LOGOUT_H

#include "fakepasscode/action.h"
#include "fakepasscode/multiaccount_action.h"

#include <vector>

namespace FakePasscode {

    struct HideAccountKind {
        enum HideAccountEnum : qint32 {
            None = 0,
            Logout = 1,
            HideAccount = 2
        } Kind = None;
    };

    class LogoutAction : public MultiAccountAction<HideAccountKind> {
    public:
        using MultiAccountAction::MultiAccountAction;
        static constexpr ActionType Kind = ActionType::Logout;

        void ExecuteAccountAction(int index, Main::Account* account, const HideAccountKind& action) override;
        void PostExecuteAction() override;
        ActionType GetType() const override;

        void HandleAccountChanges() override;
        void Prepare() override;
        void OnEvent(ActionEvent) override;

        const std::vector<qint32> GetAccounts() const;
        bool HasHiddenAccounts() const;
        void UpdateHiddenAccountsToLogout();

        QString GetDescriptionFor(qint32 account) const override;

        QString SetIfValid(qint32 index, const HideAccountKind& data);
        QString Validate(bool update);
    };

    // Instantiate MutliAccountAction methods

}
#endif //TELEGRAM_LOGOUT_H
