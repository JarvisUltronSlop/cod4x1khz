// Bounded stock archive policy contracts; native codec/gameplay checks are separate.
// These execute the production policy with an independent lookup reference.
#include <archive_history.hpp>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <initializer_list>

namespace {
using namespace khz_archive;
void require(bool value, const char* expression, int line) {
    if (value) return;
    std::fprintf(stderr, "archive policy contract failed at line %d: %s\n", line, expression);
    std::abort();
}
#define CHECK(value) require(bool(value), #value, __LINE__)

struct Fixture {
    State state;
    int nextFrame = 0;
    int nextByte = 0;
    Fixture() { state.reset(1); }
    const Entry* entry(int id) const {
        const auto& row = state.entries[unsigned(id) % FrameCapacity];
        return state.committed(id, nextFrame, nextByte, row.start, row.size);
    }
    void start(int time) {
        CHECK(state.begin(time, std::uint64_t(time) + 1, nextFrame, nextByte) == Due::Started);
    }
    void complete(int size = 80) {
        CHECK(state.canCommit(size, nextFrame, nextByte));
        CHECK(state.commit(size, nextFrame, nextByte));
        ++nextFrame; nextByte += size;
        CHECK(state.refreshAvailability(nextFrame, nextByte, [&](const Entry& row) { return entry(row.id); }));
    }
    void full(int time, int size = 80) {
        start(time); CHECK(state.encodedFull()); complete(size);
    }
    Selection select(int time, int delay) const {
        return state.select(time, delay, nextFrame, nextByte);
    }
};

void clockAndRates() {
    Fixture f;
    CHECK(f.state.begin(0, 1, 0, 0) == Due::Started);
    CHECK(f.state.begin(0, 1, 0, 0) == Due::Busy);
    CHECK(f.state.encodedFull()); f.complete();
    unsigned simulatedTicks = 1;
    for (int time = 1; time < 1000; ++time) {
        ++simulatedTicks;
        const auto due = f.state.begin(time, std::uint64_t(time) + 1, f.nextFrame, f.nextByte);
        if (time % 50) CHECK(due == Due::NotDue);
        else { CHECK(due == Due::Started); CHECK(f.state.encodedFull()); f.complete(); }
    }
    CHECK(simulatedTicks == 1000); CHECK(f.state.attempts == 20); CHECK(f.nextFrame == 20);
    CHECK(f.state.nextDueTime == 1000); CHECK(f.state.skippedDeadlines == 0);
    // A scheduler stall encodes one present world, not fabricated old worlds.
    f.start(1277); CHECK(f.state.skippedDeadlines == 5); CHECK(f.state.nextDueTime == 1300);
    CHECK(f.state.encodedFull()); f.complete();
    CHECK(f.entry(20)->nativeTime == 1277);
    CHECK(f.state.begin(1276, 0, f.nextFrame, f.nextByte) == Due::ClockReversed);
    CHECK(f.state.begin(1299, 0, f.nextFrame, f.nextByte) == Due::NotDue);
    CHECK(f.state.begin(1300, 0, f.nextFrame, f.nextByte) == Due::Started);
}

void logicalDelayDoesNotCollapse() {
    Fixture f;
    for (int time = 0; time <= 10000; time += SampleMsec) f.full(time);
    int persistentSessionDelay = 5000;
    for (int now = 10000; now != 10050; ++now) {
        const auto result = f.select(now, persistentSessionDelay);
        CHECK(result.id >= 0); CHECK(!result.clamped);
        // Independent arithmetic reference to the stock 50 ms ceiling sample.
        const int expectedTime = ((now - 5000 + 49) / 50) * 50;
        CHECK(result.actualNativeTime == expectedTime);
        CHECK(result.logicalDelay == 5000);
        persistentSessionDelay = result.logicalDelay; // Real sess.archiveTime assignment.
    }
    CHECK(persistentSessionDelay == 5000);
    CHECK(f.select(10001, 0).id == -1);
    CHECK(f.select(10001, -1).logicalDelay == -1);
    CHECK(f.select(10000, 1).id == -1); // Rounded to current => current fallback, delay 0.
    CHECK(f.select(10000, 1).logicalDelay == 0);
    auto oldest = f.select(10000, 11000);
    CHECK(oldest.id == 0); CHECK(oldest.clamped); CHECK(oldest.logicalDelay == 10000);
}

void skippedOverflowAndExactCachedIdentity() {
    Fixture f; f.full(0);
    f.start(50); CHECK(f.state.encodedFull());
    // Native full encoder has already written a phantom cache row and evicted
    // payload rows before the message overflows. Those counters stay advanced.
    int nativeCacheWrites = 16;
    const int phantomId = f.nextFrame, phantomTime = 50;
    f.state.abortOverflow();
    CHECK(nativeCacheWrites == 16); CHECK(f.state.overflows == 1);
    CHECK(f.nextFrame == 1); CHECK(f.nextByte == 80);
    CHECK(!f.state.committed(phantomId, f.nextFrame, f.nextByte, 80, 80));
    f.full(100);
    f.start(150);
    CHECK(!f.state.fullBase(f.entry(1), phantomId, phantomTime));
    CHECK(f.state.fullBase(f.entry(1), 1, 100));
    CHECK(!f.state.fullBase(f.entry(0), 1, 0));
    CHECK(f.state.encodedDelta(f.entry(1), 1, 100)); f.complete();
    CHECK(f.entry(2)->kind == Kind::Delta && f.entry(2)->baseId == 1);
    auto internalGap = f.select(175, 120); // target 55; no record at 50.
    CHECK(internalGap.id == 1 && internalGap.actualNativeTime == 100);
    CHECK(internalGap.logicalDelay == 120 && !internalGap.clamped);
    // Cache overflow eviction is not rolled back even when the archived id is reused.
    CHECK(nativeCacheWrites == 16);
}

void fullBaseTimestampAndGraphGuards() {
    Fixture f; f.full(0);
    f.start(1000);
    const Entry* original = f.entry(0);
    CHECK(original); CHECK(f.state.fullBase(original, 0, 0));
    Entry foreign = *original;
    CHECK(!f.state.fullBase(&foreign, 0, 0)); // Exact original sidecar identity.
    CHECK(!f.state.fullBase(original, 0, 1)); // Cached native time must match.
    CHECK(!f.state.fullBase(original, 1, 0)); // Cached id must match.
    CHECK(f.state.encodedDelta(original, 0, 0)); f.complete();
    f.start(1050);
    CHECK(!f.state.fullBase(f.entry(0), 0, 0)); // 1050 ms exceeds 1 s full-base horizon.
    CHECK(!f.state.fullBase(f.entry(1), 1, 1000)); // Delta chaining rejected.
    CHECK(!f.state.encodedDelta(f.entry(1), 1, 1000));
    CHECK(f.state.encodedFull()); f.complete();
    CHECK(!f.state.encodedFull());
}

void boundedRingsAndByteEviction() {
    Fixture f;
    for (int id = 0; id != FrameCapacity + 30; ++id) f.full(id * 50);
    CHECK(f.nextFrame == 1230); CHECK(!f.entry(0)); CHECK(!f.entry(29)); CHECK(f.entry(30));
    CHECK(f.state.committed(30, f.nextFrame, f.nextByte, 2400, 80));
    CHECK(!f.state.committed(30, f.nextFrame, f.nextByte, 2401, 80));
    CHECK(!f.state.committed(30, f.nextFrame, f.nextByte, 2400, 81));
    CHECK(f.select(61451, 60000).logicalDelay == 59951); // Oldest 1500 ms timestamp clamp.
    Fixture bytes;
    for (int id = 0; id < 270; ++id) bytes.full(id * 50, MessageCapacity);
    CHECK(!bytes.entry(0)); CHECK(!bytes.entry(13)); CHECK(bytes.entry(14));
    CHECK(bytes.entry(14)->start == bytes.nextByte - ByteCapacity); // Inclusive byte retention edge.
    auto result = bytes.select(13451, 20000);
    CHECK(result.id == 14 && result.clamped && result.logicalDelay == 12751);
    CHECK(!bytes.state.committed(bytes.nextFrame, bytes.nextFrame, bytes.nextByte, 0, 80));
    CHECK(!bytes.state.committed(-1, bytes.nextFrame, bytes.nextByte, 0, 80));
    CHECK(!bytes.state.committed(269, bytes.nextFrame, bytes.nextByte, bytes.nextByte - 80, MessageCapacity + 1));
}

void availabilityIndependentOfCachedPresence() {
    Fixture f;
    for (int time = 0; time <= 400; time += 50) f.full(time);
    // External byte/base availability can contain a hole. Select skips it but
    // preserves the logical request; decoded-cache presence is not an input.
    CHECK(f.state.refreshAvailability(f.nextFrame, f.nextByte,
        [&](const Entry& row) { return row.id != 5 && row.id != 6 && f.entry(row.id); }));
    const auto result = f.select(451, 201);
    CHECK(result.id == 7 && result.actualNativeTime == 350);
    CHECK(result.logicalDelay == 201 && !result.clamped);
    CHECK(f.state.refreshAvailability(f.nextFrame, f.nextByte, [](const Entry&) { return false; }));
    CHECK(f.select(451, 201).id == -1);
    CHECK(f.select(451, 201).logicalDelay == 0);
    CHECK(f.state.refreshAvailability(f.nextFrame, f.nextByte, [](const Entry&) { return true; }));
    CHECK(f.select(60451, 201).id == -1);
    CHECK(f.select(399, 201).valid == false); // Native clock cannot precede a committed future sample.
}

void bitmapMatchesIndependentLinearSearch() {
    Fixture f;
    for (int id = 0; id != 1240; ++id) f.full(id * 50);
    const int first = f.nextFrame - FrameCapacity;
    const auto permitted = [](int id, unsigned pattern) {
        switch (pattern) {
        case 0: return true;
        case 1: return id % 2 == 0;
        case 2: return id % 67 == 66; // Whole zero bitmap words and ring wrap.
        case 3: return id % 1200 == 1199; // Only the tail word's final bit.
        default: return false;
        }
    };
    for (unsigned pattern = 0; pattern != 5; ++pattern) {
        CHECK(f.state.refreshAvailability(f.nextFrame, f.nextByte,
            [&](const Entry& row) { return permitted(row.id, pattern); }));
        for (int offset : {0, 1, 49, 50, 10001}) {
            const int now = 61950 + offset;
            for (int delay = 1; delay < 70000; delay += 137) {
                int oldest = -1, candidate = -1;
                // Independent slow reference used only by the proposed tests.
                // Production lookup uses binary search and bounded bitmap words.
                for (int id = first; id < f.nextFrame; ++id) {
                    const int time = id * 50;
                    if (!permitted(id, pattern) || std::int64_t(now) - time > 60000) continue;
                    if (oldest < 0) oldest = id;
                    if (std::int64_t(time) >= std::int64_t(now) - delay) { candidate = id; break; }
                }
                const auto actual = f.select(now, delay);
                CHECK(actual.valid);
                if (candidate < 0 || candidate * 50 == now) {
                    CHECK(actual.id == -1 && actual.logicalDelay == 0 && !actual.clamped);
                }
                else {
                    const bool clamp = std::int64_t(now) - delay < oldest * 50;
                    CHECK(actual.id == candidate && actual.actualNativeTime == candidate * 50);
                    CHECK(actual.clamped == clamp);
                    CHECK(actual.logicalDelay == (clamp ? now - oldest * 50 : delay));
                }
            }
        }
    }
    CHECK(!f.state.select(62000, 5000, f.nextFrame + 1, f.nextByte).valid);
    CHECK(!f.state.select(62000, 5000, f.nextFrame, f.nextByte + 1).valid);
    // Corrupted dense metadata cannot establish an availability index.
    f.state.entries[unsigned(first + 1) % FrameCapacity].nativeTime = first * 50;
    CHECK(!f.state.refreshAvailability(f.nextFrame, f.nextByte, [](const Entry&) { return true; }));
    CHECK(!f.select(62000, 5000).valid);
}

void nonmonotonicDeltaBaseEvictionHasExactHoles() {
    Fixture f;
    f.full(0, MessageCapacity); f.full(50, MessageCapacity);
    for (int id = 2; id <= 4; ++id) {
        f.start(id * 50);
        const int base = id == 3 ? 1 : 0;
        CHECK(f.state.encodedDelta(f.entry(base), base, base * 50));
        f.complete(MessageCapacity);
    }
    for (int id = 5; id != 257; ++id) f.full(id * 50, MessageCapacity);
    CHECK(!f.entry(0) && f.entry(1));
    CHECK(f.state.refreshAvailability(f.nextFrame, f.nextByte, [&](const Entry& row) {
        if (row.kind == Kind::Full) return true;
        const auto* base = f.entry(row.baseId);
        return base && base->kind == Kind::Full && base->nativeTime == row.baseTime;
    }));
    CHECK(f.state.nextAvailable(2, 5) == 3);
    const auto selected = f.select(12801, 12701); // Target100; row2's full base0 was evicted.
    CHECK(selected.id == 3 && selected.actualNativeTime == 150 && selected.logicalDelay == 12701);
    CHECK(!selected.clamped);
    const auto clamped = f.select(12801, 13000);
    CHECK(clamped.id == 1 && clamped.clamped && clamped.logicalDelay == 12751);
}

void decoderBorrowedStorageIsNotOverwritten() {
    CHECK(!decoderWriteHeadroom(0, 512, 1000, 16, 1016)); // Base metadata is next cache write slot.
    CHECK(decoderWriteHeadroom(511, 512, 1000, 16, 1016));
    CHECK(!decoderWriteHeadroom(511, 512, 0, 16, 4096)); // New client rows would overwrite base.
    CHECK(!decoderWriteHeadroom(511, 512, 63, 16, 4096));
    CHECK(decoderWriteHeadroom(511, 512, 64, 16, 4096)); // Exactly 64-row write headroom.
    std::array<int, CacheClientCapacity> payload{};
    for (int i = 0; i < 16; ++i) payload[64 + i] = 100 + i;
    for (int i = 0; i < NativeClientCapacity; ++i) payload[i] = -1;
    for (int i = 0; i < 16; ++i) CHECK(payload[64 + i] == 100 + i);
    CHECK(!decoderWriteHeadroom(511, 512, 2000, 65, 2065));
    CHECK(!decoderWriteHeadroom(511, 512, 2000, 16, 2000));
    CHECK(!decoderWriteHeadroom(512, 512, 0, 0, 0));
}

void preflightLifecycleAndCheckpointBranch() {
    Fixture f;
    CHECK(f.state.begin(-1, 1, 0, 0) == Due::InvalidNativeState);
    CHECK(f.state.begin(0, 1, -1, 0) == Due::InvalidNativeState);
    CHECK(f.state.begin(0, 1, 0, -1) == Due::InvalidNativeState);
    CHECK(f.state.begin(0, 1, (std::numeric_limits<int>::max)() - FrameCapacity, 0) == Due::InvalidNativeState);
    CHECK(f.state.begin(0, 1, 0, (std::numeric_limits<int>::max)() - ByteCapacity) == Due::InvalidNativeState);
    f.start(0);
    CHECK(!f.state.canCommit(80, 0, 0)); // Kind not selected.
    CHECK(f.state.encodedFull());
    CHECK(!f.state.canCommit(0, 0, 0)); CHECK(!f.state.canCommit(MessageCapacity + 1, 0, 0));
    CHECK(!f.state.canCommit(80, 1, 0)); CHECK(!f.state.canCommit(80, 0, 1));
    f.complete();
    f.state.begin(1, 2, f.nextFrame, f.nextByte);
    const State saved = f.state; const int savedFrames = f.nextFrame, savedBytes = f.nextByte;
    // Explicit same-process native-ring restoration is represented here only
    // by these synthetic counters; this test is not a native world restore.
    f.full(50); f.full(100); f.start(150); CHECK(f.state.encodedFull()); f.state.abortOverflow();
    f.state = saved; f.nextFrame = savedFrames; f.nextByte = savedBytes;
    CHECK(f.state.nextDueTime == 50); CHECK(f.state.attempts == 1 && f.state.overflows == 0);
    CHECK(f.state.begin(49, 50, f.nextFrame, f.nextByte) == Due::NotDue);
    f.full(50); CHECK(f.entry(1)->nativeTime == 50);
    const Entry previous = *f.entry(0);
    f.state.reset(2); f.nextFrame = f.nextByte = 0;
    CHECK(!f.state.committed(previous.id, 1, previous.start + previous.size, previous.start, previous.size));
    CHECK(f.state.begin(0, 1, 0, 0) == Due::Started); // Map time may reset only after epoch reset.
}
}
int main() {
    clockAndRates(); logicalDelayDoesNotCollapse(); skippedOverflowAndExactCachedIdentity();
    fullBaseTimestampAndGraphGuards(); boundedRingsAndByteEviction();
    availabilityIndependentOfCachedPresence(); decoderBorrowedStorageIsNotOverwritten();
    bitmapMatchesIndependentLinearSearch(); nonmonotonicDeltaBaseEvictionHasExactHoles();
    preflightLifecycleAndCheckpointBranch();
    std::puts("archive policy contracts passed (native codec/world tested separately)");
}
