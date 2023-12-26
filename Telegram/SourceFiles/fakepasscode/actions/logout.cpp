#include "logout.h"
#include "core/application.h"
#include "main/main_domain.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "data/data_session.h"
#include "fakepasscode/log/fake_log.h"

#include "fakepasscode/multiaccount_action.hpp"



void FakePasscode::LogoutAction::ExecuteAccountAction(int index, Main::Account* account, const HideAccountKind& action) {
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

FakePasscode::ActionType FakePasscode::LogoutAction::GetType() const {
    return ActionType::Logout;
}

const std::vector<qint32> FakePasscode::LogoutAction::GetAccounts() const
{
    std::vector<qint32> result;

    for (auto pos = index_actions_.begin(); pos != index_actions_.end(); pos ++) {
        result.push_back(pos->first);
    }

    return result;
}

namespace FakePasscode {
    template class MultiAccountAction<HideAccountKind>;
}
