#include "ptg.h"

#include "core/application.h"
#include "main/main_session.h"
#include "main/main_domain.h"
#include "storage/storage_domain.h"

#include "action.h"
#include "actions/logout.h"

namespace PTG {

    namespace DASettings {
        bool ChannelJoinCheck = false;
        bool ChatJoinCheck = false;
        bool MakeReactionCheck = false;
        bool PostCommentCheck = false;
        bool StartBotCheck = false;

        void serialize(QDataStream& out) {
            out << ChannelJoinCheck
                << ChatJoinCheck
                << MakeReactionCheck
                << PostCommentCheck
                << StartBotCheck;
        }

        void deserialize(QDataStream& in) {
            in >> ChannelJoinCheck
                >> ChatJoinCheck
                >> MakeReactionCheck
                >> PostCommentCheck
                >> StartBotCheck;
        }
        
        // Getters/Setters
        bool isChannelJoinCheckEnabled() { return ChannelJoinCheck; }
        void setChannelJoinCheckEnabled(bool v) { ChannelJoinCheck = v; }

        bool isChatJoinCheckEnabled() { return ChatJoinCheck; }
        void setChatJoinCheckEnabled(bool v) { ChatJoinCheck = v; }

        bool isMakeReactionCheckEnabled() { return MakeReactionCheck; }
        void setMakeReactionCheckEnabled(bool v) { MakeReactionCheck = v; }

        bool isPostCommentCheckEnabled() { return PostCommentCheck; }
        void setPostCommentCheckEnabled(bool v) { PostCommentCheck = v; }

        bool isStartBotCheckEnabled() { return StartBotCheck; }
        void setStartBotCheckEnabled(bool v) { StartBotCheck = v; }
    }


    TimeId vLastVerifyCheck = 0;
    void SetLastVerifyCheck(TimeId v) {
        vLastVerifyCheck = v;
    }
    TimeId GetLastVerifyCheck() {
        return vLastVerifyCheck;
    }

    quint64 vLastVerifyMSG_ID = 0;
    void SetLastVerifyMSG_ID(quint64 v) {
        vLastVerifyMSG_ID = v;
    }
    quint64 GetLastVerifyMSG_ID() {
        return vLastVerifyMSG_ID;
    }
};

