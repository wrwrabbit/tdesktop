#include "multiaccount_chats_ui.h"

#include "fakepasscode/log/fake_log.h"

#include "core/application.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_account.h"
#include "data/data_user.h"
#include "storage/storage_domain.h"
#include "settings/settings_common.h"
#include "ui/widgets/buttons.h"
#include "lang/lang_keys.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/vertical_list.h"
#include "styles/style_boxes.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "boxes/abstract_box.h"
#include "ui/text/text_utilities.h"
#include "data/data_session.h"
#include "data/data_folder.h"
#include "history/history.h"
#include "window/window_session_controller.h"

#include "data/data_cloud_file.h"
#include "dialogs/dialogs_row.h"
#include "dialogs/dialogs_entry.h"
#include "dialogs/ui/dialogs_layout.h"
#include "ui/painter.h"

#include "fakepasscode/actions/delete_chats.h"
#include "styles/style_menu_icons.h"
#include "styles/style_dialogs.h"

using Action = FakePasscode::MultiAccountAction<FakePasscode::SelectPeersData>;

using ButtonHandler = MultiAccountSelectChatsUi::ButtonHandler;

class SelectChatsContentBox : public Ui::BoxContent {
public:
    SelectChatsContentBox(QWidget* parent,
                          Main::Domain* domain, Action* action,
                          qint64 index,
                          MultiAccountSelectChatsUi::Description* description);

protected:
    void prepare() override;

private:
    Main::Domain* domain_;
    Action* action_;
    qint64 index_;
    MultiAccountSelectChatsUi::Description* description_;
};

SelectChatsContentBox::SelectChatsContentBox(QWidget *,
                                             Main::Domain* domain, Action* action,
                                             qint64 index,
                                             MultiAccountSelectChatsUi::Description* description)
        : domain_(domain)
        , action_(action)
        , index_(index)
        , description_(description) {
}

class SelectChatsContent : public Ui::RpWidget {
public:
    SelectChatsContent(QWidget *parent,
                       Main::Domain* domain, Action* action,
                       SelectChatsContentBox*, qint64 index,
                       MultiAccountSelectChatsUi::Description* description,
                       FakePasscode::SelectPeersData data = {});

    void setupContent();

private:
    Main::Domain* domain_;
    Action* action_;
    std::vector<Ui::SettingsButton*> buttons_;
    qint64 index_;
    MultiAccountSelectChatsUi::Description* description_;
    FakePasscode::SelectPeersData data_;
};

SelectChatsContent::SelectChatsContent(QWidget *parent,
                                       Main::Domain* domain, Action* action,
                                       SelectChatsContentBox*, qint64 index,
                                       MultiAccountSelectChatsUi::Description* description,
                                       FakePasscode::SelectPeersData data)
        : Ui::RpWidget(parent)
        , domain_(domain)
        , action_(action)
        , index_(index)
        , description_(description)
        , data_(std::move(data)) {
}

void SelectChatsContentBox::prepare() {
    using namespace Settings;
    addButton(tr::lng_close(), [=] { closeBox(); });
    const auto content =
            setInnerWidget(object_ptr<SelectChatsContent>(this, domain_, action_, this, index_, description_,
                                                          action_->GetData(index_)),
                           st::sessionsScroll);
    content->resize(st::boxWideWidth, st::noContactsHeight);
    content->setupContent();
    setDimensions(st::boxWideWidth, st::sessionsHeight);
}

void AddDialogImageToButton(
    not_null<Ui::AbstractButton*> button,
    const style::SettingsButton& st,
    not_null<Dialogs::Row*> dialog) {

    struct IconWidget {
        IconWidget(QWidget* parent, Dialogs::Row* dialog)
            : widget(parent)
            , dialog(std::move(dialog)) {
        }
        Ui::RpWidget widget;
        Dialogs::Row* dialog;
    };
    const auto icon = button->lifetime().make_state<IconWidget>(
        button,
        std::move(dialog));
    icon->widget.setAttribute(Qt::WA_TransparentForMouseEvents);
    icon->widget.resize(st::menuIconLock.size()); // use size from icon
    button->sizeValue(
    ) | rpl::start_with_next([=, left = st.iconLeft](QSize size) {
        icon->widget.moveToLeft(
            left,
            (size.height() - icon->widget.height()) / 2,
            size.width());
        }, icon->widget.lifetime());
    icon->widget.paintRequest(
    ) | rpl::start_with_next([=] {
        auto iconStyle = style::DialogRow{
            .height = icon->widget.height(),
            .padding = style::margins(0, 0, 0, 0),
            .photoSize = icon->widget.height(),
        };
        auto p = Painter(&icon->widget);
        icon->dialog->entry()->paintUserpic(p, icon->dialog->userpicView(), {
            .st = &iconStyle,
            .currentBg = st::windowBg,
            .width = icon->widget.width(),
            });
        }, icon->widget.lifetime());
}



