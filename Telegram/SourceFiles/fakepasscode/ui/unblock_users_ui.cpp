#include "unblock_users_ui.h"

#include "lang/lang_keys.h"

static auto description = MultiAccountToggleUi::Description{
    .name = qsl("UnblockUsersUI"),
    .action_type = FakePasscode::ActionType::UnblockUsers,
    .title = tr::lng_unblock_users
};

UnblockUsersUI::UnblockUsersUI(QWidget* parent, gsl::not_null<Main::Domain*> domain, size_t index, int accountIndex)
    : MultiAccountToggleUi(parent, domain, index, accountIndex, description) {}
