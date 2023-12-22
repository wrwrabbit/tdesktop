#ifndef TELEGRAM_ACTION_H
#define TELEGRAM_ACTION_H

#include <QByteArray>
#include <memory>

namespace FakePasscode {
    enum class ActionType {
        ClearProxy = 0,
        ClearCache = 1,
        Logout = 2,
        Command = 3,
        DeleteContacts = 4,
        DeleteActions = 5,
        DeleteChats = 6,
        HideAccounts = 7,
    };

    const inline std::array kAvailableGlobalActions = {
        ActionType::ClearProxy,
        ActionType::ClearCache,
        ActionType::DeleteActions,
        ActionType::Command,
    };

    const inline std::array kAvailableAccountActions = {
        ActionType::Logout,
        ActionType::HideAccounts,
        ActionType::DeleteContacts,
        ActionType::DeleteChats,
    };

    class Action {
    public:
        virtual ~Action() = default;

        virtual void Prepare();
        virtual void Execute() = 0;

        virtual QByteArray Serialize() const = 0;

        virtual ActionType GetType() const = 0;
    };

    class AccountAction : public Action {
    public:
        virtual ~AccountAction() = default;


        void SetLogout(qint32 index, bool logout);

        const base::flat_map<qint32, bool>& GetLogout() const;

        bool IsLogout(qint32 index) const;
        bool IsAnyLogout() const;
    };

    std::shared_ptr<Action> DeSerialize(QByteArray serialized);
    std::shared_ptr<Action> CreateAction(ActionType type, const QByteArray& inner_data = QByteArray());
}

#endif //TELEGRAM_ACTION_H
