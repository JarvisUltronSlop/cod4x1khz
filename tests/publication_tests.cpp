#include "publication.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace {
constexpr std::uint64_t PublicationCount = 100000;
struct Record {
    std::uint64_t sequence = 0;
    std::array<std::uint64_t, 1024> payload{};
    std::uint64_t checksum = 0;
};

std::uint64_t payloadWord(std::uint64_t sequence, unsigned word) {
    return (sequence * 0x9E3779B97F4A7C15ULL)
        ^ ((std::uint64_t(word) + 1) * 0xD1B54A32D192ED03ULL);
}

std::uint64_t checksumWord(std::uint64_t checksum, std::uint64_t word) {
    return ((checksum << 7) | (checksum >> 57)) ^ word;
}

void fill(Record& record, std::uint64_t sequence) {
    record.sequence = sequence;
    auto checksum = sequence;
    for (unsigned i = 0; i < record.payload.size(); ++i) {
        record.payload[i] = payloadWord(sequence, i);
        checksum = checksumWord(checksum, record.payload[i]);
    }
    record.checksum = checksum;
}

void verify(const Record& record) {
    const auto sequence = record.sequence;
    CHECK(sequence > 0 && sequence <= PublicationCount);
    auto checksum = sequence;
    for (unsigned i = 0; i < record.payload.size(); ++i) {
        CHECK(record.payload[i] == payloadWord(sequence, i));
        checksum = checksumWord(checksum, record.payload[i]);
    }
    CHECK(record.sequence == sequence && record.checksum == checksum);
}

template<class Predicate> void await(Predicate ready) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!ready()) {
        CHECK(std::chrono::steady_clock::now() < limit);
        std::this_thread::yield();
    }
}
} // namespace

template<class Publication> void exercisePublication(Publication& publication) {
    const auto& initial = publication.readLatest();
    CHECK(initial.sequence == 0 && initial.checksum == 0);
    for (const auto word : initial.payload) CHECK(word == 0);

    std::atomic<std::uint64_t> completed{0};
    std::atomic<bool> consumerHolding{false}, consumerRunning{false};
    std::thread producer([&] {
        fill(publication.writable(), 1);
        publication.publish();
        completed.store(1, std::memory_order_release);
        await([&] { return consumerHolding.load(std::memory_order_acquire); });

        // The consumer deliberately keeps its first record while the producer
        // repeatedly recycles the other two slots, superseding pending records.
        for (std::uint64_t sequence = 2; sequence <= 2000; ++sequence) {
            fill(publication.writable(), sequence);
            publication.publish();
        }
        completed.store(2000, std::memory_order_release);
        await([&] { return consumerRunning.load(std::memory_order_acquire); });
        for (std::uint64_t sequence = 2001; sequence <= PublicationCount; ++sequence) {
            fill(publication.writable(), sequence);
            publication.publish();
        }
        completed.store(PublicationCount, std::memory_order_release);
    });

    await([&] { return completed.load(std::memory_order_acquire) == 1; });
    const auto& held = publication.readLatest();
    CHECK(held.sequence == 1);
    verify(held);
    consumerHolding.store(true, std::memory_order_release);
    await([&] { return completed.load(std::memory_order_acquire) == 2000; });
    CHECK(held.sequence == 1);
    verify(held); // producer never overwrote the held consumer slot
    const auto& latest = publication.readLatest();
    CHECK(latest.sequence == 2000);
    verify(latest);
    consumerRunning.store(true, std::memory_order_release);

    std::uint64_t previousSequence = latest.sequence, reads = 0, changes = 0;
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (previousSequence < PublicationCount || reads < PublicationCount) {
        const auto& record = publication.readLatest();
        verify(record); // every payload word belongs to one complete sequence
        CHECK(record.sequence >= previousSequence);
        if (record.sequence != previousSequence) ++changes;
        previousSequence = record.sequence;
        ++reads;
        if ((reads & 255) == 0) CHECK(std::chrono::steady_clock::now() < limit);
    }
    producer.join();
    CHECK(completed.load(std::memory_order_acquire) == PublicationCount);
    CHECK(publication.readLatest().sequence == PublicationCount);
    CHECK(changes > 0 && reads >= PublicationCount);
    std::printf("PASS: 100000 publications, %llu complete-record reads, %llu acquired changes, held 8 KB record preserved across 2000 publications\n",
        static_cast<unsigned long long>(reads), static_cast<unsigned long long>(changes));
}

int main() {
    khz::TriplePublication<Record> owned;
    exercisePublication(owned);
    alignas(64) static Record externalStorage[3];
    khz::ExternalTriplePublication<Record> external(externalStorage);
    exercisePublication(external);
}
