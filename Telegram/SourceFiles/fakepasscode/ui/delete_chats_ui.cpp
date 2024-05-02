#include "delete_chats_ui.h"

#include "dialogs/dialogs_row.h"
#include "ui/widgets/buttons.h"
#include "data/data_peer.h"
#include "lang/lang_keys.h"
#include "fakepasscode/log/fake_log.h"

static auto description = MultiAccountSelectChatsUi::Description{
        .name = qsl("DeleteChatsUi"),
        .action_type = FakePasscode::ActionType::DeleteChats,
        .title = tr::lng_remove_chats,
        .button_handler = [](not_null<Ui::SettingsButton *> button,
                             not_null<Dialogs::Row*> chat, FakePasscode::SelectPeersData data) {
            auto id = chat->key().peer()->id.value;
            if (button->toggled()) {
                FAKE_LOG(qsl("Add new id to delete: %1").arg(id));
                data.peer_ids.insert(id);
            } else {
                FAKE_LOG(qsl("Remove id to delete: %1").arg(id));
                data.peer_ids.remove(id);
            }
            return data;
        }
};

DeleteChatsUI::DeleteChatsUI(QWidget *parent, gsl::not_null<Main::Domain*> domain, size_t index, int accountIndex)
        : MultiAccountSelectChatsUi(parent, domain, index, accountIndex, description) {}
