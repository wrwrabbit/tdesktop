#include "unblock_users.h"

#include "lang/lang_keys.h"
#include "fakepasscode/log/fake_log.h"
#include "fakepasscode/mtp_holder/crit_api.h"

#include "main/main_account.h"
#include "main/main_session.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_stories.h"
#include "history/history.h"
#include "apiwrap.h"
#include "api/api_blocked_peers.h"

using namespace FakePasscode;

void UnblockUsersAction::ExecuteAccountAction(int index, Main::Account* account, const ToggleAction&) {
    FAKE_LOG(qsl("Executing UnblockUsersAction on account %1.").arg(index));

    auto& data_session = account->maybeSession()->data();
    auto& api = account->session().api();
    auto& blockedPeers = api.blockedPeers();

    auto subscription = blockedPeers.slice()
        | rpl::on_next([&](const Api::BlockedPeers::Slice& slice) {
        for (const auto& item : slice.list) {
            auto userId = item.id;

            auto peer = data_session.peer(userId);
            api.blockedPeers().unblock(peer);

        }
    });
}

ActionType UnblockUsersAction::GetType() const {
    return ActionType::UnblockUsers;
}

QString UnblockUsersAction::GetDescriptionFor(qint32 account) const {
    if (HasAction(account)) {
        return tr::lng_unblock_users(tr::now);
    }
    return QString();
}
