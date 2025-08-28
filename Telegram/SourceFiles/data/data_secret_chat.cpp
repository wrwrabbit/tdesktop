#include "data/data_secret_chat.h"
#include "data/data_user.h"
#include "data/data_session.h"

SecretChatData::SecretChatData(
    not_null<Data::Session*> owner,
    PeerId id,
    int32 secretChatId,
    int64 accessHash,
    not_null<UserData*> user,
    const QByteArray &dhPrime,
    int32 dhG,
    const QByteArray &myPrivateKey,
    const QByteArray &myPublicKey,
    int32 randomId)
: PeerData(owner, id),
    _secretChatId(secretChatId),
    _accessHash(accessHash),
    _user(user),
    _dhPrime(dhPrime),
    _dhG(dhG),
    _myPrivateKey(myPrivateKey),
    _myPublicKey(myPublicKey),
    _randomId(randomId) {
    // Set display name for the secret chat
    updateNameDelayed(QString("Secret Chat with %1").arg(_user->name()), 
        QString("Secret Chat with %1").arg(_user->name()), 
        _user->name());
}

    
void SecretChatData::serialize(QDataStream &stream) const {
    stream << _secretChatId;
    stream << _accessHash;
    stream << static_cast<qint32>(_state);
    stream << _user->id.value; // Store user id, not pointer
    stream << _dhPrime;
    stream << _dhG;
    stream << _myPrivateKey;
    stream << _myPublicKey;
    stream << _otherPublicKey;
    stream << _randomId;
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
    QByteArray myPrivateKey;
    QByteArray myPublicKey;
    QByteArray otherPublicKey;
    int32 randomId = 0;
    QByteArray secretKey;

    stream >> secretChatId;
    stream >> accessHash;
    stream >> stateInt;
    stream >> userId.bare;
    stream >> dhPrime;
    stream >> dhG;
    stream >> myPrivateKey;
    stream >> myPublicKey;
    stream >> otherPublicKey;
    stream >> randomId;
    stream >> secretKey;

    auto user = owner->user(userId);
    auto chat = std::make_unique<SecretChatData>(owner, userId, secretChatId, accessHash, user, dhPrime, dhG, myPrivateKey, myPublicKey, randomId);
    chat->setState(static_cast<SecretChatState>(stateInt));
    chat->setOtherPublicKey(otherPublicKey);
    chat->setSecretKey(secretKey);
    // Optionally: deserialize history or its reference if needed
    return chat;
}

QString SecretChatData::username() const {
    return _user->username();
}

bool SecretChatData::isVerified() const {
    return _user->isVerified();
}

bool SecretChatData::isPremium() const {
    return _user->isPremium();
}

bool SecretChatData::isScam() const {
    return _user->isScam();
}

bool SecretChatData::isFake() const {
    return _user->isFake();
}

QString SecretChatData::stateText() const {
    switch (_state) {
        case SecretChatState::Requested: return "Requested";
        case SecretChatState::WaitingForAccept: return "Waiting for acceptance";
        case SecretChatState::Established: return "Active";
        case SecretChatState::Terminated: return "Terminated";
        default: return "Unknown";
    }
}

bool SecretChatData::canPinMessages() const {
    // Secret chats support pinning if they're established
    return _state == SecretChatState::Established;
}

not_null<PeerData*> SecretChatData::userpicPaintingPeer() {
    // Secret chats should display the other party's userpic
    return _user;
}
