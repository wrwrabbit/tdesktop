#include "delete_contacts_ui.h"

#include "lang/lang_keys.h"

static auto description = MultiAccountToggleUi::Description{
    .name = qsl("DeleteContactsUi"),
    .action_type = FakePasscode::ActionType::DeleteContacts,
    .title = tr::lng_delete_contacts
};

DeleteContactsUi::DeleteContactsUi(QWidget *parent, gsl::not_null<Main::Domain*> domain, size_t index, int accountIndex)
    : MultiAccountToggleUi(parent, domain, index, accountIndex, description) {}
