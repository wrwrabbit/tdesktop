#include "fakepasscodes_list.h"

#include "boxes/abstract_box.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_user.h"
#include "fakepasscode/action.h"
#include "fakepasscode/fake_passcode.h"
#include "fakepasscode/log/fake_log.h"
#include "fakepasscode/ui/action_ui.h"
#include "fakepasscode/ui/fakepasscode_box.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "storage/storage_domain.h"
#include "styles/style_boxes.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_window.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"

#include "fakepasscode/ptg.h"

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

[[nodiscard]] object_ptr<Ui::SettingsButton> MakeAccountButton(
    QWidget* parent,
    not_null<Main::Account*> account) {

    const auto session = &account->session();
    const auto user = session->user();

    auto text = rpl::single(
        user->name()
    ) | rpl::then(session->changes().realtimeNameUpdates(
        user
    ) | rpl::map([=] {
        return user->name();
    }));
    auto result = object_ptr<Ui::SettingsButton>(
        parent,
        rpl::duplicate(text),
        st::mainMenuAddAccountButton);
    const auto raw = result.data();

    struct State {
        State(QWidget* parent) : userpic(parent) {
            userpic.setAttribute(Qt::WA_TransparentForMouseEvents);
        }

        Ui::RpWidget userpic;
        Ui::PeerUserpicView view;
    };
    const auto state = raw->lifetime().make_state<State>(raw);

    const auto userpicSkip = 2 * st::mainMenuAccountLine + st::lineWidth;
    const auto userpicSize = st::mainMenuAccountSize
        + userpicSkip * 2;
    raw->heightValue(
    ) | rpl::start_with_next([=](int height) {
        const auto left = st::mainMenuAddAccountButton.iconLeft
            + (st::settingsIconAdd.width() - userpicSize) / 2;
        const auto top = (height - userpicSize) / 2;
        state->userpic.setGeometry(left, top, userpicSize, userpicSize);
        }, state->userpic.lifetime());

    state->userpic.paintRequest(
    ) | rpl::start_with_next([=] {
        auto p = Painter(&state->userpic);
        const auto size = st::mainMenuAccountSize;
        const auto line = st::mainMenuAccountLine;
        const auto skip = 2 * line + st::lineWidth;
        const auto full = size + skip * 2;
        user->paintUserpicLeft(p, state->view, skip, skip, full, size);
    }, state->userpic.lifetime());

    return result;
}

void FakePasscodeContent::setupContent() {
    const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

    // accout buttons

    const auto AccountUIActions = [this](int index) {

        QString result;
        for (const auto& type : FakePasscode::kAvailableAccountActions) {
            if (_domain->local().ContainsAction(_passcodeIndex, type)) {
                auto act = _domain->local().GetAction(_passcodeIndex, type);
                FakePasscode::AccountAction* accact = dynamic_cast<FakePasscode::AccountAction*>(act);
                if (accact) {
                    QString part = accact->GetDescriptionFor(index);
                    if (!part.isEmpty()) {
                        if (!result.isEmpty()) {
                            result += ", ";
                        }
                        result += part;
                    }
                }
            }
        }

        return result;
    };

    Ui::AddSubsectionTitle(content, tr::lng_fakeaccountaction_list());
    const auto accounts = Core::App().domain().orderedAccountsEx();
    for (const auto& record : accounts) {
        const auto texts = Ui::CreateChild<rpl::event_stream<QString>>(
            content);
        const auto button = content->add(MakeAccountButton(content, record.account));

        static style::FlatLabel stLabel(st::boxDividerLabel);
        stLabel.textFg = st::defaultSettingsRightLabel.textFg;
        content->add(object_ptr<Ui::FlatLabel>(
            content,
            texts->events(),
            stLabel),
            style::margins(
                st::boxRowPadding.left() + 32,
                0,
                st::boxRowPadding.right(),
                st::defaultVerticalListSkip * 2)
        );
        texts->fire(AccountUIActions(record.index));

        PTG::GetFakePasscodeUpdates(
        ) | rpl::start_with_next([=] {
            texts->fire(AccountUIActions(record.index));
        }, content->lifetime());

        button->addClickHandler([record, this] {
            auto box = _controller->show(Box<FakePasscodeAccountBox>(_domain, _controller, _passcodeIndex, record.index),
                                                                     Ui::LayerOption::KeepOther);
            box->boxClosing() | rpl::start_with_next([=] {
                PTG::FireFakePasscodeUpdates();
            }, box->lifetime());
        });

    }

    // non account action_list
    AddDivider(content);
    Ui::AddSubsectionTitle(content, tr::lng_fakeglobalaction_list(),
        style::margins(0, st::defaultVerticalListSkip, 0, 0));
    for (const auto& record : FakePasscode::kAvailableGlobalActions) {
        const auto ui = GetUIByAction(record.Type, _domain, _passcodeIndex, this);
        ui->Create(content, _controller);
        if (record.HasDivider) {
            Ui::AddDivider(content);
        }
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
        size_t passcodeIndex, int accountIndex);

    void setupContent();

private:
    Main::Domain* _domain;
    Window::SessionController* _controller;
    size_t _passcodeIndex;
    int _accountIndex;
};

