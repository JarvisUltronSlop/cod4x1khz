#include "replay_capture.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
using namespace khz::capture;
Record sample(unsigned ordinal) {
    Record value{}; value.clientTick = ordinal; value.commandTime = ordinal; value.domainMask = AllDomains;
    for (unsigned d = 0; d < DomainCount; ++d) {
        khz::canonical::Hash hash; hash.scalar(ordinal); hash.scalar(d); value.domains[d] = hash.finish();
    }
    return value;
}
template<class Predicate> void await(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!ready()) { CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
}
int main() {
    std::array<Record,8> arena; Journal<8> small(arena.data()); Record value;
    CHECK(!small.begin({0,10,0}) && !small.begin({1,0,0}) && !small.begin({1,60001,0}));
    CHECK(small.begin({1,8,0})); CHECK(!small.begin({2,8,0}));
    for (unsigned i = 1; i <= 8; ++i) CHECK(small.push(sample(i)));
    CHECK(!small.active() && !small.status().complete);
    for (unsigned i = 1; i <= 8; ++i) { CHECK(small.pop(value)); CHECK(value.ordinal == i && value.clientTick == i); }
    CHECK(!small.pop(value) && small.status().complete && !small.status().comparisonEligible);
    CHECK(small.begin({2,9,AllRestoreConditions}));
    for (unsigned i = 1; i <= 8; ++i) CHECK(small.push(sample(i)));
    CHECK(!small.push(sample(9))); CHECK(small.status().failure == Failure::Overflow && !small.status().complete);
    for (unsigned i = 1; i <= 8; ++i) { CHECK(small.pop(value)); CHECK(value.clientTick == i); }
    CHECK(small.begin({3,1,AllRestoreConditions})); value = sample(1); value.domainMask &= ~(1u << static_cast<unsigned>(Domain::RNG));
    CHECK(small.push(value) && small.pop(value)); CHECK(small.status().failure == Failure::MissingDomain && !small.status().comparisonEligible);
    CHECK(small.begin({4,1,AllRestoreConditions})); value = sample(1); value.domains[0].valid = false;
    CHECK(small.push(value) && small.pop(value)); CHECK(small.status().failure == Failure::InvalidCanonicalField);
    CHECK(small.begin({5,10,AllRestoreConditions})); CHECK(small.push(sample(1))); small.end();
    CHECK(small.status().failure == Failure::EndedEarly && small.pop(value));
    CHECK(small.begin({6,10,AllRestoreConditions})); CHECK(small.push(sample(1))); small.invalidateLifecycle();
    CHECK(small.status().failure == Failure::Lifecycle && !small.active() && small.pop(value));

    // A real delayed consumer holds presentation for 1,000 producer samples.
    // Gameplay publication continues, then wraparound drains whole records in
    // order. Digest checks cover all domains, not just sequence counters.
    static Record concurrentArena[2048]; Journal<2048> journal(concurrentArena);
    constexpr unsigned Count = 20000;
    CHECK(journal.begin({7,Count,AllRestoreConditions}));
    std::atomic<unsigned> written{0}, read{0};
    std::atomic<bool> consumerStarted{false};
    std::thread producer([&] {
        for (unsigned i = 1; i <= Count; ++i) {
            if (i == 1001) await([&] { return consumerStarted.load(std::memory_order_acquire); });
            await([&] { return i - read.load(std::memory_order_acquire) < 1800; });
            CHECK(journal.push(sample(i))); written.store(i, std::memory_order_release);
        }
    });
    await([&] { return written.load(std::memory_order_acquire) == 1000; });
    CHECK(journal.status().recorded == 1000 && journal.active() && journal.status().overflow == 0);
    consumerStarted.store(true, std::memory_order_release);
    for (unsigned i = 1; i <= Count; ++i) {
        await([&] { return journal.pop(value); });
        CHECK(value.ordinal == i && value.clientTick == i && value.restoreConditions == AllRestoreConditions);
        const auto expected = sample(i);
        for (unsigned d = 0; d < DomainCount; ++d) CHECK(khz::canonical::equal(value.domains[d], expected.domains[d]));
        read.store(i, std::memory_order_release);
    }
    producer.join(); CHECK(journal.status().complete && journal.status().comparisonEligible);
    std::puts("bounded replay capture publication passed");
}
