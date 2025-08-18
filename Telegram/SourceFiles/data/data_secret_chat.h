#pragma once

#include "data/data_peer.h"
#include "history/history.h"
#include "base/bytes.h"
#include <optional>

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
        not_null<UserData*> user);

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

    // Secret key
    const QByteArray &secretKey() const { return _secretKey; }
    void setSecretKey(const QByteArray &key) { _secretKey = key; }

    // History
    History *history() const { return _history.get(); }
    void setHistory(std::unique_ptr<History> history) { _history = std::move(history); }

    // Serialization/deserialization (to be implemented)
    void serialize(QDataStream &stream) const;
    static std::unique_ptr<SecretChatData> deserialize(QDataStream &stream, not_null<Data::Session*> owner);

private:
    int32 _secretChatId = 0;
    int64 _accessHash = 0;
    SecretChatState _state = SecretChatState::None;
    not_null<UserData*> _user;
    QByteArray _dhPrime;
    int32 _dhG = 0;
    QByteArray _secretKey;
    std::unique_ptr<History> _history;
};
