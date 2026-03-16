#include "clear_cache_ui.h"
#include "lang/lang_keys.h"
#include "settings/settings_common.h"
#include "ui/widgets/buttons.h"
#include "ui/vertical_list.h"
#include "styles/style_boxes.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "main/main_domain.h"
#include "storage/storage_domain.h"
#include "fakepasscode/log/fake_log.h"
#include "clear_cache_permissions.h"
#include "styles/style_menu_icons.h"

void ClearCacheUI::Create(not_null<Ui::VerticalLayout*> content,
                          Window::SessionController*) {
    auto *button = Settings::AddButtonWithIcon(content, tr::lng_clear_cache(), st::settingsButton,
                                               {&st::menuIconClear})
            ->toggleOn(rpl::single(
                    _domain->local().ContainsAction(_index, FakePasscode::ActionType::ClearCache)));
    button->toggledValue(
    ) | rpl::filter([=](bool v) {
        return v != _domain->local().ContainsAction(_index, FakePasscode::ActionType::ClearCache);
    }) | rpl::on_next([=](bool v) {
        if (v) {
            FAKE_LOG(qsl("Add action ClearCache to %1").arg(_index));
            _domain->local().AddAction(_index, FakePasscode::ActionType::ClearCache);
#ifdef Q_OS_MAC
            FakePasscode::RequestCacheFolderMacosPermission();
#endif
        } else {
            FAKE_LOG(qsl("Remove action ClearCache from %1").arg(_index));
            _domain->local().RemoveAction(_index, FakePasscode::ActionType::ClearCache);
        }
        _domain->local().writeAccounts();
    }, button->lifetime());

    Ui::AddDividerText(content, tr::lng_clear_cache_help());
    Ui::AddSkip(content, st::settingsCheckboxesSkip);
}

ClearCacheUI::ClearCacheUI(QWidget * parent, gsl::not_null<Main::Domain*> domain, size_t index)
: ActionUI(parent, domain, index) {
}
