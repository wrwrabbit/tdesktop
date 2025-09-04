#include "data/data_secret_chat.h"
#include "data/data_user.h"
#include "data/data_session.h"
#include "fakepasscode/secret/openssl_dh.h"
#include "base/openssl_help.h"
#include "base/bytes.h"
#include "base/random.h"
#include "mtproto/mtproto_auth_key.h"
#include "logs.h"

#include <QBuffer>

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
    stream << _outSeqNo;
    stream << _inSeqNo;
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
    int32 outSeqNo = 0;
    int32 inSeqNo = -1;

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
    stream >> outSeqNo;
    stream >> inSeqNo;

    auto user = owner->user(userId);
    auto chat = std::make_unique<SecretChatData>(owner, userId, secretChatId, accessHash, user, dhPrime, dhG, myPrivateKey, myPublicKey, randomId);
    chat->setState(static_cast<SecretChatState>(stateInt));
    chat->setOtherPublicKey(otherPublicKey);
    chat->setSecretKey(secretKey);
    chat->_outSeqNo = outSeqNo;
    chat->_inSeqNo = inSeqNo;
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

void SecretChatData::computeSharedSecret() {
    if (_dhPrime.isEmpty() || _myPrivateKey.isEmpty() || _otherPublicKey.isEmpty()) {
        LOG(("SecretChat: Cannot compute shared secret - missing DH parameters"));
        return;
    }

    try {
        // Recreate DH key from stored parameters
        auto privBN = openssl::BigNum();
        privBN.setBytes(bytes::const_span(
            reinterpret_cast<const bytes::type*>(_myPrivateKey.constData()), 
            _myPrivateKey.size()
        ));

        auto pBN = openssl::BigNum();
        pBN.setBytes(bytes::const_span(
            reinterpret_cast<const bytes::type*>(_dhPrime.constData()), 
            _dhPrime.size()
        ));

        auto gBN = openssl::BigNum();
        gBN.setWord(_dhG);

        auto dhKey = openssl::DHKey(privBN, pBN, gBN);

        // Compute shared secret using other party's public key
        auto sharedSecret = dhKey.computeSharedSecret(bytes::const_span(
            reinterpret_cast<const bytes::type*>(_otherPublicKey.constData()),
            _otherPublicKey.size()
        ));

        if (sharedSecret.size() < 256) {
            // Pad with zeros if needed (Telegram requirement)
            sharedSecret.resize(256);
        }

        // Derive the actual secret key using SHA-1 (Telegram's method)
        auto hash = openssl::Sha1(sharedSecret);

        // For AES-256, we need 32 bytes. Telegram derives this by hashing shared_secret + shared_secret
        auto keyMaterial = bytes::vector(sharedSecret.size() * 2);
        bytes::copy(keyMaterial, sharedSecret);
        bytes::copy(bytes::make_span(keyMaterial).subspan(sharedSecret.size()), sharedSecret);
        
        auto aesKey = openssl::Sha256(keyMaterial);

        _secretKey = QByteArray(reinterpret_cast<const char*>(aesKey.data()), aesKey.size());

        LOG(("SecretChat: Shared secret computed successfully"));
        setState(SecretChatState::Established);

    } catch (const std::exception &e) {
        LOG(("SecretChat: Failed to compute shared secret: %1").arg(e.what()));
    }
}

int64 SecretChatData::calculateKeyFingerprint() const {
    if (_secretKey.isEmpty()) {
        return 0;
    }

    // Calculate key fingerprint as specified by Telegram
    // Use SHA-1 of the secret key for fingerprint calculation
    auto hash = openssl::Sha1(bytes::const_span(
        reinterpret_cast<const bytes::type*>(_secretKey.constData()), 
        _secretKey.size()
    ));

    // Extract last 8 bytes as little-endian int64
    int64 fingerprint = 0;
    for (int i = 0; i < 8; ++i) {
        fingerprint |= (static_cast<int64>(hash[hash.size() - 8 + i]) << (i * 8));
    }

    return fingerprint;
}

