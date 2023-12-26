#ifndef TDESKTOP_DELETE_CHATS_H
#define TDESKTOP_DELETE_CHATS_H

#include "fakepasscode/multiaccount_action.hpp"

namespace FakePasscode {
    struct SelectPeersData {
        base::flat_set<quint64> peer_ids;
    };

    class DeleteChatsAction final : public MultiAccountAction<SelectPeersData> {
    public:
        using MultiAccountAction::MultiAccountAction;
        void ExecuteAccountAction(int index, Main::Account* account, const SelectPeersData& action) override;
        ActionType GetType() const override;
    };

    template<class Stream>
    Stream& operator<<(Stream& stream, const SelectPeersData& data) {
        stream << quint64(data.peer_ids.size());
        for (quint64 id : data.peer_ids) {
            stream << id;
        }
        return stream;
    }

    template<class Stream>
    Stream& operator>>(Stream& stream, SelectPeersData& data) {
        quint64 size;
        stream >> size;
        data.peer_ids.reserve(size);
        for (qint64 i = 0; i < size; ++i) {
            qint64 id;
            stream >> id;
            data.peer_ids.insert(id);
        }
        return stream;
    }
}

#endif //TDESKTOP_DELETE_CHATS_H
