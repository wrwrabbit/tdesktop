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
    icon->widget.show();
}

MultiAccountSelectChatsUi::MultiAccountSelectChatsUi(QWidget *parent, gsl::not_null<Main::Domain*> domain, size_t index, int accountIndex, Description description)
        : ActionUI(parent, domain, index)
        , _description(std::move(description))
        , _accountIndex(accountIndex) {
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

    static FakePasscode::SelectPeersData data_;
    data_ = _action->GetData(_accountIndex);

    // --- Add filter box ---
    auto filterField = content->add(
        object_ptr<Ui::InputField>(
            content,
            st::createPollField,
            Ui::InputField::Mode::SingleLine,
            tr::lng_search_filter_all()),
        st::createPollFieldPadding);

    // Lambda to (re)draw chat buttons based on filter
    auto drawChats = [this, content, filterField]() {
        // Remove previous chat buttons
        for (auto button : buttons_) {
            button->deleteLater();
        }
        buttons_.clear();
        for (auto label : labels_) {
            label->deleteLater();
        }
        labels_.clear();
        QString filterText = filterField->getLastText();

        const auto& accounts = _domain->accounts();
        Main::Account* cur_account = nullptr;
        for (const auto& [index, account] : accounts) {
            if (index == _accountIndex) {
                cur_account = account.get();
            }
        }
        if (cur_account == nullptr) {
            return;
        }
        const auto& account_data = cur_account->session().data();

        using ChatWithName = std::pair<not_null<const Dialogs::MainList*>, rpl::producer<QString>>;
        std::vector<ChatWithName> chat_lists;
        if (auto archive_folder = account_data.folderLoaded(Data::Folder::kId)) {
            chat_lists.emplace_back(account_data.chatsList(archive_folder), tr::lng_chats_action_archive());
        }
        chat_lists.emplace_back(account_data.chatsList(), tr::lng_chats_action_main_chats());

        for (const auto& [list, name] : chat_lists) {
            auto title = Ui::AddSubsectionTitle(content, name);
            labels_.push_back(title);
            for (auto chat : list->indexed()->all()) {
                if (chat->entry()->fixedOnTopIndex() == Dialogs::Entry::kArchiveFixOnTopIndex) {
                    continue; // Archive, skip
                }
                const auto& chat_name = chat->history()->peer->isSelf() ? tr::lng_saved_messages(tr::now) : chat->entry()->chatListName();
                // --- Filter logic ---
                if (!filterText.isEmpty() && !chat_name.contains(filterText, Qt::CaseInsensitive)) {
                    continue;
                }
                auto button = Settings::AddButtonWithIcon(content, rpl::single(chat_name), st::settingsButton);
                AddDialogImageToButton(button, st::settingsButton, chat);
                auto dialog_id = chat->key().peer()->id.value;
                button->toggleOn(rpl::single(data_.peer_ids.contains(dialog_id)));
                button->addClickHandler([this, chat, button] {
                    data_ = _description.button_handler(button, chat, std::move(data_));
                    _action->UpdateOrAddAction(_accountIndex, data_);
                    _domain->local().writeAccounts();
                });
                buttons_.push_back(button);
            }
        }
    };

    // Connect filter field to redraw on text change
    filterField->changes(
    ) | rpl::start_with_next([drawChats]() {
        drawChats();
    }, filterField->lifetime());

    // Initial draw
    drawChats();
}