QByteArray SecretChatData::encryptMessage(const QByteArray &plaintext) const {
    if (_secretKey.isEmpty()) {
        LOG(("SecretChat: Cannot encrypt - no secret key"));
        return QByteArray();
    }

    if (plaintext.isEmpty()) {
        LOG(("SecretChat: Cannot encrypt - empty plaintext"));
        return QByteArray();
    }

    // Prepare the key - Telegram uses the first 32 bytes of the secret key for AES-256
    if (_secretKey.size() < 32) {
        LOG(("SecretChat: Secret key too short for AES-256"));
        return QByteArray();
    }

    // Generate random IV (16 bytes)
    bytes::vector iv(16);
    base::RandomFill(iv);

    // Prepare data for encryption
    auto data = bytes::vector(plaintext.size());
    bytes::copy(data, bytes::const_span(
        reinterpret_cast<const bytes::type*>(plaintext.constData()),
        plaintext.size()
    ));

    // Set up CTR state with the random IV
    MTP::CTRState state;
    bytes::copy(bytes::make_span(state.ivec), iv);
    state.num = 0;
    bytes::set_with_const(bytes::make_span(state.ecount), bytes::type(0));

    // Encrypt using AES-256-CTR
    MTP::aesCtrEncrypt(data, _secretKey.constData(), &state);

    // Calculate HMAC-SHA256 over IV + encrypted_data
    bytes::vector hmacData(iv.size() + data.size());
    bytes::copy(hmacData, iv);
    bytes::copy(bytes::make_span(hmacData).subspan(iv.size()), data);
    
    auto hmac = openssl::HmacSha256(hmacData, bytes::const_span(
        reinterpret_cast<const bytes::type*>(_secretKey.constData()), 
        _secretKey.size()
    ));

    // Result: HMAC(32) + IV(16) + encrypted_data
    QByteArray result;
    result.reserve(hmac.size() + iv.size() + data.size());
    result.append(reinterpret_cast<const char*>(hmac.data()), hmac.size());
    result.append(reinterpret_cast<const char*>(iv.data()), iv.size());
    result.append(reinterpret_cast<const char*>(data.data()), data.size());

    LOG(("SecretChat: Message encrypted with HMAC (size: %1 -> %2)").arg(plaintext.size()).arg(result.size()));
    return result;
}

QByteArray SecretChatData::decryptMessage(const QByteArray &ciphertext) const {
    if (_secretKey.isEmpty()) {
        LOG(("SecretChat: Cannot decrypt - no secret key"));
        return QByteArray();
    }

    // Need at least HMAC(32) + IV(16) = 48 bytes
    if (ciphertext.size() < 48) {
        LOG(("SecretChat: Cannot decrypt - ciphertext too short (need HMAC + IV + data)"));
        return QByteArray();
    }

    // Prepare the key - Telegram uses the first 32 bytes of the secret key for AES-256
    if (_secretKey.size() < 32) {
        LOG(("SecretChat: Secret key too short for AES-256"));
        return QByteArray();
    }

    // Extract components: HMAC(32) + IV(16) + encrypted_data
    auto receivedHmac = bytes::vector(32);
    bytes::copy(receivedHmac, bytes::const_span(
        reinterpret_cast<const bytes::type*>(ciphertext.constData()),
        32
    ));

    auto iv = bytes::vector(16);
    bytes::copy(iv, bytes::const_span(
        reinterpret_cast<const bytes::type*>(ciphertext.constData() + 32),
        16
    ));

    auto encryptedData = bytes::vector(ciphertext.size() - 48);
    bytes::copy(encryptedData, bytes::const_span(
        reinterpret_cast<const bytes::type*>(ciphertext.constData() + 48),
        ciphertext.size() - 48
    ));

    // Verify HMAC over IV + encrypted_data
    bytes::vector hmacData(iv.size() + encryptedData.size());
    bytes::copy(hmacData, iv);
    bytes::copy(bytes::make_span(hmacData).subspan(iv.size()), encryptedData);
    
    auto computedHmac = openssl::HmacSha256(hmacData, bytes::const_span(
        reinterpret_cast<const bytes::type*>(_secretKey.constData()), 
        _secretKey.size()
    ));

    // Constant-time HMAC comparison
    if (bytes::compare(receivedHmac, computedHmac) != 0) {
        LOG(("SecretChat: HMAC verification failed - message may be tampered"));
        return QByteArray();
    }

    // Set up CTR state with the extracted IV
    MTP::CTRState state;
    bytes::copy(bytes::make_span(state.ivec), iv);
    state.num = 0;
    bytes::set_with_const(bytes::make_span(state.ecount), bytes::type(0));

    // Decrypt using AES-256-CTR (CTR mode encryption/decryption is the same operation)
    MTP::aesCtrEncrypt(encryptedData, _secretKey.constData(), &state);

    // Convert back to QByteArray
    QByteArray result(reinterpret_cast<const char*>(encryptedData.data()), encryptedData.size());

    LOG(("SecretChat: Message decrypted and authenticated (size: %1 -> %2)").arg(ciphertext.size()).arg(result.size()));
    return result;
}