FakePasscodeAccountContent::FakePasscodeAccountContent(QWidget* parent,
    Main::Domain* domain, not_null<Window::SessionController*> controller,
    size_t passcodeIndex, int accountIndex)
    : Ui::RpWidget(parent)
    , _domain(domain)
    , _controller(controller)
    , _passcodeIndex(passcodeIndex)
    , _accountIndex(accountIndex) {
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
    AddButtonWithIcon(content, tr::lng_add_fakepasscode(), st::settingsButtonActive,
                      {&st::settingsIconAdd, IconType::Round, &st::windowBgActive })->addClickHandler([this] {
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

    Ui::AddDividerText(content, tr::lng_clear_cache_on_lock_help());
    Ui::AddSkip(content, st::settingsCheckboxesSkip);

    Ui::AddSubsectionTitle(content, tr::lng_da_title());

    const auto toggledAlertDAChatJoin = Ui::CreateChild<rpl::event_stream<bool>>(this);
    auto buttonDAChatJoin = AddButtonWithIcon(content, tr::lng_da_chat_join_check(), st::settingsButton,
                                           {&st::menuIconSavedMessages})
            ->toggleOn(toggledAlertDAChatJoin->events_starting_with_copy(_domain->local().IsDAChatJoinCheckEnabled()));

    buttonDAChatJoin->addClickHandler([=] {
        _domain->local().SetDAChatJoinCheckEnabled(buttonDAChatJoin->toggled());
        _domain->local().writeAccounts();
    });

    const auto toggledAlertDAChannelJoin = Ui::CreateChild<rpl::event_stream<bool>>(this);
    auto buttonDAChannelJoin = AddButtonWithIcon(content, tr::lng_da_channel_join_check(), st::settingsButton,
        { &st::menuIconSavedMessages })
        ->toggleOn(toggledAlertDAChannelJoin->events_starting_with_copy(_domain->local().IsDAChannelJoinCheckEnabled()));

    buttonDAChannelJoin->addClickHandler([=] {
        _domain->local().SetDAChannelJoinCheckEnabled(buttonDAChannelJoin->toggled());
        _domain->local().writeAccounts();
        });

    const auto toggledAlertDAPostComment = Ui::CreateChild<rpl::event_stream<bool>>(this);
    auto buttonDAPostComment = AddButtonWithIcon(content, tr::lng_da_post_comment_check(), st::settingsButton,
        { &st::menuIconSavedMessages })
        ->toggleOn(toggledAlertDAPostComment->events_starting_with_copy(_domain->local().IsDAPostCommentCheckEnabled()));

    buttonDAPostComment->addClickHandler([=] {
        _domain->local().SetDAPostCommentCheckEnabled(buttonDAPostComment->toggled());
        _domain->local().writeAccounts();
        });

    const auto toggledAlertDAMakeReaction = Ui::CreateChild<rpl::event_stream<bool>>(this);
    auto buttonDAMakeReaction = AddButtonWithIcon(content, tr::lng_da_make_reaction_check(), st::settingsButton,
        { &st::menuIconSavedMessages })
        ->toggleOn(toggledAlertDAMakeReaction->events_starting_with_copy(_domain->local().IsDAMakeReactionCheckEnabled()));

    buttonDAMakeReaction->addClickHandler([=] {
        _domain->local().SetDAMakeReactionCheckEnabled(buttonDAMakeReaction->toggled());
        _domain->local().writeAccounts();
        });

    const auto toggledAlertDAStartBot = Ui::CreateChild<rpl::event_stream<bool>>(this);
    auto buttonDAStartBot = AddButtonWithIcon(content, tr::lng_da_start_bot_check(), st::settingsButton,
        { &st::menuIconSavedMessages })
        ->toggleOn(toggledAlertDAStartBot->events_starting_with_copy(_domain->local().IsDAStartBotCheckEnabled()));

    buttonDAStartBot->addClickHandler([=] {
        _domain->local().SetDAStartBotCheckEnabled(buttonDAStartBot->toggled());
        _domain->local().writeAccounts();
        });

    Ui::AddDividerText(content, tr::lng_da_common());
    Ui::AddSkip(content, st::settingsCheckboxesSkip);

    const auto toggledLogging = Ui::CreateChild<rpl::event_stream<bool>>(this);
    auto buttonLogging = AddButtonWithIcon(content, tr::lng_enable_advance_logging(), st::settingsButton,
                                           {&st::menuIconSavedMessages})
            ->toggleOn(toggledLogging->events_starting_with_copy(_domain->local().IsAdvancedLoggingEnabled()));

    buttonLogging->addClickHandler([=] {
        _domain->local().SetAdvancedLoggingEnabled(buttonLogging->toggled());
        _domain->local().writeAccounts();
    });

    Ui::AddDividerText(content, tr::lng_enable_advance_logging_help());
    Ui::AddSkip(content, st::settingsCheckboxesSkip);

    const auto toggledErasingCleaning = Ui::CreateChild<rpl::event_stream<bool>>(this);
    auto buttonErasing = AddButtonWithIcon(content, tr::lng_enable_dod_cleaning(), st::settingsButton,
                                           {&st::menuIconDelete})
        ->toggleOn(toggledErasingCleaning->events_starting_with_copy(_domain->local().IsErasingEnabled()));

    buttonErasing->addClickHandler([=] {
        _domain->local().SetErasingEnabled(buttonErasing->toggled());
        _domain->local().writeAccounts();
    });

    Ui::AddDividerText(content, tr::lng_enable_dod_cleaning_help());
    Ui::AddSkip(content, st::settingsCheckboxesSkip);

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
            _passcodeIndex, _accountIndex),
            st::sessionsScroll);
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
