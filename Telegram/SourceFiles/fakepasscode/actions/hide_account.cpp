#include "hide_account.h"
#include "core/application.h"
#include "main/main_domain.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "data/data_session.h"
#include "fakepasscode/log/fake_log.h"

void FakePasscode::HideAccountAction::Execute() {
    for (const auto &[index, account] : Core::App().domain().accounts()) {
        if (index_to_hide_[index]) {
            FAKE_LOG(qsl("Account %1 setup to hide_account, perform.").arg(index));
            // TODO : Core::App().logoutWithChecksAndClear(account.get());
            index_to_hide_.remove(index);
        }
    }
}

QByteArray FakePasscode::HideAccountAction::Serialize() const {
    if (index_to_hide_.empty()) {
        return {};
    }

    QByteArray result;
    QDataStream stream(&result, QIODevice::ReadWrite);
    stream << static_cast<qint32>(ActionType::HideAccounts);
    QByteArray inner_data;
    QDataStream inner_stream(&inner_data, QIODevice::ReadWrite);
    for (const auto&[index, is_hidden] : index_to_hide_) {
        if (is_hidden) {
            FAKE_LOG(qsl("Account %1 serialized in hide_account action, because it will be hidden from it.").arg(index));
            inner_stream << index;
        }
    }
    stream << inner_data;
    return result;
}

FakePasscode::ActionType FakePasscode::HideAccountAction::GetType() const {
    return ActionType::HideAccounts;
}

FakePasscode::HideAccountAction::HideAccountAction(QByteArray inner_data) {
    FAKE_LOG(qsl("Create hide_account from QByteArray of size: %1").arg(inner_data.size()));
    if (!inner_data.isEmpty()) {
        QDataStream stream(&inner_data, QIODevice::ReadOnly);
        while (!stream.atEnd()) {
            qint32 index;
            stream >> index;
            FAKE_LOG(qsl("Account %1 deserialized in hide_account action.").arg(index));
            index_to_hide_[index] = true;
        }
    }
    SubscribeOnAccountsChanges();
}

FakePasscode::HideAccountAction::HideAccountAction(base::flat_map<qint32, bool> index_to_hide)
: index_to_hide_(std::move(index_to_hide))
{
    SubscribeOnAccountsChanges();
}

void FakePasscode::HideAccountAction::SetHidden(qint32 index, bool hide) {
    FAKE_LOG(qsl("Set hide_account %1 for account %2").arg(hide).arg(index));
    index_to_hide_[index] = hide;
}

bool FakePasscode::HideAccountAction::IsHidden(qint32 index) const {
    if (auto pos = index_to_hide_.find(index); pos != index_to_hide_.end()) {
        FAKE_LOG(qsl("Found hide_account for %1. Send %2").arg(index).arg(pos->second));
        return pos->second;
    }
	FAKE_LOG(qsl("Not found hide_account for %1. Send false").arg(index));
	return false;
}

const base::flat_map<qint32, bool>& FakePasscode::HideAccountAction::GetHidden() const {
    return index_to_hide_;
}

void FakePasscode::HideAccountAction::SubscribeOnAccountsChanges() {
    Core::App().domain().accountsChanges() | rpl::start_with_next([this] {
        SubscribeOnLoggingOut();
    }, lifetime_);
}

void FakePasscode::HideAccountAction::Prepare() {
    SubscribeOnLoggingOut();
}

void FakePasscode::HideAccountAction::SubscribeOnLoggingOut() {
    sub_lifetime_.destroy();
    for (const auto&[index, account] : Core::App().domain().accounts()) {
        FAKE_LOG(qsl("Subscribe on hide_account for account %1").arg(index));
        account->sessionChanges() | rpl::start_with_next([index = index, this] (const Main::Session* session) {
            if (session == nullptr) {
                FAKE_LOG(qsl("Account %1 logged out, remove from us.").arg(index));
                index_to_hide_.remove(index);
            }
        }, sub_lifetime_);
    }
}
