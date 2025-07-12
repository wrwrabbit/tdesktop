#pragma once

#include <memory>
#include <vector>
#include <cstdint>
#include <string>
#include <optional>

// Forward declarations for cryptographic primitives
namespace crl {
namespace crypto {
class DhKey;
class AesKey;
}
}

namespace MTP { // Begin MTP namespace

// Represents the state of a secret chat session
enum class SecretChatState {
    None,
    Requested,
    WaitingForAccept,
    Established,
    Terminated
};

// Holds key exchange data for a secret chat
struct SecretChatKeyExchange {
    std::vector<uint8_t> g_a; // Our public DH value
    std::vector<uint8_t> g_b; // Their public DH value
    std::vector<uint8_t> shared_key; // Shared secret
    uint64_t key_fingerprint = 0;
    bool is_initiator = false;
};

// Represents a secret chat session
class SecretChatSession {
public:
    SecretChatSession(int chat_id, bool initiator);

    // Initiate key exchange (as initiator)
    void initiateKeyExchange(const std::vector<uint8_t>& g_a);

    // Accept key exchange (as recipient)
    void acceptKeyExchange(const std::vector<uint8_t>& g_b, uint64_t key_fingerprint);

    // Complete key exchange (compute shared key)
    bool completeKeyExchange();

    // Get session state
    SecretChatState state() const;

    // Terminate session
    void terminate();

    // Getters
    int chatId() const;
    uint64_t keyFingerprint() const;
    const std::vector<uint8_t>& sharedKey() const;

private:
    int _chat_id;
    SecretChatState _state;
    SecretChatKeyExchange _key_exchange;

    // Internal helpers
    void computeSharedKey();
};

// Session manager for all secret chats
class SecretChatSessionManager {
public:
    SecretChatSessionManager();

    // Create a new session
    std::shared_ptr<SecretChatSession> createSession(int chat_id, bool initiator);

    // Get an existing session
    std::shared_ptr<SecretChatSession> getSession(int chat_id);

    // Remove a session
    void removeSession(int chat_id);

private:
    std::vector<std::shared_ptr<SecretChatSession>> _sessions;
};

} // namespace MTP
