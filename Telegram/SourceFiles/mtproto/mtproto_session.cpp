#include "mtproto_session.h"
#include <algorithm>
#include <cstring>

namespace MTP { // Begin MTP namespace

// --- SecretChatSession ---

SecretChatSession::SecretChatSession(int chat_id, bool initiator)
    : _chat_id(chat_id)
    , _state(SecretChatState::None)
{
    _key_exchange.is_initiator = initiator;
    if (initiator) {
        _state = SecretChatState::Requested;
    }
}

void SecretChatSession::initiateKeyExchange(const std::vector<uint8_t>& g_a) {
    if (!_key_exchange.is_initiator) return;
    _key_exchange.g_a = g_a;
    _state = SecretChatState::WaitingForAccept;
}

void SecretChatSession::acceptKeyExchange(const std::vector<uint8_t>& g_b, uint64_t key_fingerprint) {
    if (_key_exchange.is_initiator) return;
    _key_exchange.g_b = g_b;
    _key_exchange.key_fingerprint = key_fingerprint;
    _state = SecretChatState::WaitingForAccept;
}

bool SecretChatSession::completeKeyExchange() {
    // Both g_a and g_b must be set
    if (_key_exchange.g_a.empty() || _key_exchange.g_b.empty())
        return false;

    computeSharedKey();
    _state = SecretChatState::Established;
    return true;
}

SecretChatState SecretChatSession::state() const {
    return _state;
}

void SecretChatSession::terminate() {
    _state = SecretChatState::Terminated;
    _key_exchange = SecretChatKeyExchange(); // Clear key exchange data
}

int SecretChatSession::chatId() const {
    return _chat_id;
}

uint64_t SecretChatSession::keyFingerprint() const {
    return _key_exchange.key_fingerprint;
}

const std::vector<uint8_t>& SecretChatSession::sharedKey() const {
    return _key_exchange.shared_key;
}

// Dummy implementation for shared key computation
void SecretChatSession::computeSharedKey() {
    // In a real implementation, use DH math to compute the shared key.
    // Here, just concatenate g_a and g_b for placeholder.
    _key_exchange.shared_key.clear();
    _key_exchange.shared_key.insert(_key_exchange.shared_key.end(),
                                    _key_exchange.g_a.begin(), _key_exchange.g_a.end());
    _key_exchange.shared_key.insert(_key_exchange.shared_key.end(),
                                    _key_exchange.g_b.begin(), _key_exchange.g_b.end());
    // Compute a dummy fingerprint (e.g., first 8 bytes as uint64_t)
    if (_key_exchange.shared_key.size() >= 8) {
        std::memcpy(&_key_exchange.key_fingerprint, _key_exchange.shared_key.data(), 8);
    } else {
        _key_exchange.key_fingerprint = 0;
    }
}

// --- SecretChatSessionManager ---

SecretChatSessionManager::SecretChatSessionManager() = default;

std::shared_ptr<SecretChatSession> SecretChatSessionManager::createSession(int chat_id, bool initiator) {
    auto session = std::make_shared<SecretChatSession>(chat_id, initiator);
    _sessions.push_back(session);
    return session;
}

std::shared_ptr<SecretChatSession> SecretChatSessionManager::getSession(int chat_id) {
    auto it = std::find_if(_sessions.begin(), _sessions.end(),
        [chat_id](const std::shared_ptr<SecretChatSession>& s) {
            return s->chatId() == chat_id;
        });
    if (it != _sessions.end()) {
        return *it;
    }
    return nullptr;
}

void SecretChatSessionManager::removeSession(int chat_id) {
    _sessions.erase(
        std::remove_if(_sessions.begin(), _sessions.end(),
            [chat_id](const std::shared_ptr<SecretChatSession>& s) {
                return s->chatId() == chat_id;
            }),
        _sessions.end());
}

} // namespace MTP