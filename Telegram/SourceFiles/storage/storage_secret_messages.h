/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/cache/storage_cache_database.h"
#include "base/basic_types.h"
#include "base/bytes.h"

#include <QtCore/QByteArray>

class HistoryItem;

namespace Storage {

struct SecretMessage {
    MsgId id;
    QByteArray encryptedContent;
    TimeId timestamp;
    int32 chatId;
    
    [[nodiscard]] QByteArray serialize() const;
    [[nodiscard]] static std::optional<SecretMessage> deserialize(const QByteArray &data);
};

class SecretMessagesStorage {
public:
    explicit SecretMessagesStorage(Storage::Cache::Database &cache);
    
    void storeMessage(
        int32 chatId,
        MsgId msgId,
        const QByteArray &encryptedContent,
        TimeId timestamp,
        FnMut<void(Cache::Error)> &&done = nullptr);
    
    void getMessage(
        int32 chatId,
        MsgId msgId,
        FnMut<void(std::optional<SecretMessage>)> &&done);
    
    void removeMessage(
        int32 chatId,
        MsgId msgId,
        FnMut<void(Cache::Error)> &&done = nullptr);
    
    void getMessagesInRange(
        int32 chatId,
        MsgId fromId,
        MsgId toId,
        int limit,
        FnMut<void(std::vector<SecretMessage>)> &&done);
    
    void removeAllMessages(
        int32 chatId,
        FnMut<void(Cache::Error)> &&done = nullptr);
    
    void clearAll(FnMut<void(Cache::Error)> &&done = nullptr);

private:
    [[nodiscard]] Storage::Cache::Key makeMessageKey(int32 chatId, MsgId msgId) const;
    [[nodiscard]] Storage::Cache::Key makeChatKey(int32 chatId) const;
    [[nodiscard]] Storage::Cache::Key makeIndexKey(int32 chatId) const;
    
    void updateMessageIndex(
        int32 chatId,
        MsgId msgId,
        bool add,
        FnMut<void(Cache::Error)> &&done = nullptr);
    
    Storage::Cache::Database &_cache;
    
    static constexpr uint8 kSecretMessageTag = 0xE0;
    static constexpr uint8 kSecretIndexTag = 0xE1;
};

} // namespace Storage
