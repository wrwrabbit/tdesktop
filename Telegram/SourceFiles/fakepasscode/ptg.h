#ifndef TELEGRAM_FAKE_PASSCODE_PTG_H
#define TELEGRAM_FAKE_PASSCODE_PTG_H
#pragma once

#include <memory>

namespace PTG {

    class Passcode {
        int _index;
    public:
        Passcode(int index)
            :_index(index)
        {}

        bool HasHiddenAccounts();
        void SetHidden2Logout();
    };

    std::shared_ptr<Passcode> GetPasscode(int index);

    rpl::producer<> GetFakePasscodeUpdates();
    void FireFakePasscodeUpdates();

    bool IsFakeActive();

}

#endif // TELEGRAM_FAKE_PASSCODE_PTG_H
