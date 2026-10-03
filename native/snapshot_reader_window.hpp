#pragma once
#include <cstdint>

namespace khz {
struct SnapshotReaderWindow { int processed; std::uint64_t expired; bool valid; };
// CL_GetSnapshot rejects distance >= retention. Skip only that unavailable
// prefix; the following read starts at the oldest still-retained message.
inline constexpr SnapshotReaderWindow snapshotReaderWindow(int latest, int processed, unsigned retention) {
    if (!retention || latest < processed) return {processed, 0, false};
    const auto distance = std::int64_t(latest) - processed;
    if (distance <= retention) return {processed, 0, true};
    return {static_cast<int>(std::int64_t(latest) - retention), static_cast<std::uint64_t>(distance - retention), true};
}
}
