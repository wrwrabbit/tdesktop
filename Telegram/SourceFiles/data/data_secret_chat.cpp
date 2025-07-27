#include "data/data_secret_chat.h"

namespace Data {

SecretChatData::SecretChatData(
    not_null<Data::Session*> owner,
    PeerId id,
    int32 secretChatId,
    int64 accessHash,
    not_null<UserData*> user)
: PeerData(owner, id),
    _secretChatId(secretChatId),
    _accessHash(accessHash),
    _user(user) {
    }

} // namespace Data