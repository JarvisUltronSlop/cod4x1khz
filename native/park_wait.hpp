#pragma once
#include <atomic>
#include <cstdint>

namespace khz::park_wait {
// An event is a wake hint; the release-published worker phase is the actual
// acknowledgement. Event supplies reset(), wait(milliseconds), now(). wait()
// returns false only for a failed wait; a timeout is followed by another phase
// check. One serialized main owner waits; the worker publishes parked and sets
// the manual-reset event after leaving every mutable gameplay callback.
template<class Phase, class Event>
bool acknowledged(std::atomic<Phase>& phase, const std::atomic<bool>& requested,
                  Phase running, Phase parking, Phase parked, Phase fault,
                  Event& event, unsigned timeout) {
    Phase expected = running;
    // Never replace a Parked/Fault phase published after the main owner's read.
    phase.compare_exchange_strong(expected, parking, std::memory_order_acq_rel, std::memory_order_acquire);
    const std::uint64_t started = event.now();
    for (;;) {
        // Clear stale notifications BEFORE inspecting the acknowledgement. A
        // worker arriving afterward either makes the phase check succeed or
        // leaves a new event for wait(). Resetting after the check loses wakes.
        if (!event.reset()) return false;
        const auto observed = phase.load(std::memory_order_acquire);
        if (observed == parked) return requested.load(std::memory_order_acquire);
        if (observed == fault) return false;
        const std::uint64_t elapsed = event.now() - started;
        if (elapsed >= timeout) return false;
        if (!event.wait(timeout - static_cast<unsigned>(elapsed))) return false;
    }
}
}
