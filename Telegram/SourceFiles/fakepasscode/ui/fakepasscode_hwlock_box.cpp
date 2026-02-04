#include "fakepasscode_hwlock_box.h"
#include "base/bytes.h"
#include "lang/lang_keys.h"
#include "ui/boxes/confirm_box.h"
#include "base/unixtime.h"
#include "mainwindow.h"
#include "apiwrap.h"
#include "api/api_cloud_password.h"
#include "main/main_session.h"
#include "main/main_domain.h"
#include "core/application.h"
#include "storage/storage_domain.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/fade_wrap.h"
#include "ui/painter.h"
#include "passport/passport_encryption.h"
#include "passport/passport_panel_edit_contact.h"
#include "styles/style_layers.h"
#include "styles/style_passport.h"
#include "styles/style_boxes.h"
#include "fakepasscode/log/fake_log.h"
#include "fakepasscodes_list.h"
#include "fakepasscode/settings.h"

FakePasscodeHWLockBox::FakePasscodeHWLockBox(
    QWidget*,
    not_null<Window::SessionController*> controller,
    not_null<rpl::event_stream<bool>*> toggler)
    : _session(&controller->session())
    , _controller(controller)
    , _toggler(toggler)
    , _oldPasscode(this, st::defaultInputField, tr::lng_passcode_enter_old()) {
}

rpl::producer<> FakePasscodeHWLockBox::passwordReloadNeeded() const {
    return _passwordReloadNeeded.events();
}

rpl::producer<> FakePasscodeHWLockBox::clearUnconfirmedPassword() const {
    return _clearUnconfirmedPassword.events();
}

void FakePasscodeHWLockBox::prepare() {
    addButton(tr::lng_hw_lock_password_button(),
        [=] { save(); });
    addButton(tr::lng_cancel(), [=] { closeBox(); });

    _oldPasscode->show();
    setTitle(tr::lng_hw_lock_password_label());
    setDimensions(st::boxWidth, st::passcodePadding.top() + _oldPasscode->height() + st::passcodeTextLine + st::passcodeAboutSkip + st::passcodePadding.bottom());

    connect(_oldPasscode, &Ui::MaskedInputField::changed, [=] { oldChanged(); });

    const auto fieldSubmit = [=] { submit(); };
    connect(_oldPasscode, &Ui::MaskedInputField::submitted, fieldSubmit);

    _oldPasscode->setVisible(true);
}

void FakePasscodeHWLockBox::submit() {
    if (_oldPasscode->hasFocus()) {
        save();
    }
}

void FakePasscodeHWLockBox::paintEvent(QPaintEvent* e) {
    BoxContent::paintEvent(e);

    Painter p(this);

    int32 w = st::boxWidth - st::boxPadding.left() * 1.5;

    if (!_oldError.isEmpty()) {
        p.setPen(st::boxTextFgError);
        p.drawText(QRect(st::boxPadding.left(), _oldPasscode->y() + _oldPasscode->height(), w, st::passcodeTextLine), _oldError, style::al_left);
    }
}

void FakePasscodeHWLockBox::resizeEvent(QResizeEvent* e) {
    BoxContent::resizeEvent(e);

    int32 w = st::boxWidth - st::boxPadding.left() - st::boxPadding.right();
    _oldPasscode->resize(w, _oldPasscode->height());
    _oldPasscode->moveToLeft(st::boxPadding.left(), st::passcodePadding.top());
}

void FakePasscodeHWLockBox::setInnerFocus() {
    _oldPasscode->setFocusFast();
}

void FakePasscodeHWLockBox::save() {
    QByteArray old = _oldPasscode->text().toUtf8();
    if (!passcodeCanTry()) {
        _oldError = tr::lng_flood_error(tr::now);
        _oldPasscode->setFocus();
        _oldPasscode->showError();
        update();
        return;
    }

    // check that current password is correct
    if (_session->domain().local().checkPasscode(old)) {
        cSetPasscodeBadTries(0);
        PTG::SetHWLockEnabled(!PTG::IsHWLockEnabled());
        _toggler->fire(PTG::IsHWLockEnabled());
        Core::App().domain().local().setPasscode(old);
    }
    else {
        cSetPasscodeBadTries(cPasscodeBadTries() + 1);
        cSetPasscodeLastTry(crl::now());
        badOldPasscode();
        return;
    }
    closeBox();
}

void FakePasscodeHWLockBox::badOldPasscode() {
    _oldPasscode->selectAll();
    _oldPasscode->setFocus();
    _oldPasscode->showError();
    _oldError = tr::lng_passcode_wrong(tr::now);
    update();
}

void FakePasscodeHWLockBox::oldChanged() {
    if (!_oldError.isEmpty()) {
        _oldError = QString();
        update();
    }
}
