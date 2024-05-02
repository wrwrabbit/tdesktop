#ifndef TELEGRAM_UNBLOCK_USERS_UI_H
#define TELEGRAM_UNBLOCK_USERS_UI_H

#include "multiaccount_toggle_ui.h"

class UnblockUsersUI final : public MultiAccountToggleUi {
public:
    UnblockUsersUI(QWidget* parent, gsl::not_null<Main::Domain*> domain, size_t index, int accountIndex);
};


#endif //TELEGRAM_UNBLOCK_USERS_UI_H
