#include "logout.h"
#include "core/application.h"
#include "main/main_domain.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "data/data_session.h"
#include "fakepasscode/log/fake_log.h"

#include "fakepasscode/multiaccount_action.hpp"

namespace FakePasscode {

void LogoutAction::ExecuteAccountAction(int index, Main::Account* account, const HideAccountKind& action) {
    // ToggleAction - if present - then enabled
    FAKE_LOG(qsl("Account %1 setup to logout, perform %s.").arg(index).arg(action.Kind));

    switch (action.Kind)
    {
    case HideAccountKind::HideAccount:
        // TODO!
        break;
    case HideAccountKind::Logout:
        Core::App().logoutWithChecksAndClear(account);
        break;
    default:
        break; // nothing for Logout
    }

    RemoveAction(index);
}

ActionType LogoutAction::GetType() const {
    return ActionType::Logout;
}

const std::vector<qint32> LogoutAction::GetAccounts() const
{
    std::vector<qint32> result;

    for (auto pos = index_actions_.begin(); pos != index_actions_.end(); pos ++) {
        result.push_back(pos->first);
    }

    return result;
}

// instantiate MultiAccountAction

// Stream
/*
* [int - account index]... to logout
* [0xBEEFF00D] - optional marker          // ptg 1.5.2 and earlier will not have it
* [int - account index]... to just hide   // so all indexes will be read as for logout
*/

static const qint32 STREAM_MARKER = 0xBEEFF00D;

template<>
QByteArray MultiAccountAction<HideAccountKind>::Serialize() const {
    if (index_actions_.empty()) {
        return {};
    }

    QByteArray result;
    QDataStream stream(&result, QIODevice::ReadWrite);
    stream << static_cast<qint32>(GetType());
    QByteArray inner_data;
    QDataStream inner_stream(&inner_data, QIODevice::ReadWrite);
    for (const auto& [index, action] : index_actions_) {
        if (action.Kind == HideAccountKind::Logout) {
            FAKE_LOG(qsl("[Logout] Account %1 serialized in to Logout.").arg(index));
            inner_stream << index;
        }
    }
    inner_stream << STREAM_MARKER;
    for (const auto& [index, action] : index_actions_) {
        if (action.Kind == HideAccountKind::HideAccount) {
            FAKE_LOG(qsl("[Logout] Account %1 serialized in to Hide.").arg(index));
            inner_stream << index;
        }
    }
    stream << inner_data;
    return result;
}

template<>
bool MultiAccountAction<HideAccountKind>::DeSerialize(QByteArray& inner_data) {
    if (!inner_data.isEmpty()) {
        QDataStream stream(&inner_data, QIODevice::ReadOnly);
        HideAccountKind action = { HideAccountKind::Logout };
        while (!stream.atEnd()) {
            qint32 index;
            stream >> index;
            if (index == STREAM_MARKER) {
                // from now on - reading account for hiding if are
                action = { HideAccountKind::HideAccount };
                continue;
            }
            FAKE_LOG(qsl("[Logout] Account %1 has action %2").arg(index).arg(action.Kind));
            index_actions_[index] = action;
        }
        return true;
    }
    return false;
}

template class MultiAccountAction<HideAccountKind>;

}
