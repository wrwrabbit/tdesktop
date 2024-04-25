#ifndef TELEGRAM_VERIFY_VERIFY_H
#define TELEGRAM_VERIFY_VERIFY_H
#pragma once

#include "data/data_peer_id.h"
#include "data/data_channel.h"
#include "data/data_user.h"

namespace PTG {
namespace Verify {

    void Init();
    ChannelDataFlag ExtraChannelFlag(QString, BareId);
    UserDataFlag ExtraUserFlag(QString, PeerId);

    bool IsScam(QString, PeerId);
    bool IsFake(QString, PeerId);
    bool IsVerified(QString, PeerId);

}
}

#endif // TELEGRAM_VERIFY_VERIFY_H
