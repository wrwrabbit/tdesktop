#ifndef TELEGRAM_HIDE_ACCOUNTS_H
#define TELEGRAM_HIDE_ACCOUNTS_H

#include "fakepasscode/action.h"

#include "base/flat_map.h"

namespace FakePasscode {
    class HideAccountsAction : public Action {
    public:
        HideAccountsAction() = default;
        explicit HideAccountsAction(QByteArray inner_data);
        HideAccountsAction(base::flat_map<qint32, bool> hidden_accounts);

        void Execute() override;

        QByteArray Serialize() const override;

        ActionType GetType() const override;

        void SetHide(qint32 index, bool hide);

        const base::flat_map<qint32, bool>& GetHide() const;

        bool IsHidden(qint32 index) const;

        bool IsSessionHidden(uint64 sessionId) const;
 
        void SubscribeOnLoggingOut();

        void Prepare() override;

    private:
        base::flat_map<qint32, bool> index_to_hide_;

        rpl::lifetime lifetime_;
        rpl::lifetime sub_lifetime_;

        void SubscribeOnAccountsChanges();
    };
}
#endif //TELEGRAM_HIDE_ACCOUNTS_H