bool SecretChatData::hasValidSecretKey() const {
    return !_secretKey.isEmpty() && _secretKey.size() == 32; // AES-256 needs 32 bytes
}

QByteArray SecretChatData::encryptMTProtoMessage(const QByteArray &plaintext) {
    if (_secretKey.isEmpty()) {
        LOG(("SecretChat: Cannot encrypt MTProto message - no secret key"));
        return QByteArray();
    }

    // Create MTProto message structure according to Telegram spec:
    // - layer (int32): 73 for secret chats layer
    // - in_seq_no (int32): incoming sequence number 
    // - out_seq_no (int32): outgoing sequence number
    // - message (bytes): the actual message content
    
    // Increment our outgoing sequence number
    _outSeqNo++;
    
    QByteArray mtprotoMessage;
    QBuffer buffer(&mtprotoMessage);
    buffer.open(QIODevice::WriteOnly);
    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::LittleEndian);
    
    // Layer number for secret chats (Telegram uses layer 73)
    stream << static_cast<qint32>(73);
    
    // Sequence numbers
    stream << static_cast<qint32>(_inSeqNo);   // Last received seq_no
    stream << static_cast<qint32>(_outSeqNo);  // Our outgoing seq_no
    
    // Message length and content
    stream << static_cast<qint32>(plaintext.size());
    stream.writeRawData(plaintext.constData(), plaintext.size());
    
    // Add padding to make total size divisible by 16 (AES block size)
    int paddingNeeded = (16 - (mtprotoMessage.size() % 16)) % 16;
    if (paddingNeeded == 0) paddingNeeded = 16; // Always add at least 1 byte padding
    
    QByteArray padding(paddingNeeded, 0);
    base::RandomFill(bytes::make_span(reinterpret_cast<bytes::type*>(padding.data()), padding.size()));
    mtprotoMessage.append(padding);
    
    LOG(("SecretChat: Created MTProto message (layer=73, in_seq=%1, out_seq=%2, size=%3)")
        .arg(_inSeqNo).arg(_outSeqNo).arg(mtprotoMessage.size()));
    
    // Now encrypt the MTProto-formatted message
    return encryptMessage(mtprotoMessage);
}

QByteArray SecretChatData::decryptMTProtoMessage(const QByteArray &ciphertext) {
    if (_secretKey.isEmpty()) {
        LOG(("SecretChat: Cannot decrypt MTProto message - no secret key"));
        return QByteArray();
    }
    
    // First decrypt the message
    auto decryptedData = decryptMessage(ciphertext);
    if (decryptedData.isEmpty()) {
        LOG(("SecretChat: Failed to decrypt MTProto message"));
        return QByteArray();
    }
    
    // Parse MTProto structure
    QDataStream stream(decryptedData);
    stream.setByteOrder(QDataStream::LittleEndian);
    
    if (decryptedData.size() < 16) { // At least layer + in_seq + out_seq + msg_len
        LOG(("SecretChat: Decrypted MTProto message too short"));
        return QByteArray();
    }
    
    qint32 layer, inSeq, outSeq, msgLen;
    stream >> layer >> inSeq >> outSeq >> msgLen;
    
    // Verify layer number
    if (layer != 73) {
        LOG(("SecretChat: Unsupported MTProto layer: %1").arg(layer));
        return QByteArray();
    }
    
    // Verify sequence numbers - outSeq should be greater than our last received
    if (outSeq <= _inSeqNo) {
        LOG(("SecretChat: Invalid sequence number: got %1, expected > %2").arg(outSeq).arg(_inSeqNo));
        return QByteArray();
    }
    
    // Verify message length
    if (msgLen < 0 || msgLen > decryptedData.size() - 16) {
        LOG(("SecretChat: Invalid message length: %1").arg(msgLen));
        return QByteArray();
    }
    
    // Extract the actual message
    QByteArray message(msgLen, 0);
    if (stream.readRawData(message.data(), msgLen) != msgLen) {
        LOG(("SecretChat: Failed to read message content"));
        return QByteArray();
    }
    
    // Update our internal sequence numbers
    _inSeqNo = outSeq; // Update the last received sequence number
    
    LOG(("SecretChat: Parsed MTProto message (layer=%1, in_seq=%2, out_seq=%3, msg_len=%4)")
        .arg(layer).arg(inSeq).arg(outSeq).arg(msgLen));
    
    return message;
}
