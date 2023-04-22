#include "hide_accounts.h"
#include "core/application.h"
#include "main/main_domain.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "data/data_session.h"
#include "fakepasscode/log/fake_log.h"

void FakePasscode::HideAccountsAction::Execute() {
    std::vector<Main::Account*> toHide;
    for (const auto &[index, account] : Core::App().domain().accounts()) {
        if (index_to_hide_[index]) {
            FAKE_LOG(qsl("Account %1 setup to hide, perform.").arg(index));
            toHide.push_back(account.get());
        }
    }
    Core::App().domain().hideAccounts(toHide);
}

QByteArray FakePasscode::HideAccountsAction::Serialize() const {
    if (index_to_hide_.empty()) {
        return {};
    }

    QByteArray result;
    QDataStream stream(&result, QIODevice::ReadWrite);
    stream << static_cast<qint32>(ActionType::HideAccounts);
    QByteArray inner_data;
    QDataStream inner_stream(&inner_data, QIODevice::ReadWrite);
    for (const auto&[index, is_logged_out] : index_to_hide_) {
        if (is_logged_out) {
            FAKE_LOG(qsl("Account %1 serialized in hide action, because it will be hide from it.").arg(index));
            inner_stream << index;
        }
    }
    stream << inner_data;
    return result;
}

FakePasscode::ActionType FakePasscode::HideAccountsAction::GetType() const {
    return ActionType::HideAccounts;
}

FakePasscode::HideAccountsAction::HideAccountsAction(QByteArray inner_data) {
    FAKE_LOG(qsl("Create hide from QByteArray of size: %1").arg(inner_data.size()));
    if (!inner_data.isEmpty()) {
        QDataStream stream(&inner_data, QIODevice::ReadOnly);
        while (!stream.atEnd()) {
            qint32 index;
            stream >> index;
            FAKE_LOG(qsl("Account %1 deserialized in hide action.").arg(index));
            index_to_hide_[index] = true;
        }
    }
    SubscribeOnAccountsChanges();
}

FakePasscode::HideAccountsAction::HideAccountsAction(base::flat_map<qint32, bool> index_to_hide)
: index_to_hide_(std::move(index_to_hide))
{
    SubscribeOnAccountsChanges();
}

void FakePasscode::HideAccountsAction::SetHide(qint32 index, bool hide) {
    FAKE_LOG(qsl("Set hide %1 for account %2").arg(hide).arg(index));
    index_to_hide_[index] = hide;
}

bool FakePasscode::HideAccountsAction::IsHidden(qint32 index) const {
    if (auto pos = index_to_hide_.find(index); pos != index_to_hide_.end()) {
        FAKE_LOG(qsl("Found hide for %1. Send %2").arg(index).arg(pos->second));
        return pos->second;
    }
	FAKE_LOG(qsl("Not found hide for %1. Send false").arg(index));
	return false;
}

const base::flat_map<qint32, bool>& FakePasscode::HideAccountsAction::GetHide() const {
    return index_to_hide_;
}

void FakePasscode::HideAccountsAction::SubscribeOnAccountsChanges() {
    Core::App().domain().accountsChanges() | rpl::start_with_next([this] {
        SubscribeOnLoggingOut();
    }, lifetime_);
}

void FakePasscode::HideAccountsAction::Prepare() {
    SubscribeOnLoggingOut();
}

void FakePasscode::HideAccountsAction::SubscribeOnLoggingOut() {
    sub_lifetime_.destroy();
    for (const auto&[index, account] : Core::App().domain().accounts()) {
        FAKE_LOG(qsl("Subscribe on hide for account %1").arg(index));
        account->sessionChanges() | rpl::start_with_next([index = index, this] (const Main::Session* session) {
            if (session == nullptr) {
                FAKE_LOG(qsl("Account %1 logged out, remove from us.").arg(index));
                index_to_hide_.remove(index);
            }
        }, sub_lifetime_);
    }
}
