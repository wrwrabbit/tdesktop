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
        UnblockUsers = 7,
    };

    struct ActionUIRecord {
        ActionType Type;
        bool HasDivider;
    };

    const inline std::array<ActionUIRecord, 4> kAvailableGlobalActions = {{
        { ActionType::ClearProxy, true },
        { ActionType::ClearCache, false },
        { ActionType::DeleteActions, false },
        { ActionType::Command, true }
    }};

    const inline std::array kAvailableAccountActions = {
        ActionType::DeleteContacts,
        ActionType::Logout,
        ActionType::UnblockUsers,
        ActionType::DeleteChats,
    };

    enum class ActionEvent {
        SwitchToInfinityFake,
        ClearActions
    };

    class Action {
    public:
        virtual ~Action() = default;

        virtual void Prepare();
        virtual void Execute() = 0;

        virtual QByteArray Serialize() const = 0;

        virtual ActionType GetType() const = 0;

        virtual void OnEvent(ActionEvent) {};
    };

    class AccountAction : public Action {
    public:
        virtual ~AccountAction() = default;

        virtual QString GetDescriptionFor(qint32 account) const = 0;
    };

    std::shared_ptr<Action> DeSerialize(QByteArray serialized);
    std::shared_ptr<Action> CreateAction(ActionType type, const QByteArray& inner_data = QByteArray());
}

#endif //TELEGRAM_ACTION_H
