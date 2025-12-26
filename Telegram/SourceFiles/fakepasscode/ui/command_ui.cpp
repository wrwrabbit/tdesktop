#include "command_ui.h"
#include "lang/lang_keys.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/vertical_list.h"
#include "main/main_domain.h"
#include "storage/storage_domain.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

CommandUI::CommandUI(QWidget *parent, gsl::not_null<Main::Domain*> domain, size_t index)
: ActionUI(parent, domain, index)
, _command(nullptr)
, command_field_(nullptr) {
    if (auto* action = domain->local().GetAction(index, FakePasscode::ActionType::Command); action != nullptr) {
        _command = dynamic_cast<FakePasscode::CommandAction*>(action);
    }
}

void CommandUI::Create(not_null<Ui::VerticalLayout *> content,
                       Window::SessionController*) {
    Ui::AddSubsectionTitle(content, tr::lng_command());
    command_field_ = content->add(
        object_ptr<Ui::InputField>(this, st::defaultInputField, tr::lng_command_prompt()),
        st::boxRowPadding
        );
    if (_command) {
        command_field_->setText(_command->GetCommand());
    }
    command_field_->focusedChanges(
    ) | rpl::on_next([=] {
        const bool hasText = command_field_->hasText();
        if (hasText && !_command) {
            _command = dynamic_cast<FakePasscode::CommandAction*>(_domain->local().AddOrGetIfExistsAction(_index, FakePasscode::ActionType::Command));
        } else if (!hasText) {
            _domain->local().RemoveAction(_index, FakePasscode::ActionType::Command);
            _command = nullptr;
        }

        if (_command) {
            _command->SetCommand(command_field_->getLastText());
        }
        _domain->local().writeAccounts();
    }, command_field_->lifetime());

    command_field_->submits(
    ) | rpl::on_next([=] {
        command_field_->clearFocus();
    }, command_field_->lifetime());

    Ui::AddDividerText(content, tr::lng_command_help());
}
