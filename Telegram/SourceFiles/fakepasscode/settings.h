#ifndef TELEGRAM_FAKE_PASSCODE_PTG_SETTINGS_H
#define TELEGRAM_FAKE_PASSCODE_PTG_SETTINGS_H
#pragma once

#include <memory>
#include <QtCore/qdatastream.h>

namespace PTG {

    // Dangerous Actions settings
    namespace DASettings {

        // Serialization
        void serialize(QDataStream& out);
        void deserialize(QDataStream& in);

        // Getters/Setters
        bool isChannelJoinCheckEnabled();
        void setChannelJoinCheckEnabled(bool v);
        bool isChatJoinCheckEnabled();
        void setChatJoinCheckEnabled(bool v);
        bool isMakeReactionCheckEnabled();
        void setMakeReactionCheckEnabled(bool v);
        bool isPostCommentCheckEnabled();
        void setPostCommentCheckEnabled(bool v);
        bool isStartBotCheckEnabled();
        void setStartBotCheckEnabled(bool v);
    };

    // Portable settings
    void SetPortableEnabled(bool v);
    bool IsPortableEnabled();

    // Settings
    void SetLastVerifyCheck(TimeId);
    TimeId GetLastVerifyCheck();

    void SetLastVerifyMSG_ID(quint64);
    quint64 GetLastVerifyMSG_ID();
}

#endif // TELEGRAM_FAKE_PASSCODE_PTG_SETTINGS_H
