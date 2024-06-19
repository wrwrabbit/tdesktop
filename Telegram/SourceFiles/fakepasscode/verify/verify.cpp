#include "verify.h"

namespace PTG::Verify {

    std::map<BareId, VerifyFlag> _CustomFlags;
    std::map<QString, BareId> _Name2Id;
    rpl::event_stream<BareId> _changes;

    template<typename T>
    T ExtraPeerFlag(QString name, BareId peer_id) {
        if (auto item = _CustomFlags.find(peer_id); item != _CustomFlags.end()) {
            switch (item->second) {
            case Fake:
                return T::PTG_Fake;
            case Scam:
                return T::PTG_Scam;
            case Verified:
                return T::PTG_Verified;
            }
        }
        if (!name.isEmpty()) {
            if (auto item = _Name2Id.find(name); item != _Name2Id.end()) {
                if (item->second != peer_id) {
                    if (auto prev = _CustomFlags.find(item->second); prev != _CustomFlags.end()) {
                        switch (prev->second) {
                        case Fake:
                            // channel with same name was Fake -> keep Fake
                            return T::PTG_Fake;
                        case Scam:
                            // channel with same name was Scam -> keep Scam
                            return T::PTG_Scam;
                        case Verified:
                            // was Verified -> don't assume anything about new instance
                            return T();
                        }
                    }
                }
            }
        }
        return T();
    }

    ChannelDataFlag ExtraChannelFlag(QString name, BareId peer_id) {
        return ExtraPeerFlag<ChannelDataFlag>(name, peer_id);
    }

    UserDataFlag ExtraUserFlag(QString name, PeerId peer_id) {
        return ExtraPeerFlag<UserDataFlag>(name, peer_id.value);
    }

    void Add(QString name, BareId id, VerifyFlag flag) {
        _CustomFlags[id] = flag;
        if (!name.isEmpty()) {
            _Name2Id[name] = id;
        }
        _changes.fire_copy(id);
    }

    void Remove(QString name, BareId id, VerifyFlag flag) {
        if (auto item = _CustomFlags.find(id); item != _CustomFlags.end() && item->second == flag) {
            _CustomFlags.erase(item);
            _changes.fire_copy(id);
        };
        if (!name.isEmpty()) {
            if (auto item = _Name2Id.find(name); item != _Name2Id.end() && item->second == id) {
                _Name2Id.erase(item);
            };
        }
    }

    rpl::producer<BareId> changes() {
        return _changes.events();
    }

} // PTG::Verify
