#pragma once
#include "core.hpp"
#include <array>
#include <cstdint>

namespace khz {
// The snapshot decoder owns this store. Readers require that same owner or a
// quiescent decoder; presentation receives copied headers in its publication.
class SnapshotHeaderHistory {
public:
    void reset(std::uint64_t session) {
        session_ = session;
        newestTick_ = 0;
        if (!++epoch_) { entries_ = {}; ++epoch_; }
    }
    bool commit(int messageNumber, const WireHeader& header) {
        if (!session_ || header.session != session_ || !header.tick || messageNumber < 0
            || (newestTick_ >= header.tick && newestTick_ - header.tick >= SnapshotHistory)) return false;
        if (header.tick > newestTick_) newestTick_ = header.tick;
        auto& entry = entries_[static_cast<unsigned>(messageNumber) & (SnapshotHistory - 1)];
        entry = {epoch_, messageNumber, header};
        return true;
    }
    bool read(int messageNumber, WireHeader* output) const {
        if (!output || !session_ || messageNumber < 0) return false;
        const auto& entry = entries_[static_cast<unsigned>(messageNumber) & (SnapshotHistory - 1)];
        if (entry.epoch != epoch_ || entry.messageNumber != messageNumber || entry.header.session != session_
            || newestTick_ < entry.header.tick || newestTick_ - entry.header.tick >= SnapshotHistory) return false;
        *output = entry.header;
        return true;
    }
private:
    struct Entry { std::uint64_t epoch = 0; int messageNumber = 0; WireHeader header{}; };
    std::array<Entry, SnapshotHistory> entries_{};
    std::uint64_t session_ = 0, epoch_ = 0, newestTick_ = 0;
};
static_assert((SnapshotHistory & (SnapshotHistory - 1)) == 0);
}
