#include "ptg.h"

#include "core/application.h"
#include "main/main_session.h"
#include "main/main_domain.h"
#include "storage/storage_domain.h"

#include "action.h"
#include "actions/logout.h"

namespace PTG {

    bool Passcode::HasHiddenAccounts() {
        auto action = Core::App().domain().local().GetAction(_index, FakePasscode::ActionType::Logout);
        if (action) {
            FakePasscode::LogoutAction* logout = (FakePasscode::LogoutAction*)action;
            return logout->HasHiddenAccounts();
        }
        return true;
    }

    void Passcode::SetHidden2Logout() {
        auto action = Core::App().domain().local().GetAction(_index, FakePasscode::ActionType::Logout);
        if (action) {
            FakePasscode::LogoutAction* logout = (FakePasscode::LogoutAction*)action;
            logout->UpdateHiddenAccountsToLogout();
            FireFakePasscodeUpdates();
        }
    }

    std::shared_ptr<Passcode> GetPasscode(int index) {
        return std::shared_ptr<Passcode>(new Passcode(index));
    }

    rpl::event_stream<> FakePasscodeUpdated;
    rpl::producer<> GetFakePasscodeUpdates() {
        return FakePasscodeUpdated.events();
    }
    void FireFakePasscodeUpdates() {
        return FakePasscodeUpdated.fire({});
    }

    bool IsFakeActive() {
        return Core::App().domain().local().IsFake();
    }

};

