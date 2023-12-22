#ifndef TELEGRAM_HIDE_ACCOUNT_H
#define TELEGRAM_HIDE_ACCOUNT_H

#include "fakepasscode/action.h"

#include "base/flat_map.h"

namespace FakePasscode {
    class HideAccountAction : public Action {
    public:
        static constexpr ActionType Kind = ActionType::HideAccounts;

        HideAccountAction() = default;
        explicit HideAccountAction(QByteArray inner_data);
        HideAccountAction(base::flat_map<qint32, bool> hide_accounts);

        void Execute() override;

        QByteArray Serialize() const override;

        ActionType GetType() const override;

        void SetHidden(qint32 index, bool hide);

        const base::flat_map<qint32, bool>& GetHidden() const;

        bool IsHidden(qint32 index) const;

        void SubscribeOnLoggingOut();

        void Prepare() override;

    private:
        base::flat_map<qint32, bool> index_to_hide_;

        rpl::lifetime lifetime_;
        rpl::lifetime sub_lifetime_;

        void SubscribeOnAccountsChanges();
    };
}
#endif //TELEGRAM_HIDE_ACCOUNT_H
