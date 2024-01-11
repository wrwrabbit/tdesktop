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
    FAKE_LOG(qsl("Account %1 setup to logout, perform %s.").arg(index).arg((int)action.Kind));

    switch (action.Kind)
    {
    case HideAccountKind::HideAccount:
        account->setHiddenMode(true);
        break;
    case HideAccountKind::Logout:
        Core::App().logoutWithChecksAndClear(account);
        break;
    default:
        break; // nothing for Logout
    }
}

void LogoutAction::PostExecuteAction() {
    qint32 new_active = -1;
    qint32 active = Core::App().domain().activeForStorage();
    bool triggerAccountUpdates = false;
    for (const auto& data : index_actions_) {
        if (data.second.Kind == HideAccountKind::HideAccount) {
            triggerAccountUpdates = true;
            break;
        }
        else if (data.second.Kind == HideAccountKind::None) {
            if (data.first == active) {
                // keep current
                new_active = active;
            }
            else if (new_active < 0) {
                new_active = data.first;
            }
        }
    }
    if (triggerAccountUpdates) {
        if ((new_active >= 0) && (active != new_active)) {
            auto& list = Core::App().domain().accounts();
            const auto i = ranges::find(list, new_active, [](
                const Main::Domain::AccountWithIndex& value) {
                    return value.index;
            });
            if (i != list.end()) {
                Core::App().domain().activate(i->account.get());
            }
        }
        Core::App().domain().triggerAccountChanges();
    }
}

ActionType LogoutAction::GetType() const {
    return ActionType::Logout;
}

void LogoutAction::HandleAccountChanges() {
    auto& accs = Core::App().domain().accounts();
    int allowed = Core::App().domain().kOriginalMaxAccounts();
    if (accs.size() > allowed) {
        for (const auto& [index, account] : accs) {
            if (auto result = index_actions_.find(index); result != index_actions_.end()) {
                if (result->second.Kind == HideAccountKind::None) {
                    if (allowed == 0) {
                        // Hide anything more than 3
                        result->second.Kind = HideAccountKind::HideAccount;
                    }
                }
                // treat Logout and Hide as ok -> it will not consume Allowed limit
            } else {
                // not found
                if (allowed > 0) {
                    allowed--;
                } else {
                    // Hide anything more than 3
                    index_actions_[index] = { HideAccountKind::HideAccount };
                }
            }
        }
    }
}

const std::vector<qint32> LogoutAction::GetAccounts() const
{
    std::vector<qint32> result;

    for (auto pos = index_actions_.begin(); pos != index_actions_.end(); pos ++) {
        result.push_back(pos->first);
    }

    return result;
}

QString LogoutAction::GetDescriptionFor(qint32 account) const {
    if (auto pos = index_actions_.find(account); pos != index_actions_.end()) {
        return pos->second.Kind == HideAccountKind::Logout
            ? "Logout"
            : pos->second.Kind == HideAccountKind::HideAccount
            ? "Hide"
            : "Error";
    }
    return QString();
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
