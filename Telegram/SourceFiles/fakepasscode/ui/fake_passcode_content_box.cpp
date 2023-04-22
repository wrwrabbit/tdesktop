#include "fake_passcode_content_box.h"
#include "fake_passcode_content.h"
#include "boxes/abstract_box.h"
#include "settings/settings_common.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "lang/lang_keys.h"


FakePasscodeContentBox::FakePasscodeContentBox(QWidget*,
    Main::Domain* domain, not_null<Window::SessionController*> controller,
    size_t passcodeIndex)
    : _domain(domain)
    , _controller(controller)
    , _passcodeIndex(passcodeIndex) {
}

void FakePasscodeContentBox::prepare() {
    using namespace Settings;
    addButton(tr::lng_close(), [=] { closeBox(); });
    const auto content =
        setInnerWidget(object_ptr<FakePasscodeContent>(this, _domain, _controller,
            _passcodeIndex, this),
            st::sessionsScroll);
    content->resize(st::boxWideWidth, st::noContactsHeight);
    content->setupContent();
    setDimensions(st::boxWideWidth, st::sessionsHeight);
}