void SelectChatsContent::setupContent() {
    using ChatWithName = std::pair<not_null<const Dialogs::MainList*>, rpl::producer<QString>>;

    const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
    Ui::AddSubsectionTitle(content, description_->popup_window_title());

    const auto& accounts = domain_->accounts();
    Main::Account* cur_account = nullptr;
    for (const auto&[index, account]: accounts) {
        if (index == index_) {
            cur_account = account.get();
        }
    }
    if (cur_account == nullptr) {
        return;
    }
    const auto& account_data = cur_account->session().data();

    std::vector<ChatWithName> chat_lists;
    if (auto archive_folder = account_data.folderLoaded(Data::Folder::kId)) {
        chat_lists.emplace_back(account_data.chatsList(archive_folder), tr::lng_chats_action_archive());
    }
    chat_lists.emplace_back(account_data.chatsList(), tr::lng_chats_action_main_chats());
    for (const auto&[list, name] : chat_lists) {
        Ui::AddSubsectionTitle(content, name);
        for (auto chat: list->indexed()->all()) {
            if (chat->entry()->fixedOnTopIndex() == Dialogs::Entry::kArchiveFixOnTopIndex) {
                continue; // Archive, skip
            }

            const auto& chat_name = chat->history()->peer->isSelf() ? tr::lng_saved_messages(tr::now) : chat->entry()->chatListName();
            auto button = Settings::AddButtonWithIcon(content, rpl::single(chat_name), st::settingsButton);
            AddDialogImageToButton(button, st::settingsButton, chat);
            auto dialog_id = chat->key().peer()->id.value;
            button->toggleOn(rpl::single(data_.peer_ids.contains(dialog_id)));
            button->addClickHandler([this, chat, button] {
                data_ = description_->button_handler(button, chat, std::move(data_));
                action_->UpdateOrAddAction(index_, data_);
                domain_->local().writeAccounts();
            });
            buttons_.push_back(button);
        }
    }

    Ui::ResizeFitChild(this, content);
}

MultiAccountSelectChatsUi::MultiAccountSelectChatsUi(QWidget *parent, gsl::not_null<Main::Domain*> domain, size_t index, Description description)
        : ActionUI(parent, domain, index)
        , _description(std::move(description)) {
    if (auto* action = domain->local().GetAction(_index, _description.action_type)) {
        _action = dynamic_cast<Action*>(action);
    } else {
        _action = dynamic_cast<Action*>(
                _domain->local().AddAction(_index, _description.action_type));
    }
}

void MultiAccountSelectChatsUi::Create(not_null<Ui::VerticalLayout *> content,
                                       Window::SessionController* controller) {
    Expects(controller != nullptr);
    Ui::AddSubsectionTitle(content, _description.title());
    const auto& accounts = Core::App().domain().accounts();
    for (const auto&[index, account] : accounts) {
        Settings::AddButtonWithIcon(
                content,
                _description.account_title(account.get()),
                st::settingsButton,
                {&st::menuIconChannel}
        )->addClickHandler([index = index, controller, this] {
            if (!_action->HasAction(index)) {
                _action->AddAction(index, FakePasscode::SelectPeersData{});
            }

            _domain->local().writeAccounts();
            controller->show(Box<SelectChatsContentBox>(_domain, _action, index, &_description));
        });
    }
}

rpl::producer<QString> MultiAccountSelectChatsUi::DefaultAccountNameFormat(gsl::not_null<Main::Account*> account) {
    auto user = account->session().user();
    return rpl::single(user->firstName + " " + user->lastName);
}

