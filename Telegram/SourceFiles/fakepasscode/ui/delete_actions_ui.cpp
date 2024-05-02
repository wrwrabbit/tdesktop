#include "delete_actions_ui.h"
#include "lang/lang_keys.h"
#include "settings/settings_common.h"
#include "ui/widgets/buttons.h"
#include "ui/vertical_list.h"
#include "styles/style_settings.h"
#include "main/main_domain.h"
#include "storage/storage_domain.h"
#include "fakepasscode/log/fake_log.h"
#include "styles/style_menu_icons.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "ui/layers/generic_box.h"
#include "styles/style_layers.h" // st::boxLabel
#include "ui/boxes/confirm_box.h"
#include "core/application.h"

#include "fakepasscode/ptg.h"

void DeleteActionsUI::Create(not_null<Ui::VerticalLayout*> content,
                             Window::SessionController* session) {
    const auto toggled = Ui::CreateChild<rpl::event_stream<bool>>(content.get());
    auto *button = Settings::AddButtonWithIcon(content, tr::lng_delete_actions(), st::settingsButton,
                                               {&st::menuIconRemove})
            ->toggleOn(toggled->events_starting_with_copy(
                    _domain->local().ContainsAction(_index, FakePasscode::ActionType::DeleteActions)));
    button->addClickHandler([=] {
        if (button->toggled()) {
            FAKE_LOG(qsl("Add action DeleteActions to %1").arg(_index));

            auto passcode = PTG::GetPasscode(_index);
            if (passcode && passcode->HasHiddenAccounts()) {
                toggled->fire(false);
                Core::App().hideMediaView();
				const auto use = session
					? &session->window()
					: Core::App().activeWindow();
				auto box = Box([=](not_null<Ui::GenericBox*> box) {
					Ui::ConfirmBox(box, {
						.text = (tr::lng_delete_actions_confirm(tr::now)),
						.confirmed = [=](Fn<void()> hide) {
                            toggled->fire(true);
                            _domain->local().AddAction(_index, FakePasscode::ActionType::DeleteActions);
                            passcode->SetHidden2Logout();
                            hide();
						},
						.confirmText = tr::lng_box_yes()
					});
    			});
    			use->show(std::move(box));
                return;
            }

            _domain->local().AddAction(_index, FakePasscode::ActionType::DeleteActions);
        } else {
            FAKE_LOG(qsl("Remove action DeleteActions from %1").arg(_index));
            _domain->local().RemoveAction(_index, FakePasscode::ActionType::DeleteActions);
        }
        _domain->local().writeAccounts();
    });

    Ui::AddDividerText(content, tr::lng_delete_actions_help());
    Ui::AddSkip(content, st::settingsCheckboxesSkip);
}

DeleteActionsUI::DeleteActionsUI(QWidget * parent, gsl::not_null<Main::Domain*> domain, size_t index)
: ActionUI(parent, domain, index) {}
