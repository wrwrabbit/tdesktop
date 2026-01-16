#ifndef TELEGRAM_FAKEPASSCODE_HWLOCK_BOX_H
#define TELEGRAM_FAKEPASSCODE_HWLOCK_BOX_H

#include "boxes/abstract_box.h"
#include "mtproto/sender.h"
#include "core/core_cloud_password.h"
#include "window/window_session_controller.h"

namespace MTP {
    class Instance;
} // namespace MTP

namespace Main {
    class Session;
} // namespace Main

namespace Ui {
    class InputField;
    class PasswordInput;
    class LinkButton;
} // namespace Ui

namespace Core {
    struct CloudPasswordState;
} // namespace Core

class FakePasscodeHWLockBox : public Ui::BoxContent {
public:
    FakePasscodeHWLockBox(QWidget*, 
        not_null<Window::SessionController*> controller,
        not_null<rpl::event_stream<bool>*> toggler);

    rpl::producer<> passwordReloadNeeded() const;
    rpl::producer<> clearUnconfirmedPassword() const;

protected:
    void prepare() override;
    void setInnerFocus() override;

    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void submit();
    void oldChanged();
    void save();
    void badOldPasscode();

    Main::Session* _session = nullptr;
    Window::SessionController* _controller = nullptr;
    rpl::event_stream<bool>* _toggler = nullptr;

    object_ptr<Ui::PasswordInput> _oldPasscode;

    QString _oldError;

    rpl::event_stream<QByteArray> _newPasswordSet;
    rpl::event_stream<> _passwordReloadNeeded;
    rpl::event_stream<> _clearUnconfirmedPassword;

};

#endif //TELEGRAM_FAKEPASSCODE_HWLOCK_BOX_H
