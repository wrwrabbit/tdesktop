#include "verify.h"

namespace PTG {
namespace Verify {

    std::map<BareId, VerifyFlag> _CustomFlags;
    std::map<QString, BareId> _Name2Id;

    ChannelDataFlag ExtraChannelFlag(QString name, BareId peer_id) {
        if (auto item = _Name2Id.find(name); item != _Name2Id.end()) {
            if (item->second != peer_id) {
                // incorrect id? with old known name
                return ChannelDataFlag::PTG_Scam;
            }
        }
        if (auto item = _CustomFlags.find(peer_id); item != _CustomFlags.end()) {
            switch (item->second) {
            case Fake:
                return ChannelDataFlag::PTG_Fake;
            case Scam:
                return ChannelDataFlag::PTG_Scam;
            case Verified:
                return ChannelDataFlag::PTG_Verified;
            }
        }
        return ChannelDataFlag();
    }

    UserDataFlag ExtraUserFlag(QString name, PeerId peer_id) {
        if (auto item = _Name2Id.find(name); item != _Name2Id.end()) {
            if (item->second != peer_id.value) {
                // incorrect id? with old known name
                return UserDataFlag::PTG_Scam;
            }
        }
        if (auto item = _CustomFlags.find(peer_id.value); item != _CustomFlags.end()) {
            switch (item->second) {
            case Fake:
                return UserDataFlag::PTG_Fake;
            case Scam:
                return UserDataFlag::PTG_Scam;
            case Verified:
                return UserDataFlag::PTG_Verified;
            }
        }
        return UserDataFlag();
    }

    void Add(QString name, BareId id, VerifyFlag flag) {
        _CustomFlags[id] = flag;
        if (!name.isEmpty()) {
            _Name2Id[name] = id;
        }
    }

    void Remove(QString name, BareId id, VerifyFlag flag) {
        if (auto item = _CustomFlags.find(id); item != _CustomFlags.end() && item->second == flag) {
            _CustomFlags.erase(item);
        };
        if (!name.isEmpty()) {
            if (auto item = _Name2Id.find(name); item != _Name2Id.end() && item->second == id) {
                _Name2Id.erase(item);
            };
        }
    }

    void Init()
    {
        Add("cpartisans_by", 1224880559, VerifyFlag::Verified);
        Add("cpartisans_security", 1164492294, VerifyFlag::Verified);
        Add("cpartisans_chat", 1297930553, VerifyFlag::Verified);
        Add("cpartisans_sec_chat", 1394174706, VerifyFlag::Verified);
    }

}
}
