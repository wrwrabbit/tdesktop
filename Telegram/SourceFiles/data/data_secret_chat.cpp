#include "data/data_secret_chat.h"
#include "data/data_user.h"
#include "data/data_session.h"

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

    
void SecretChatData::serialize(QDataStream &stream) const {
    stream << _secretChatId;
    stream << _accessHash;
    stream << static_cast<qint32>(_state);
    stream << _user->id.value; // Store user id, not pointer
    stream << _dhPrime;
    stream << _dhG;
    stream << _secretKey;
    // Optionally: serialize history or its reference if needed
}

std::unique_ptr<SecretChatData> SecretChatData::deserialize(QDataStream &stream, not_null<Data::Session*> owner) {
    int32 secretChatId = 0;
    int64 accessHash = 0;
    qint32 stateInt = 0;
    UserId userId;
    QByteArray dhPrime;
    int32 dhG = 0;
    QByteArray secretKey;

    stream >> secretChatId;
    stream >> accessHash;
    stream >> stateInt;
    stream >> userId.bare;
    stream >> dhPrime;
    stream >> dhG;
    stream >> secretKey;

    auto user = owner->user(userId);
    auto chat = std::make_unique<SecretChatData>(owner, userId, secretChatId, accessHash, user);
    chat->setState(static_cast<SecretChatState>(stateInt));
    chat->setDhPrime(dhPrime);
    chat->setDhG(dhG);
    chat->setSecretKey(secretKey);
    // Optionally: deserialize history or its reference if needed
    return chat;
}
