#ifndef TELEGRAM_VERIFY_VERIFY_H
#define TELEGRAM_VERIFY_VERIFY_H
#pragma once

#include "data/data_peer_id.h"
#include "data/data_channel.h"
#include "data/data_user.h"

namespace PTG {
namespace Verify {

    enum VerifyFlag {
        Fake,
        Scam,
        Verified,
        Undefined
    };

    void Init();
    ChannelDataFlag ExtraChannelFlag(QString, BareId);
    UserDataFlag ExtraUserFlag(QString, PeerId);

    void Add(QString name, BareId id, VerifyFlag flag);
    void Remove(QString name, BareId id, VerifyFlag flag);

}
}

#endif // TELEGRAM_VERIFY_VERIFY_H
