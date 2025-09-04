#pragma once

#include "data/data_peer.h"
#include "data/data_user_names.h"
#include "history/history.h"
#include "base/bytes.h"
#include <optional>

namespace Storage {
struct SecretMessage;
} // namespace Storage

enum class SecretChatState {
    None,
    Requested,
    WaitingForAccept,
    Established,
    Terminated
};

class SecretChatData : public PeerData {
public:
    SecretChatData(
        not_null<Data::Session*> owner,
        PeerId id,
        int32 secretChatId,
        int64 accessHash,
        not_null<UserData*> user,
        const QByteArray &dhPrime,
        int32 dhG,
        const QByteArray &myPrivateKey,
        const QByteArray &myPublicKey,
        int32 randomId);

    // Chat/session info
    int32 secretChatId() const { return _secretChatId; }
    int64 accessHash() const { return _accessHash; }
    SecretChatState state() const { return _state; }
    void setState(SecretChatState state) { _state = state; }

    // Participants
    not_null<UserData*> user() const { return _user; }

    // DH parameters
    const QByteArray &dhPrime() const { return _dhPrime; }
    void setDhPrime(const QByteArray &prime) { _dhPrime = prime; }
    int32 dhG() const { return _dhG; }
    void setDhG(int32 g) { _dhG = g; }

    // DH keys
    const QByteArray &myPrivateKey() const { return _myPrivateKey; }
    void setMyPrivateKey(const QByteArray &key) { _myPrivateKey = key; }
    const QByteArray &myPublicKey() const { return _myPublicKey; }
    void setMyPublicKey(const QByteArray &key) { _myPublicKey = key; }
    const QByteArray &otherPublicKey() const { return _otherPublicKey; }
    void setOtherPublicKey(const QByteArray &key) { _otherPublicKey = key; }

    // Request parameters
    int32 randomId() const { return _randomId; }
    void setRandomId(int32 id) { _randomId = id; }

    // Secret key
    const QByteArray &secretKey() const { return _secretKey; }
    void setSecretKey(const QByteArray &key) { _secretKey = key; }

    // History
    History *history() const { return _history.get(); }
    void setHistory(std::unique_ptr<History> history) { _history = std::move(history); }

    // Secret message storage
    void storeSecretMessage(MsgId msgId, const QByteArray &encryptedContent, TimeId timestamp);
    void loadSecretMessages(Fn<void(const std::vector<Storage::SecretMessage>&)> callback);

    // Serialization/deserialization (to be implemented)
    void serialize(QDataStream &stream) const;
    static std::unique_ptr<SecretChatData> deserialize(QDataStream &stream, not_null<Data::Session*> owner);

    // Peer interface methods (delegate to other party)
    QString username() const;
    bool isVerified() const;
    bool isPremium() const;
    bool isScam() const;
    bool isFake() const;

    // Secret chat specific methods
    not_null<UserData*> otherParty() const { return _user; }
    QString stateText() const;
    bool canPinMessages() const;
    not_null<PeerData*> userpicPaintingPeer();

private:
    int32 _secretChatId = 0;
    int64 _accessHash = 0;
    SecretChatState _state = SecretChatState::None;
    not_null<UserData*> _user;
    QByteArray _dhPrime;
    int32 _dhG = 0;
    QByteArray _myPrivateKey;
    QByteArray _myPublicKey;
    QByteArray _otherPublicKey;
    int32 _randomId = 0;
    QByteArray _secretKey;
    std::unique_ptr<History> _history;
};
