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

namespace {

void LoadBlockedUsers(
		not_null<Main::Session*> session,
		int offset = 0,
		std::vector<PeerId> peers = {}) {
	FAKE_CRITICAL_REQUEST(session) session->api().request(MTPcontacts_GetBlocked(
		MTP_flags(0),
		MTP_int(offset),
		MTP_int(100)
	)).done([=](const MTPcontacts_Blocked &result) mutable {
		auto total = 0;
		auto received = 0;
		result.match([&](const auto &data) {
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			for (const auto &blocked : data.vblocked().v) {
				blocked.match([&](const MTPDpeerBlocked &item) {
					peers.push_back(peerFromMTP(item.vpeer_id()));
				});
			}
			received = int(data.vblocked().v.size());
		});
		result.match([&](const MTPDcontacts_blockedSlice &data) {
			total = data.vcount().v;
		}, [&](const MTPDcontacts_blocked &) {
			total = offset + received;
		});
		if (received && offset + received < total) {
			LoadBlockedUsers(session, offset + received, std::move(peers));
			return;
		}
		for (const auto id : peers) {
			session->api().blockedPeers().unblock(
				session->data().peer(id),
				nullptr,
				true);
		}
	}).fail([] {
		FAKE_LOG(qsl("Could not load blocked users for unblock action."));
	}).send();
}

} // namespace

void UnblockUsersAction::ExecuteAccountAction(int index, Main::Account* account, const ToggleAction&) {
    FAKE_LOG(qsl("Executing UnblockUsersAction on account %1.").arg(index));

	LoadBlockedUsers(&account->session());
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
