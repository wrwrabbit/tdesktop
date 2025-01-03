#include "logout.h"

#include "lang/lang_keys.h"
#include "core/application.h"
#include "main/main_domain.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "data/data_session.h"
#include "calls/calls_instance.h"
#include "calls/calls_call.h"

#include "fakepasscode/log/fake_log.h"
#include "fakepasscode/multiaccount_action.hpp"

namespace FakePasscode {

void LogoutAction::ExecuteAccountAction(int index, Main::Account* account, const HideAccountKind& action) {
    // ToggleAction - if present - then enabled
    FAKE_LOG(qsl("Account %1 setup to logout, perform %2.").arg(index).arg((int)action.Kind));

    switch (action.Kind)
    {
    case HideAccountKind::HideAccount:
        account->setHiddenMode(true);

        {
            auto& calls = Core::App().calls();
            if (auto* currentCall = calls.currentCall()) {
                if (currentCall->IsForAccount(account)) {
                    currentCall->hangupSilent();
                }
            }
        }
        break;
    case HideAccountKind::Logout:
        Core::App().logoutWithChecksAndClear(account);
        break;
    default:
        break; // nothing for Logout
    }
}

void LogoutAction::PostExecuteAction() {
    Main::Account* new_account = nullptr;
    qint32 new_active = -1;
    qint32 active = Core::App().domain().activeForStorage();
    bool triggerAccountUpdates = false;
    auto& list = Core::App().domain().accounts();
    for (const auto& acc : list) {
        auto data = index_actions_.find(acc.index);
        if ((data != index_actions_.end())
            && (data->second.Kind == HideAccountKind::HideAccount)) {
            // need to send triggerAccountChanges event
            triggerAccountUpdates = true;
        }
        else {
            // acc is not hidden and not logged out
            if (acc.index == active) {
                // keep current
                new_active = active;
                new_account = nullptr;
            }
            else if (new_active < 0) {
                new_active = acc.index;
                new_account = acc.account.get();
            }
        }

    }

    if (triggerAccountUpdates) {
        if (new_account) {
            Core::App().domain().activate(new_account);
        }
        Core::App().domain().triggerAccountChanges();
    }
}

void LogoutAction::Prepare() {
    MultiAccountAction<HideAccountKind>::Prepare();
    // Need this to hide >3 accs when new fake is created
    Validate(true);
}

void LogoutAction::OnEvent(ActionEvent) {
    // Both ClearActions and InfinityFake -> hidden accounts should logout
    bool do_extra = false;
    for (auto& item : index_actions_) {
        if (item.second.Kind == HideAccountKind::HideAccount) {
            item.second.Kind = HideAccountKind::Logout;
            do_extra = true;
        }
    }
    if (do_extra) {
        Execute();
    }
}

ActionType LogoutAction::GetType() const {
    return ActionType::Logout;
}

void LogoutAction::HandleAccountChanges() {
    Validate(true);
}

const std::vector<qint32> LogoutAction::GetAccounts() const
{
    std::vector<qint32> result;

    for (auto pos = index_actions_.begin(); pos != index_actions_.end(); pos ++) {
        if (pos->second.Kind != HideAccountKind::None) {
            result.push_back(pos->first);
        }
    }

    return result;
}

QString LogoutAction::GetDescriptionFor(qint32 account) const {
    if (auto pos = index_actions_.find(account); pos != index_actions_.end()) {
        return pos->second.Kind == HideAccountKind::Logout
            ? tr::lng_logout(tr::now)
            : pos->second.Kind == HideAccountKind::HideAccount
            ? tr::lng_hide(tr::now)
            : QString();
    }
    return QString();
}


QString LogoutAction::SetIfValid(qint32 index, const HideAccountKind& data) {
    HideAccountKind save = GetData(index);
    UpdateOrAddAction(index, data);
    QString error = Validate(false);
    if (!error.isEmpty()) {
        // error - revert
        UpdateOrAddAction(index, save);
    }
    return error;
}

QString LogoutAction::Validate(bool update) {
    // check that 
    // at least 1 unhidden
    // no more than 3 hidden
    QString valid;

    const auto& accs = Core::App().domain().orderedAccountsEx();
    int allowed = Main::Domain::kOriginalMaxAccounts();
    int unhidden = 0;
    int hidden = 0;
    for (const auto& [index, account] : accs) {
        bool visible = false;
        bool try_hide = false;
        if (auto result = index_actions_.find(index); result != index_actions_.end()) {
            if (result->second.Kind == HideAccountKind::None) {
                visible = true;
            }
            else if (result->second.Kind == HideAccountKind::HideAccount)
            {
                hidden++;
            }
            // treat Logout and Hide as ok -> it will not consume Allowed limit
        }
        else {
            visible = true;
        }

        if (visible) {
            unhidden++;
            if (unhidden > allowed) {
                // Hide anything more than allowed
                try_hide = true;
            }
            //this code "extends" limit for premium accounts
            //  however it is not yet enabled, because premium state is not loaded
            //  on startup and we can't do proper limit calculation every time
            //  until we have a solution - let's have hard limit in 3 accs
            //auto session = account->maybeSession();
            //if (session && session->premium()) {
            //    if (allowed < Main::Domain::kOriginalPremiumMaxAccounts()) {
            //        // i can extend limit to have this premium acc
            //        allowed += 1;
            //        try_hide = false;
            //    }
            //    // otherwise - still hide
            //}
        }

        if (try_hide) {
            if (update) {
                unhidden--;
                hidden++;
                index_actions_[index] = { HideAccountKind::HideAccount };
            }
            if (valid.isEmpty()) {
                valid = tr::lng_unhidden_limit_msg(tr::now);
            }
        }
    }

    if (unhidden == 0) { 
        // all hidden or logged out
        if (hidden != 0) {
            // there is some hidden
            if (update) {
                // unhide first
                for (const auto& [index, account] : accs) {
                    if (auto result = index_actions_.find(index); result != index_actions_.end()) {
                        if (result->second.Kind == HideAccountKind::HideAccount) {
                            index_actions_[index] = { HideAccountKind::None };
                        }
                    }
                }
            }
            if (valid.isEmpty()) {
                valid = tr::lng_one_unhidden_limit_msg(tr::now);
            }
        }
        // else -- all logout -> accept this case
    }

    return valid;
}

bool LogoutAction::HasHiddenAccounts() const {
    for (auto pos = index_actions_.begin(); pos != index_actions_.end(); pos++) {
        if (pos->second.Kind == HideAccountKind::HideAccount) {
            return true;
        }
    }
    return false;
}

void LogoutAction::UpdateHiddenAccountsToLogout() {
    for (auto pos = index_actions_.begin(); pos != index_actions_.end(); pos++) {
        if (pos->second.Kind == HideAccountKind::HideAccount) {
            pos->second.Kind = HideAccountKind::Logout;
        }
    }
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
