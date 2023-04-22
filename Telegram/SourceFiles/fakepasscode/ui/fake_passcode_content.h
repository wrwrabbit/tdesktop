#ifndef TELEGRAM_FAKEPASSCODE_CONTENT_H
#define TELEGRAM_FAKEPASSCODE_CONTENT_H

#include "window/window_session_controller.h"
#include "main/main_domain.h"
#include "fakepasscode/ui/fake_passcode_content_box.h"

class FakePasscodeContent : public Ui::RpWidget {
public:
    FakePasscodeContent(QWidget* parent,
        Main::Domain* domain, not_null<Window::SessionController*> controller,
        size_t passcodeIndex, FakePasscodeContentBox* outerBox);

    void setupContent();

private:
    Main::Domain* _domain;
    Window::SessionController* _controller;
    size_t _passcodeIndex;
    FakePasscodeContentBox* _outerBox;
};
#endif //TELEGRAM_FAKEPASSCODE_CONTENT_H