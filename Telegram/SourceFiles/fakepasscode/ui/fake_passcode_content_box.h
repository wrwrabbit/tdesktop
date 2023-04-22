#ifndef TELEGRAM_FAKEPASSCODE_CONTENT_BOX_H
#define TELEGRAM_FAKEPASSCODE_CONTENT_BOX_H

#include "main/main_domain.h"
#include "storage/storage_domain.h"
#include <ui/layers/box_content.h>
#include "window/window_session_controller.h"


class FakePasscodeContentBox : public Ui::BoxContent {
public:
    FakePasscodeContentBox(QWidget* parent,
        Main::Domain* domain, not_null<Window::SessionController*> controller,
        size_t passcodeIndex);

protected:
    void prepare() override;

private:
    Main::Domain* _domain;
    Window::SessionController* _controller;
    size_t _passcodeIndex;

};
#endif //TELEGRAM_FAKEPASSCODE_CONTENT_BOX_H