#pragma once
// Stock presentation/killcam archive policy; authoritative simulation and transport remain 1 ms.
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace khz_archive {

constexpr int FrameCapacity = 1200;
constexpr int ByteCapacity = 33554432;
constexpr int MessageCapacity = 131072;
constexpr int SampleMsec = 50;
constexpr int RetentionMsec = 60000;
constexpr int FullBaseMsec = 1000;
constexpr int CacheFrameCapacity = 512;
constexpr int CacheClientCapacity = 4096;
constexpr int NativeClientCapacity = 64;
constexpr unsigned AvailabilityWords = (FrameCapacity + 63) / 64;

enum class Kind : unsigned { Unknown, Full, Delta };
enum class Due { NotDue, Started, InvalidNativeState, ClockReversed, Busy };

inline bool validKind(Kind kind) { return kind == Kind::Full || kind == Kind::Delta; }

// Delta decoding writes another frame and up to 64 client rows. A cached
// full base that fails this rule must be freshly decoded from committed bytes.
// Do not return it and then overwrite its borrowed metadata/client storage.
inline bool decoderWriteHeadroom(int cacheIndex, int nextCacheFrame,
    int firstClient, int clients, int nextClient) {
    return cacheIndex >= 0 && cacheIndex < CacheFrameCapacity && nextCacheFrame >= 0 &&
        cacheIndex != nextCacheFrame % CacheFrameCapacity && firstClient >= 0 &&
        clients >= 0 && clients <= NativeClientCapacity && nextClient >= 0 &&
        std::int64_t(firstClient) >= std::int64_t(nextClient) - CacheClientCapacity + NativeClientCapacity &&
        std::int64_t(firstClient) + clients <= nextClient;
}

struct Entry {
    std::uint64_t epoch;
    std::uint64_t logicalTick;
    int id;
    int nativeTime;
    int start;
    int size;
    int baseId;
    int baseTime;
    Kind kind;
};
struct Attempt {
    std::uint64_t logicalTick;
    int id;
    int nativeTime;
    int start;
    int baseId;
    int baseTime;
    Kind kind;
    bool active;
};
struct Selection {
    int id;                 // -1 means native current-client fallback.
    int actualNativeTime;   // Never substituted for the logical delay in normal rounding.
    int logicalDelay;
    bool clamped;
    bool valid;
};
struct State {
    // This POD is a mandatory sidecar of native svs/cache rings in a checkpoint.
    // Use static BSS storage, explicit lifecycle reset, and bytewise image copy.
    std::array<Entry, FrameCapacity> entries;
    std::array<std::uint64_t, AvailabilityWords> availability;
    Attempt attempt;
    std::uint64_t epoch;
    std::uint64_t nextDueTime;
    std::uint64_t attempts;
    std::uint64_t overflows;
    std::uint64_t skippedDeadlines;
    int lastOwnerTime;
    int indexedFrame;
    int indexedByte;
    bool clockStarted;
    bool indexValid;

    void reset(std::uint64_t newEpoch) {
        std::memset(this, 0, sizeof(*this));
        epoch = newEpoch;
        indexValid = newEpoch != 0;
    }

    Due begin(int now, std::uint64_t tick, int nextFrame, int nextByte) {
        if (attempt.active) return Due::Busy;
        if (!epoch || now < 0 || nextFrame < 0 || nextByte < 0 ||
            !indexValid || indexedFrame != nextFrame || indexedByte != nextByte ||
            nextFrame >= (std::numeric_limits<int>::max)() - FrameCapacity ||
            nextByte >= (std::numeric_limits<int>::max)() - ByteCapacity)
            return Due::InvalidNativeState;
        if (clockStarted && now < lastOwnerTime) return Due::ClockReversed;
        lastOwnerTime = now;
        const auto current = std::uint64_t(now);
        if (!clockStarted) {
            clockStarted = true;
            nextDueTime = current;
        }
        if (current < nextDueTime) return Due::NotDue;
        const auto deadlines = (current - nextDueTime) / SampleMsec + 1;
        skippedDeadlines += deadlines - 1;
        nextDueTime += deadlines * SampleMsec;
        attempt = {tick, nextFrame, now, nextByte, -1, 0, Kind::Unknown, true};
        ++attempts;
        return Due::Started;
    }

    const Entry* committed(int id, int nextFrame, int nextByte, int start, int size) const {
        if (!epoch || id < 0 || id >= nextFrame || id < nextFrame - FrameCapacity ||
            nextFrame < 0 || nextByte < 0 || start < 0 || size <= 0 || size > MessageCapacity)
            return nullptr;
        const auto& value = entries[unsigned(id) % FrameCapacity];
        if (value.epoch != epoch || value.id != id || value.nativeTime < 0 ||
            value.start != start || value.size != size || !validKind(value.kind))
            return nullptr;
        const auto beginByte = std::int64_t(start), endByte = beginByte + size;
        if (beginByte < std::int64_t(nextByte) - ByteCapacity || endByte > nextByte)
            return nullptr;
        return &value;
    }

    bool fullBase(const Entry* value, int cachedId, int cachedTime) const {
        if (!attempt.active || !value || value->epoch != epoch || value->id < 0 || value->nativeTime < 0 ||
            &entries[unsigned(value->id) % FrameCapacity] != value || value->kind != Kind::Full ||
            value->id != cachedId || value->nativeTime != cachedTime ||
            value->id >= attempt.id || value->nativeTime > attempt.nativeTime)
            return false;
        return std::int64_t(attempt.nativeTime) - value->nativeTime <= FullBaseMsec;
    }

    bool encodedFull() {
        if (!attempt.active || attempt.kind != Kind::Unknown) return false;
        attempt.kind = Kind::Full;
        return true;
    }
    bool encodedDelta(const Entry* base, int cachedId, int cachedTime) {
        if (attempt.kind != Kind::Unknown || !fullBase(base, cachedId, cachedTime)) return false;
        attempt.kind = Kind::Delta;
        attempt.baseId = cachedId;
        attempt.baseTime = cachedTime;
        return true;
    }

    bool canCommit(int size, int nextFrame, int nextByte) const {
        return attempt.active && validKind(attempt.kind) && nextFrame >= 0 && nextByte >= 0 &&
            attempt.id == nextFrame && attempt.start == nextByte && size > 0 &&
            size <= MessageCapacity &&
            std::int64_t(nextByte) + size < (std::numeric_limits<int>::max)() &&
            (attempt.kind != Kind::Delta || (attempt.baseId >= 0 && attempt.baseId < attempt.id &&
                attempt.baseTime >= 0 && attempt.baseTime <= attempt.nativeTime));
    }
    bool commit(int size, int nextFrame, int nextByte) {
        // Call after payload copies succeed, with the original pre-copy native counters.
        // canCommit() must have passed before any payload/descriptor mutation.
        if (!canCommit(size, nextFrame, nextByte)) return false;
        const auto& a = attempt;
        entries[unsigned(a.id) % FrameCapacity] = {
            epoch, a.logicalTick, a.id, a.nativeTime, a.start, size,
            a.baseId, a.baseTime, a.kind,
        };
        attempt.active = false;
        indexValid = false;
        return true;
    }
    void abortOverflow() {
        // Native encoder cache counters/writes are intentionally NOT rewound.
        // Their naturally advanced eviction limits remain authoritative.
        if (attempt.active) ++overflows;
        attempt.active = false;
    }

    template<class Available>
    bool buildAvailability(std::array<std::uint64_t, AvailabilityWords>& output,
        int nextFrame, int nextByte, Available available) const {
        output.fill(0);
        if (!epoch || attempt.active || nextFrame < 0 || nextByte < 0) return false;
        const int first = nextFrame > FrameCapacity ? nextFrame - FrameCapacity : 0;
        int previousTime = -1;
        std::int64_t previousEnd = -1;
        for (int id = first; id < nextFrame; ++id) {
            const auto& value = entries[unsigned(id) % FrameCapacity];
            if (value.epoch != epoch || value.id != id || value.nativeTime <= previousTime ||
                value.start < 0 || value.size <= 0 || value.size > MessageCapacity ||
                !validKind(value.kind) || std::int64_t(value.start) + value.size > nextByte ||
                (previousEnd >= 0 && value.start != previousEnd)) return false;
            previousTime = value.nativeTime;
            previousEnd = std::int64_t(value.start) + value.size;
            if (!committed(id, nextFrame, nextByte, value.start, value.size) || !available(value)) continue;
            const unsigned slot = unsigned(id) % FrameCapacity;
            output[slot / 64] |= std::uint64_t(1) << (slot % 64);
        }
        return nextFrame ? previousEnd == nextByte : nextByte == 0;
    }

    template<class Available>
    bool refreshAvailability(int nextFrame, int nextByte, Available available) {
        indexValid = false;
        if (!buildAvailability(availability, nextFrame, nextByte, available)) return false;
        indexedFrame = nextFrame;
        indexedByte = nextByte;
        indexValid = true;
        return true;
    }

    int lowerBoundTime(int first, int last, std::int64_t time) const {
        while (first < last) {
            const int middle = first + (last - first) / 2;
            if (entries[unsigned(middle) % FrameCapacity].nativeTime < time) first = middle + 1;
            else last = middle;
        }
        return first;
    }
    static unsigned firstBit(std::uint64_t nonzero) {
        unsigned result = 0;
        if (!(nonzero & UINT64_C(0xffffffff))) { nonzero >>= 32; result += 32; }
        if (!(nonzero & UINT64_C(0xffff))) { nonzero >>= 16; result += 16; }
        if (!(nonzero & UINT64_C(0xff))) { nonzero >>= 8; result += 8; }
        if (!(nonzero & UINT64_C(0xf))) { nonzero >>= 4; result += 4; }
        if (!(nonzero & UINT64_C(0x3))) { nonzero >>= 2; result += 2; }
        if (!(nonzero & UINT64_C(0x1))) ++result;
        return result;
    }
    int nextAvailable(int first, int last) const {
        // At most 20 bounded word reads for a <=1200-entry circular range,
        // even when availability has arbitrary delta-base/byte-eviction holes.
        for (int id = first; id < last;) {
            const unsigned slot = unsigned(id) % FrameCapacity, bit = slot % 64;
            unsigned count = 64 - bit;
            if (count > unsigned(FrameCapacity) - slot) count = unsigned(FrameCapacity) - slot;
            if (count > unsigned(last - id)) count = unsigned(last - id);
            auto bits = availability[slot / 64] >> bit;
            if (count != 64) bits &= (std::uint64_t(1) << count) - 1;
            if (bits) return id + int(firstBit(bits));
            id += int(count);
        }
        return -1;
    }
    Selection select(int now, int delay, int nextFrame, int nextByte) const {
        if (attempt.active || !indexValid || indexedFrame != nextFrame || indexedByte != nextByte ||
            !epoch || now < 0 || nextFrame < 0 || nextByte < 0) return {-1, now, 0, false, false};
        if (delay <= 0) return {-1, now, delay, false, true};
        const int first = nextFrame > FrameCapacity ? nextFrame - FrameCapacity : 0;
        // Timestamp comparisons are over dense committed id rows, not cache
        // entries. Overflow never inserts a committed id/time row.
        const int last = nextFrame;
        if (first < last && entries[unsigned(last - 1) % FrameCapacity].nativeTime > now)
            return {-1, now, 0, false, false}; // Original owner clock/restore mismatch.
        const auto retentionStart = std::int64_t(now) - RetentionMsec;
        const int retained = first < last && entries[unsigned(first) % FrameCapacity].nativeTime < retentionStart ?
            lowerBoundTime(first, last, retentionStart) : first;
        const int oldest = nextAvailable(retained, last);
        if (oldest < 0) return {-1, now, 0, false, true};
        const auto target = std::int64_t(now) - delay;
        const int ceiling = lowerBoundTime(oldest, last, target);
        const int selected = nextAvailable(ceiling, last);
        if (selected < 0) return {-1, now, 0, false, true};
        const int actualTime = entries[unsigned(selected) % FrameCapacity].nativeTime;
        if (actualTime == now) return {-1, now, 0, false, true};
        const int oldestTime = entries[unsigned(oldest) % FrameCapacity].nativeTime;
        const bool clamped = target < oldestTime;
        // Preserve logical delay on ordinary ceiling rounding and internal
        // holes. Native SV_BuildClientSnapshot persists this logical value.
        return {selected, actualTime, clamped ? now - oldestTime : delay, clamped, true};
    }
};

static_assert(std::is_trivially_copyable<State>::value, "same-process checkpoint sidecar");
static_assert(sizeof(State) < 80000, "bounded metadata, no world/player-state copies");

} // namespace khz_archive
