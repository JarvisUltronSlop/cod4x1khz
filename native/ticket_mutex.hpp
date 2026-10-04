#pragma once
#include <atomic>

namespace khz {
// Serve queued owners in order. Waiting uses the platform's atomic wait;
// uncontended acquisition needs no kernel wait or heap allocation.
class TicketMutex {
    std::atomic<unsigned> next_{0}, serving_{0};
public:
    void lock() noexcept {
        const unsigned ticket = next_.fetch_add(1, std::memory_order_relaxed);
        unsigned current = serving_.load(std::memory_order_acquire);
        while (current != ticket) {
            serving_.wait(current, std::memory_order_relaxed);
            current = serving_.load(std::memory_order_acquire);
        }
    }
    void unlock() noexcept {
        serving_.fetch_add(1, std::memory_order_release);
        serving_.notify_all();
    }
    // Diagnostic only: includes the current holder and queued acquisitions.
    unsigned pending() const noexcept {
        return next_.load(std::memory_order_relaxed) - serving_.load(std::memory_order_relaxed);
    }
};
}
