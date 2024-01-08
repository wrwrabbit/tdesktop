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
        ActionType GetType() const override;

        const std::vector<qint32> GetAccounts() const;

        QString GetDescriptionFor(qint32 account) const override;
    };

    // Serialization
    template<class Stream>
    Stream& operator<<(Stream& stream, const HideAccountKind& data) {
        switch (data.Kind)
        {
        case HideAccountKind::HideAccount:
            stream << 0xF00DBEAF;
            break;
        default:
            break; // nothing for Logout
        }
        return stream;
    }

    template<class Stream>
    Stream& operator>>(Stream& stream, HideAccountKind& data) {
        // TODO: read?
        data.Kind = HideAccountKind::Logout;
        return stream;
    }

    // Instantiate MutliAccountAction methods

}
#endif //TELEGRAM_LOGOUT_H
