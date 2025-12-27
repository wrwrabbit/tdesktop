#include "delete_contacts.h"

#include "lang/lang_keys.h"
#include "fakepasscode/log/fake_log.h"
#include "fakepasscode/mtp_holder/crit_api.h"

#include "main/main_account.h"
#include "main/main_session.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_stories.h"
#include "history/history.h"
#include "apiwrap.h"

using namespace FakePasscode;

void DeleteContactsAction::ExecuteAccountAction(int index, Main::Account* account, const ToggleAction&) {
    //TODO check with logout
    FAKE_LOG(qsl("Executing DeleteContactsAction on account %1.").arg(index));
    if (!account->sessionExists()) {
        FAKE_LOG(qsl("Account %1 session doesn't exists.").arg(index));
        return;
    }

    QVector<MTPInputUser> contacts;
    auto session = account->maybeSession();
    auto& data_session = session->data();
    for (auto row : data_session.contactsList()->all()) {
        if (auto history = row->history()) {
            if (auto userData = history->peer->asUser()) {
                contacts.push_back(userData->inputUser());
            }
            // clear stories
            if (history->peer->hasActiveStories()) {
                data_session.stories().toggleHidden(history->peer->id, true, nullptr);
            }
        }
    }

    session->data().clearContacts();
    
    auto onFail = [](const MTP::Error& error){
        FAKE_LOG(qsl("DeleteContactsAction: error(%1):%2 %3").arg(error.code()).arg(error.type()).arg(error.description()));
    };

    FAKE_CRITICAL_REQUEST(session)
    session->api().request(MTPcontacts_ResetSaved())
        .fail(onFail)
        .send();

    FAKE_CRITICAL_REQUEST(session)
    session->api().request(MTPcontacts_DeleteContacts(
            MTP_vector<MTPInputUser>(std::move(contacts))))
        .done([session](const MTPUpdates &result){
            session->data().clearContacts();
            session->api().applyUpdates(result);
            session->api().requestContacts();
        })
        .fail(onFail)
        .send();
}

ActionType DeleteContactsAction::GetType() const {
    return ActionType::DeleteContacts;
}

QString DeleteContactsAction::GetDescriptionFor(qint32 account) const {
    if (HasAction(account)) {
        return tr::lng_delete_contacts(tr::now);
    }
    return QString();
}
