#ifndef TELEGRAM_MULTIACCOUNT_CHATS_UI_H
#define TELEGRAM_MULTIACCOUNT_CHATS_UI_H

#include "action_ui.h"
#include "fakepasscode/actions/delete_chats.h"

namespace Ui {
    class SettingsButton;
    class FlatLabel;
}

namespace Dialogs {
class Row;
}

class SelectChatsContent;

class MultiAccountSelectChatsUi : public ActionUI {
public:
    using ButtonHandler = std::function<FakePasscode::SelectPeersData(
            not_null<Ui::SettingsButton *>, not_null<Dialogs::Row*>, FakePasscode::SelectPeersData)>;

    struct Description {
        QString name;
        FakePasscode::ActionType action_type;
        std::function<rpl::producer<QString>()> title;
        ButtonHandler button_handler;
    };

    MultiAccountSelectChatsUi(QWidget* parent, gsl::not_null<Main::Domain*> domain, size_t index, int accountIndex,
                              Description description);

    void Create(not_null<Ui::VerticalLayout*> content,
                Window::SessionController* controller = nullptr) override;
private:
    using Action = FakePasscode::MultiAccountAction<FakePasscode::SelectPeersData>;
    Description _description;
    Action* _action = nullptr;
    int _accountIndex;
    std::vector<Ui::SettingsButton*> buttons_;
    std::vector<Ui::FlatLabel*> labels_;
};


#endif //TELEGRAM_MULTIACCOUNT_CHATS_UI_H
