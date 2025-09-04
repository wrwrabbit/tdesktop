/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/storage_secret_messages.h"

#include "storage/serialize_common.h"
#include "base/invoke_queued.h"
#include "logs.h"

#include <QtCore/QDataStream>

namespace Storage {
namespace {

constexpr auto kSecretMessagesVersion = 1;

} // namespace

QByteArray SecretMessage::serialize() const {
    auto stream = Serialize::ByteArrayWriter();
    stream << kSecretMessagesVersion;
    stream << id.bare;
    stream << encryptedContent;
    stream << timestamp;
    stream << chatId;
    return std::move(stream).result();
}

std::optional<SecretMessage> SecretMessage::deserialize(const QByteArray &data) {
    if (data.isEmpty()) {
        return std::nullopt;
    }
    
    auto stream = QDataStream(data);
    stream.setVersion(QDataStream::Qt_5_1);
    
    qint32 version = 0;
    stream >> version;
    if (version != kSecretMessagesVersion) {
        LOG(("Secret Message: Bad version %1").arg(version));
        return std::nullopt;
    }
    
    SecretMessage result;
    stream >> result.id.bare;
    stream >> result.encryptedContent;
    stream >> result.timestamp;
    stream >> result.chatId;
    
    if (stream.status() != QDataStream::Ok) {
        LOG(("Secret Message: Bad deserialize status"));
        return std::nullopt;
    }
    
    return result;
}

SecretMessagesStorage::SecretMessagesStorage(Storage::Cache::Database &cache)
: _cache(cache) {
}

void SecretMessagesStorage::storeMessage(
        int32 chatId,
        MsgId msgId,
        const QByteArray &encryptedContent,
        TimeId timestamp,
        FnMut<void(Cache::Error)> &&done) {
    
    const auto message = SecretMessage{
        .id = msgId,
        .encryptedContent = encryptedContent,
        .timestamp = timestamp,
        .chatId = chatId
    };
    
    const auto key = makeMessageKey(chatId, msgId);
    const auto data = message.serialize();
    
    auto weakDone = done 
        ? [done = std::move(done)](Cache::Error error) mutable {
            InvokeQueued(qApp, [done = std::move(done), error]() mutable {
                done(error);
            });
        }
        : FnMut<void(Cache::Error)>();
    
    _cache.put(
        key,
        Cache::Database::TaggedValue(QByteArray(data), kSecretMessageTag),
        [=, weakDone = std::move(weakDone)](Cache::Error error) mutable {
            if (error.type == Cache::Error::Type::None) {
                // Update the message index for this chat
                updateMessageIndex(chatId, msgId, true, std::move(weakDone));
            } else if (weakDone) {
                weakDone(error);
            }
        });
}

void SecretMessagesStorage::getMessage(
        int32 chatId,
        MsgId msgId,
        FnMut<void(std::optional<SecretMessage>)> &&done) {
    
    const auto key = makeMessageKey(chatId, msgId);
    
    _cache.get(key, [done = std::move(done)](QByteArray &&result) mutable {
        InvokeQueued(qApp, [done = std::move(done), result = std::move(result)]() mutable {
            done(SecretMessage::deserialize(result));
        });
    });
}

void SecretMessagesStorage::removeMessage(
        int32 chatId,
        MsgId msgId,
        FnMut<void(Cache::Error)> &&done) {
    
    const auto key = makeMessageKey(chatId, msgId);
    
    auto weakDone = done 
        ? [done = std::move(done)](Cache::Error error) mutable {
            InvokeQueued(qApp, [done = std::move(done), error]() mutable {
                done(error);
            });
        }
        : FnMut<void(Cache::Error)>();
    
    _cache.remove(key, [=, weakDone = std::move(weakDone)](Cache::Error error) mutable {
        if (error.type == Cache::Error::Type::None) {
            // Update the message index for this chat
            updateMessageIndex(chatId, msgId, false, std::move(weakDone));
        } else if (weakDone) {
            weakDone(error);
        }
    });
}

void SecretMessagesStorage::getMessagesInRange(
        int32 chatId,
        MsgId fromId,
        MsgId toId,
        int limit,
        FnMut<void(std::vector<SecretMessage>)> &&done) {
    
    // First get the message index for this chat
    const auto indexKey = makeIndexKey(chatId);
    
    _cache.get(indexKey, [=, done = std::move(done)](QByteArray &&indexData) mutable {
        std::vector<MsgId> messageIds;
        
        if (!indexData.isEmpty()) {
            auto stream = QDataStream(indexData);
            stream.setVersion(QDataStream::Qt_5_1);
            
            qint32 count = 0;
            stream >> count;
            
            for (int i = 0; i < count && i < limit; ++i) {
                MsgId msgId;
                stream >> msgId.bare;
                if (msgId >= fromId && msgId <= toId) {
                    messageIds.push_back(msgId);
                }
            }
        }
        
        // Now fetch the actual messages
        auto messages = std::make_shared<std::vector<SecretMessage>>();
        auto remaining = std::make_shared<int>(messageIds.size());
        
        if (messageIds.empty()) {
            InvokeQueued(qApp, [done = std::move(done), messages]() mutable {
                done(std::move(*messages));
            });
            return;
        }
        
        for (const auto msgId : messageIds) {
            getMessage(chatId, msgId, [=, done = std::move(done)](std::optional<SecretMessage> message) mutable {
                if (message) {
                    messages->push_back(*message);
                }
                
                if (--(*remaining) == 0) {
                    // Sort messages by ID
                    std::sort(messages->begin(), messages->end(), 
                        [](const SecretMessage &a, const SecretMessage &b) {
                            return a.id < b.id;
                        });
                    
                    if (done) {
                        done(std::move(*messages));
                        done = nullptr; // Prevent multiple calls
                    }
                }
            });
        }
    });
}

void SecretMessagesStorage::removeAllMessages(
        int32 chatId,
        FnMut<void(Cache::Error)> &&done) {
    
    // First get all message IDs for this chat
    const auto indexKey = makeIndexKey(chatId);
    
    _cache.get(indexKey, [=, done = std::move(done)](QByteArray &&indexData) mutable {
        std::vector<MsgId> messageIds;
        
        if (!indexData.isEmpty()) {
            auto stream = QDataStream(indexData);
            stream.setVersion(QDataStream::Qt_5_1);
            
            qint32 count = 0;
            stream >> count;
            
            for (int i = 0; i < count; ++i) {
                MsgId msgId;
                stream >> msgId.bare;
                messageIds.push_back(msgId);
            }
        }
        
        // Remove all messages and the index
        auto remaining = std::make_shared<int>(messageIds.size() + 1); // +1 for index
        auto hasError = std::make_shared<bool>(false);
        
        auto checkCompletion = [=, done = std::move(done)]() mutable {
            if (--(*remaining) == 0) {
                InvokeQueued(qApp, [done = std::move(done), hasError]() mutable {
                    done(*hasError ? Cache::Error{ Cache::Error::Type::IO, QString() } : Cache::Error::NoError());
                });
            }
        };
        
        // Remove the index
        _cache.remove(indexKey, [&checkCompletion, hasError](Cache::Error error) mutable {
            if (error.type != Cache::Error::Type::None) {
                *hasError = true;
            }
            checkCompletion();
        });
        
        // Remove all messages
        for (const auto msgId : messageIds) {
            const auto messageKey = makeMessageKey(chatId, msgId);
            _cache.remove(messageKey, [&checkCompletion, hasError](Cache::Error error) mutable {
                if (error.type != Cache::Error::Type::None) {
                    *hasError = true;
                }
                checkCompletion();
            });
        }
        
        if (messageIds.empty()) {
            checkCompletion(); // Only the index removal
        }
    });
}

void SecretMessagesStorage::clearAll(FnMut<void(Cache::Error)> &&done) {
    auto weakDone = done 
        ? [done = std::move(done)](Cache::Error error) mutable {
            InvokeQueued(qApp, [done = std::move(done), error]() mutable {
                done(error);
            });
        }
        : FnMut<void(Cache::Error)>();
    
    _cache.clearByTag(kSecretMessageTag, std::move(weakDone));
    _cache.clearByTag(kSecretIndexTag, nullptr);
}

Storage::Cache::Key SecretMessagesStorage::makeMessageKey(int32 chatId, MsgId msgId) const {
    // Use a specific tag for secret messages to avoid conflicts
    constexpr auto kSecretMessageTag = 0x5EC7E7000000ULL; // "SECRET" in hex-like format
    const auto high = kSecretMessageTag | ((uint64(chatId) & 0xFFFFFFULL) << 16);
    const auto low = uint64(msgId.bare);
    return Storage::Cache::Key{ high, low };
}

Storage::Cache::Key SecretMessagesStorage::makeChatKey(int32 chatId) const {
    // Use a different tag for chat metadata
    constexpr auto kSecretChatTag = 0x5EC7E7111111ULL;
    const auto high = kSecretChatTag | ((uint64(chatId) & 0xFFFFFFULL) << 16);
    const auto low = 0ULL; // No message ID for chat metadata
    return Storage::Cache::Key{ high, low };
}

Storage::Cache::Key SecretMessagesStorage::makeIndexKey(int32 chatId) const {
    // Use a different tag for message indices
    constexpr auto kSecretIndexTag = 0x5EC7E7222222ULL;
    const auto high = kSecretIndexTag | ((uint64(chatId) & 0xFFFFFFULL) << 16);
    const auto low = 0ULL; // No message ID for indices
    return Storage::Cache::Key{ high, low };
}

void SecretMessagesStorage::updateMessageIndex(
        int32 chatId,
        MsgId msgId,
        bool add,
        FnMut<void(Cache::Error)> &&done) {
    
    const auto indexKey = makeIndexKey(chatId);
    
    _cache.get(indexKey, [=, done = std::move(done)](QByteArray &&indexData) mutable {
        std::vector<MsgId> messageIds;
        
        // Read existing index
        if (!indexData.isEmpty()) {
            auto stream = QDataStream(indexData);
            stream.setVersion(QDataStream::Qt_5_1);
            
            qint32 count = 0;
            stream >> count;
            
            for (int i = 0; i < count; ++i) {
                MsgId existingId;
                stream >> existingId.bare;
                if (existingId != msgId) { // Skip the ID we're adding/removing
                    messageIds.push_back(existingId);
                }
            }
        }
        
        // Add the new message ID if requested
        if (add) {
            messageIds.push_back(msgId);
            // Sort to maintain order
            std::sort(messageIds.begin(), messageIds.end());
        }
        
        // Write updated index
        auto stream = Serialize::ByteArrayWriter();
        stream << static_cast<qint32>(messageIds.size());
        for (const auto id : messageIds) {
            stream << id.bare;
        }
        
        const auto newIndexData = std::move(stream).result();
        
        _cache.put(
            indexKey,
            Cache::Database::TaggedValue(QByteArray(newIndexData), kSecretIndexTag),
            [done = std::move(done)](Cache::Error error) mutable {
                if (done) {
                    InvokeQueued(qApp, [done = std::move(done), error]() mutable {
                        done(error);
                    });
                }
            });
    });
}

} // namespace Storage
