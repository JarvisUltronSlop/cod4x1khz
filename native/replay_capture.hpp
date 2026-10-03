#pragma once
#include "canonical.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace khz::capture {
enum class Domain : unsigned { Player, InputFeedback, AcceptedWorld, ControllerPoseAnimation, Aim, GameplayFX, RNG, Count };
constexpr unsigned DomainCount = static_cast<unsigned>(Domain::Count);
constexpr std::uint32_t AllDomains = (1u << DomainCount) - 1;
enum Restore : std::uint32_t {
    AssetsPinned = 1 << 0, AuthoritativeInputTimeline = 1 << 1, WorldRestored = 1 << 2,
    ScriptsRestored = 1 << 3, ServerRngRestored = 1 << 4, PrivateClientRestored = 1 << 5,
    CollisionPrepared = 1 << 6, FloatingPointMatched = 1 << 7
};
constexpr std::uint32_t AllRestoreConditions = (1u << 8) - 1;
enum class Failure : unsigned { None, MissingDomain, InvalidCanonicalField, Overflow, Lifecycle, EndedEarly };
struct Request {
    std::uint64_t fixture;
    unsigned samples;
    std::uint32_t verifiedRestoreConditions;
};
struct Record {
    std::uint64_t ordinal, session, generation, clientTick, authoritativeTick, inputAck;
    std::uint64_t currentAuthoritativeTick, currentInputAck, currentSnapshotAck, nextSnapshotAck;
    int commandNumber, commandTime, acceptedSnapshot, acceptedServerTime, currentMessage, nextMessage;
    std::uint32_t domainMask, restoreConditions;
    canonical::Digest domains[DomainCount];
};
struct Status {
    Request request;
    std::uint64_t recorded, drained, invalid, overflow;
    unsigned queued;
    Failure failure;
    bool active, finished, complete, comparisonEligible;
};

// Single worker producer/main consumer. Records live in a caller-owned fixed
// arena so neither ticking nor publication allocates or initializes a huge
// aggregate temporary. Begin/end require a producer quiescence acknowledgement.
template<unsigned Capacity> class Journal {
    static_assert(Capacity > 1 && !(Capacity & (Capacity - 1)));
public:
    explicit Journal(Record* records) : records_(records) {}
    bool begin(const Request& request) {
        if (!request.fixture || !request.samples || request.samples > 60000 ||
            (request.verifiedRestoreConditions & ~AllRestoreConditions) || active_.load(std::memory_order_acquire) ||
            write_.load(std::memory_order_acquire) != read_.load(std::memory_order_acquire)) return false;
        request_ = request; write_.store(0); read_.store(0); recorded_.store(0); drained_.store(0);
        invalid_.store(0); overflow_.store(0); failure_.store(Failure::None); finished_.store(false);
        active_.store(true, std::memory_order_release); return true;
    }
    bool push(Record record) {
        if (!active_.load(std::memory_order_acquire)) return false;
        const auto written = recorded_.load(std::memory_order_relaxed);
        record.ordinal = written + 1; record.restoreConditions = request_.verifiedRestoreConditions;
        if (record.domainMask != AllDomains) {
            ++invalid_; fail(Failure::MissingDomain);
        } else {
            for (const auto& digest : record.domains) if (!digest.valid) {
                ++invalid_; fail(Failure::InvalidCanonicalField); break;
            }
        }
        const auto write = write_.load(std::memory_order_relaxed);
        if (write - read_.load(std::memory_order_acquire) >= Capacity) {
            ++overflow_; fail(Failure::Overflow); active_.store(false, std::memory_order_release);
            finished_.store(true, std::memory_order_release); return false;
        }
        records_[write & (Capacity - 1)] = record;
        write_.store(write + 1, std::memory_order_release); recorded_.store(written + 1, std::memory_order_release);
        if (written + 1 >= request_.samples) {
            active_.store(false, std::memory_order_release); finished_.store(true, std::memory_order_release);
        }
        return true;
    }
    bool pop(Record& record) {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        record = records_[read & (Capacity - 1)]; read_.store(read + 1, std::memory_order_release);
        ++drained_; return true;
    }
    void end() {
        if (!request_.fixture) return;
        active_.store(false, std::memory_order_release);
        if (recorded_.load(std::memory_order_acquire) != request_.samples) fail(Failure::EndedEarly);
        finished_.store(true, std::memory_order_release);
    }
    // May be called before owner teardown. It never resets producer indices or
    // overwrites queued records; the main consumer can retain the partial proof.
    void invalidateLifecycle() {
        if (active_.exchange(false, std::memory_order_acq_rel)) { fail(Failure::Lifecycle); finished_.store(true, std::memory_order_release); }
    }
    bool active() const { return active_.load(std::memory_order_acquire); }
    Status status() const {
        Status result{}; result.request = request_;
        result.recorded = recorded_.load(std::memory_order_acquire); result.drained = drained_.load(std::memory_order_acquire);
        result.invalid = invalid_.load(std::memory_order_acquire); result.overflow = overflow_.load(std::memory_order_acquire);
        result.queued = write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
        result.failure = failure_.load(std::memory_order_acquire); result.active = active(); result.finished = finished_.load(std::memory_order_acquire);
        result.complete = request_.fixture && request_.samples && result.finished && !result.active && result.failure == Failure::None && result.recorded == request_.samples && result.drained == result.recorded;
        result.comparisonEligible = result.complete && request_.verifiedRestoreConditions == AllRestoreConditions;
        return result;
    }
private:
    void fail(Failure failure) { auto expected = Failure::None; failure_.compare_exchange_strong(expected, failure, std::memory_order_acq_rel); }
    Record* records_;
    Request request_{};
    alignas(64) std::atomic<unsigned> write_{0}, read_{0};
    std::atomic<std::uint64_t> recorded_{0}, drained_{0}, invalid_{0}, overflow_{0};
    std::atomic<Failure> failure_{Failure::None};
    std::atomic<bool> active_{false}, finished_{false};
};
} // namespace khz::capture
