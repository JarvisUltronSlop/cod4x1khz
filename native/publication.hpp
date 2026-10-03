#pragma once

#include <array>
#include <atomic>
#include <type_traits>

namespace khz {

// Exactly one producer and one consumer own this publication. The producer
// fills writable(), then publishes the complete record. The consumer can skip
// superseded publications and holds its returned record until its next
// readLatest() call. Neither operation allocates, copies T, or waits.
template<class T> class TriplePublication {
    static_assert(std::is_trivially_copyable_v<T>, "Publication records must be trivially copyable");
    static_assert(std::is_default_constructible_v<T>, "Publication records require an initial value");
    static_assert(std::atomic<unsigned>::is_always_lock_free, "Publication ownership must be lock-free");

public:
    TriplePublication() = default;
    TriplePublication(const TriplePublication&) = delete;
    TriplePublication& operator=(const TriplePublication&) = delete;

    // Producer only. References obtained here expire at publish().
    T& writable() noexcept { return slots_[back_].value; }

    void publish() noexcept {
        // Release publishes every field; acquire takes back the middle slot,
        // including a former consumer slot after the consumer released it.
        back_ = middle_.exchange(back_ | Dirty, std::memory_order_acq_rel) & IndexMask;
    }

    // Consumer only. The reference remains immutable until this consumer
    // acquires another publication. Before the first publish it contains T{}.
    const T& readLatest() noexcept {
        if (middle_.load(std::memory_order_acquire) & Dirty) {
            // A concurrent producer may replace the pending value before this
            // exchange. We acquire that latest value and release our old front.
            front_ = middle_.exchange(front_, std::memory_order_acq_rel) & IndexMask;
        }
        return slots_[front_].value;
    }

private:
    static constexpr unsigned IndexMask = 3;
    static constexpr unsigned Dirty = 4;
    struct alignas(64) Slot { T value{}; };
    std::array<Slot, 3> slots_{};
    alignas(64) unsigned back_ = 2; // producer-owned slot
    alignas(64) unsigned front_ = 0; // consumer-owned slot
    alignas(64) std::atomic<unsigned> middle_{1};
};

// The same ownership exchange with externally provided fixed storage. Large
// engine records live in loader-zeroed POD BSS, keeping construction away from
// the Win32 startup stack. The caller initializes all slots before either
// thread starts and retains them until both owners have stopped.
template<class T> class ExternalTriplePublication {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<unsigned>::is_always_lock_free);
public:
    explicit ExternalTriplePublication(T (&storage)[3]) noexcept : slots_(storage) {}
    ExternalTriplePublication(const ExternalTriplePublication&) = delete;
    ExternalTriplePublication& operator=(const ExternalTriplePublication&) = delete;
    T& writable() noexcept { return slots_[back_]; }
    void publish() noexcept {
        back_ = middle_.exchange(back_ | Dirty, std::memory_order_acq_rel) & IndexMask;
    }
    const T& readLatest() noexcept {
        if (middle_.load(std::memory_order_acquire) & Dirty)
            front_ = middle_.exchange(front_, std::memory_order_acq_rel) & IndexMask;
        return slots_[front_];
    }
private:
    static constexpr unsigned IndexMask = 3, Dirty = 4;
    T* slots_;
    alignas(64) unsigned back_ = 2;
    alignas(64) unsigned front_ = 0;
    alignas(64) std::atomic<unsigned> middle_{1};
};

} // namespace khz
