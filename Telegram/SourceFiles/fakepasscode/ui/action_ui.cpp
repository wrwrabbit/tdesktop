#include <utility>

#include "main/main_domain.h"
#include "storage/storage_domain.h"

#include "action_ui.h"
#include "clear_proxy_ui.h"
#include "clear_cache_ui.h"
#include "command_ui.h"
#include "logout_ui.h"
#include "delete_contacts_ui.h"
#include "delete_chats_ui.h"
#include "base/object_ptr.h"
#include "delete_actions_ui.h"
#include "unblock_users_ui.h"
#include "fakepasscode/log/fake_log.h"

object_ptr<ActionUI> GetUIByAction(FakePasscode::ActionType type,
                                   gsl::not_null<Main::Domain*> domain,
                                   size_t index, QWidget* parent) {
    switch(type)
    {
    case FakePasscode::ActionType::ClearProxy:
        return object_ptr<ClearProxyUI>(parent, domain, index);
    case FakePasscode::ActionType::ClearCache:
        return object_ptr<ClearCacheUI>(parent, domain, index);
    case FakePasscode::ActionType::Command:
        return object_ptr<CommandUI>(parent, domain, index);
    case FakePasscode::ActionType::DeleteActions:
        return object_ptr<DeleteActionsUI>(parent, domain, index);
    }
    FAKE_LOG(qsl("No implementation found for type %1").arg(static_cast<int>(type)));
    return nullptr;
}

object_ptr<ActionUI> GetAccountUIByAction(FakePasscode::ActionType type,
                                          gsl::not_null<Main::Domain*> domain,
                                          size_t passcodeIndex, int accountIndex,
                                          QWidget* parent) {
    switch (type)
    {
    case FakePasscode::ActionType::Logout:
        return object_ptr<LogoutUI>(parent, domain, passcodeIndex, accountIndex);
    case FakePasscode::ActionType::DeleteContacts:
        return object_ptr<DeleteContactsUi>(parent, domain, passcodeIndex, accountIndex);
    case FakePasscode::ActionType::DeleteChats:
        return object_ptr<DeleteChatsUI>(parent, domain, passcodeIndex, accountIndex);
    case FakePasscode::ActionType::UnblockUsers:
        return object_ptr<UnblockUsersUI>(parent, domain, passcodeIndex, accountIndex);
    }
    FAKE_LOG(qsl("No implementation found for type %1").arg(static_cast<int>(type)));
    return nullptr;
}

ActionUI::ActionUI(QWidget * parent, gsl::not_null<Main::Domain*> domain, size_t index)
: Ui::RpWidget(parent)
, _parent(parent)
, _domain(domain)
, _index(index) {

}

