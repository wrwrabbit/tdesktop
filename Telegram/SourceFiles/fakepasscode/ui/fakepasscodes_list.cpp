#include "fakepasscodes_list.h"
#include "lang/lang_keys.h"
#include "ui/wrap/vertical_layout.h"
#include "fakepasscode/fake_passcode.h"
#include "fakepasscode/action.h"
#include "settings/settings_common.h"
#include "styles/style_boxes.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "fakepasscode/ui/fakepasscode_box.h"
#include "core/application.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/buttons.h"
#include "ui/vertical_list.h"
#include "fakepasscode/ui/action_ui.h"
#include "main/main_session.h"
#include "main/main_domain.h"
#include "main/main_account.h"
#include "storage/storage_domain.h"
#include "data/data_user.h"
#include "boxes/abstract_box.h"
#include "ui/text/text_utilities.h"
#include "styles/style_menu_icons.h"
#include "fakepasscode/log/fake_log.h"

class FakePasscodeContentBox;
class FakePasscodeAccountContent;

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
    std::vector<Settings::Button*> account_buttons_;
};

FakePasscodeContent::FakePasscodeContent(QWidget *parent,
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

    // accout buttons

    const auto AccountUIActions = [=] {

        size_t total = 0;
        for (const auto& type : FakePasscode::kAvailableAccountActions) {
            if (_domain->local().ContainsAction(_passcodeIndex, type)) {
                total++;
            }
        }

        return rpl::single(QString::number(total));
    };

    Ui::AddSubsectionTitle(content, tr::lng_fakeaccountaction_list());
    const auto& accounts = Core::App().domain().accounts();
    account_buttons_.resize(accounts.size());
    size_t idx = 0;
    for (const auto& [index, account] : accounts) {
        auto user = account->session().user();
        auto button = Settings::AddButtonWithLabel(
            content,
            rpl::single(user->name()),
            AccountUIActions(),
            st::settingsButtonNoIcon
        );
        account_buttons_[idx] = button;

        button->addClickHandler([index, button, this] {
                _controller->show(Box<FakePasscodeAccountBox>(_domain, _controller, _passcodeIndex, index),
                                  Ui::LayerOption::KeepOther);
            });
        ++idx;
    }

    // non account action_list
    Ui::AddSubsectionTitle(content, tr::lng_fakeglobalaction_list());
    for (const auto& type : FakePasscode::kAvailableGlobalActions) {
        const auto ui = GetUIByAction(type, _domain, _passcodeIndex, this);
        ui->Create(content, _controller);
        Ui::AddDivider(content);
    }

    // password actions
    Ui::AddSubsectionTitle(content, tr::lng_fakepassaction_list());
    Settings::AddButtonWithIcon(content, tr::lng_fakepasscode_change(), st::settingsButton,
                                {&st::menuIconEdit})
            ->addClickHandler([this] {
                _controller->show(Box<FakePasscodeBox>(_controller, false, false,
                                                       _passcodeIndex),
                                  Ui::LayerOption::KeepOther);
            });
    Settings::AddButtonWithIcon(content, tr::lng_remove_fakepasscode(), st::settingsAttentionButtonWithIcon,
                                {&st::menuIconRemove})
            ->addClickHandler([this] {
                destroy();
                _domain->local().RemoveFakePasscode(_passcodeIndex);
                _outerBox->closeBox();
            });

    Ui::ResizeFitChild(this, content);
}

class FakePasscodeAccountContent : public Ui::RpWidget {
public:
    FakePasscodeAccountContent(QWidget* parent,
        Main::Domain* domain, not_null<Window::SessionController*> controller,
        size_t passcodeIndex, int accountIndex,
        FakePasscodeAccountBox* outerBox);

    void setupContent();

private:
    Main::Domain* _domain;
    Window::SessionController* _controller;
    size_t _passcodeIndex;
    int _accountIndex;
    FakePasscodeAccountBox* _outerBox;
};

FakePasscodeAccountContent::FakePasscodeAccountContent(QWidget* parent,
    Main::Domain* domain, not_null<Window::SessionController*> controller,
    size_t passcodeIndex, int accountIndex,
    FakePasscodeAccountBox* outerBox)
    : Ui::RpWidget(parent)
    , _domain(domain)
    , _controller(controller)
    , _passcodeIndex(passcodeIndex)
    , _accountIndex(accountIndex)
    , _outerBox(outerBox) {
}

void FakePasscodeAccountContent::setupContent() {
    const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

    Ui::AddSubsectionTitle(content, tr::lng_fakeaccountaction_list());

    // account action_list
    for (const auto& type : FakePasscode::kAvailableAccountActions) {
        const auto ui = GetAccountUIByAction(type, _domain, _passcodeIndex, _accountIndex, this);
        ui->Create(content, _controller);
    }

    Ui::ResizeFitChild(this, content);
}

class FakePasscodeList : public Ui::RpWidget {
public:
    FakePasscodeList(QWidget*, not_null<Main::Domain*> domain,
                     not_null<Window::SessionController*> controller);

    void setupContent();

private:
    Main::Domain* _domain;
    Window::SessionController* _controller;

    void draw(size_t passcodesSize);
};

FakePasscodeList::FakePasscodeList(QWidget * parent, not_null<Main::Domain *> domain,
                                   not_null<Window::SessionController*> controller)
: Ui::RpWidget(parent), _domain(domain), _controller(controller) {
}

