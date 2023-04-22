#include "fake_passcode_content.h"
#include "fake_passcode_content_box.h"
#include "lang/lang_keys.h"
#include "styles/style_boxes.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "fakepasscode/ui/fakepasscode_box.h"
#include "fakepasscode/ui/action_ui.h"
#include "settings/settings_common.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/buttons.h"

FakePasscodeContent::FakePasscodeContent(QWidget* parent,
    Main::Domain* domain, not_null<Window::SessionController*> controller,
    size_t passcodeIndex, FakePasscodeContentBox* outerBox)
    : Ui::RpWidget(parent)
    , _domain(domain)
    , _controller(controller)
    , _passcodeIndex(passcodeIndex)
    , _outerBox(outerBox) {
}

void FakePasscodeContent::setupContent() {
    const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
    Settings::AddSubsectionTitle(content, tr::lng_fakeaction_list());

    for (const auto& type : FakePasscode::kAvailableActions) {
        const auto ui = GetUIByAction(type, _domain, _passcodeIndex, this);
        ui->Create(content, _controller);
        Settings::AddDivider(content);
    }
    Settings::AddButton(content, tr::lng_fakepasscode_change(), st::settingsButton,
        { &st::settingsIconReload, Settings::kIconGreen })
        ->addClickHandler([this] {
        _controller->show(Box<FakePasscodeBox>(&_controller->session(), false, false,
        _passcodeIndex),
        Ui::LayerOption::KeepOther);
            });
    Settings::AddButton(content, tr::lng_remove_fakepasscode(), st::settingsAttentionButton)
        ->addClickHandler([this] {
        destroy();
    _domain->local().RemoveFakePasscode(_passcodeIndex);
    _outerBox->closeBox();
            });
    Ui::ResizeFitChild(this, content);
}