void FakePasscodeList::draw(size_t passcodesSize) {
    using namespace Settings;
    FAKE_LOG(("Draw %1 passcodes").arg(passcodesSize));
    const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
    for (size_t i = 0; i < passcodesSize; ++i) {
        AddButtonWithIcon(content, tr::lng_fakepasscode(lt_caption, _domain->local().GetFakePasscodeName(i)),
                          st::settingsButton, { &st::menuIconLock }
                          )->addClickHandler([this, i]{
            _controller->show(Box<FakePasscodeContentBox>(_domain, _controller, i),
                              Ui::LayerOption::KeepOther);
        });
    }
    AddDivider(content);
    AddButtonWithIcon(content, tr::lng_add_fakepasscode(), st::settingsButton,
                      {&st::settingsIconAdd})->addClickHandler([this] {
        _controller->show(Box<FakePasscodeBox>(_controller, false, true, 0), // _domain
                          Ui::LayerOption::KeepOther);
    });
    AddDividerText(content, tr::lng_special_actions());
    const auto toggledCacheCleaning = Ui::CreateChild<rpl::event_stream<bool>>(this);
    auto buttonCacheCleaning = AddButtonWithIcon(content, tr::lng_clear_cache_on_lock(), st::settingsButton,
                                                 {&st::menuIconClear})
            ->toggleOn(toggledCacheCleaning->events_starting_with_copy(_domain->local().IsCacheCleanedUpOnLock()));

    buttonCacheCleaning->addClickHandler([=] {
        _domain->local().SetCacheCleanedUpOnLock(buttonCacheCleaning->toggled());
        _domain->local().writeAccounts();
    });

    const auto toggledLogging = Ui::CreateChild<rpl::event_stream<bool>>(this);
    auto buttonLogging = AddButtonWithIcon(content, tr::lng_enable_advance_logging(), st::settingsButton,
                                           {&st::menuIconSavedMessages})
            ->toggleOn(toggledLogging->events_starting_with_copy(_domain->local().IsAdvancedLoggingEnabled()));

    buttonLogging->addClickHandler([=] {
        _domain->local().SetAdvancedLoggingEnabled(buttonLogging->toggled());
        _domain->local().writeAccounts();
    });

    const auto toggledErasingCleaning = Ui::CreateChild<rpl::event_stream<bool>>(this);
    auto buttonErasing = AddButtonWithIcon(content, tr::lng_enable_dod_cleaning(), st::settingsButton,
                                           {&st::menuIconClear})
        ->toggleOn(toggledErasingCleaning->events_starting_with_copy(_domain->local().IsErasingEnabled()));

    buttonErasing->addClickHandler([=] {
        _domain->local().SetErasingEnabled(buttonErasing->toggled());
        _domain->local().writeAccounts();
    });

    Ui::ResizeFitChild(this, content);
    FAKE_LOG(("Draw %1 passcodes: success").arg(passcodesSize));
}

void FakePasscodeList::setupContent() {
    using namespace Settings;
    auto size = _domain->local().GetFakePasscodesSize();
    std::move(size) | rpl::start_with_next([this](size_t value) {
        draw(value);
    }, lifetime());
}

FakePasscodeContentBox::FakePasscodeContentBox(QWidget *,
                                               Main::Domain* domain, not_null<Window::SessionController*> controller,
                                               size_t passcodeIndex)
: _domain(domain)
, _controller(controller)
, _passcodeIndex(passcodeIndex) {
}

void FakePasscodeContentBox::prepare() {
    using namespace Settings;
    setTitle(tr::lng_fakepasscode(lt_caption, _domain->local().GetFakePasscodeName(_passcodeIndex)));
    addButton(tr::lng_close(), [=] { closeBox(); });
    const auto content =
            setInnerWidget(object_ptr<FakePasscodeContent>(this, _domain, _controller,
                                                           _passcodeIndex, this),
                    st::sessionsScroll);
    //content->resize(st::boxWideWidth, st::noContactsHeight);
    content->setupContent();
    setDimensionsToContent(st::boxWideWidth, content);
}

FakePasscodeAccountBox::FakePasscodeAccountBox(QWidget*,
    Main::Domain* domain, not_null<Window::SessionController*> controller,
    size_t passcodeIndex, const int accountIndex)
    : _domain(domain)
    , _controller(controller)
    , _passcodeIndex(passcodeIndex)
    , _accountIndex(accountIndex) {
}

void FakePasscodeAccountBox::prepare() {
    using namespace Settings;
    for (auto& account : _domain->accounts())
    {
        if (account.index == _accountIndex)
        {
            setTitle(tr::lng_fakeaccountaction_title(lt_caption, rpl::single(account.account->session().user()->name())));
        }
    }
    addButton(tr::lng_close(), [=] { closeBox(); });
    const auto content =
        setInnerWidget(object_ptr<FakePasscodeAccountContent>(this, _domain, _controller,
            _passcodeIndex, _accountIndex, this),
            st::sessionsScroll);
    //content->resize(st::boxWideWidth, st::noContactsHeight);
    content->setupContent();
    setDimensionsToContent(st::boxWideWidth, content);
}

void FakePasscodeListBox::prepare() {
    setTitle(tr::lng_fakepasscodes_list());
    addButton(tr::lng_close(), [=] { closeBox(); });

    const auto content = setInnerWidget(
            object_ptr<FakePasscodeList>(this, _domain, _controller),
            st::sessionsScroll);
    //content->resize(st::boxWideWidth, st::noContactsHeight);
    content->setupContent();
    setDimensionsToContent(st::boxWideWidth, content);
}

FakePasscodeListBox::FakePasscodeListBox(QWidget *, not_null<Main::Domain *> domain,
                                         not_null<Window::SessionController*> controller)
: _domain(domain), _controller(controller) {
}
