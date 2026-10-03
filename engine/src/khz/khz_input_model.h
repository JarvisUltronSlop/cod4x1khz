#pragma once

#include "khz_input_owner.h"
#include <array>
#include <atomic>
#include <limits>

// Portable models used by both the production producer and isolated tests.
// Ring has exactly one producer/consumer; resets require their barrier.
namespace khz_input {
enum class WindowStart { Reuse, AwaitPark, Recycle, Reject };
inline WindowStart WindowStartPolicy(std::uintptr_t current,std::uintptr_t next,bool running,bool requestedPark,bool acknowledgedPark){
    if(!next||!running)return WindowStart::Reject;
    if(current==next)return WindowStart::Reuse;
    if(!requestedPark)return WindowStart::Reject;
    return acknowledgedPark?WindowStart::Recycle:WindowStart::AwaitPark;
}
inline bool ParkAcknowledged(std::uint64_t version,std::uint64_t acknowledged,bool running,bool parked,bool active){
    return !(version&1)&&running&&parked&&!active&&acknowledged==version;
}
inline bool CheckpointParked(std::uint64_t version,std::uint64_t acknowledged,bool running,bool parked,bool active,unsigned queued,bool overflow){
    return ParkAcknowledged(version,acknowledged,running,parked,active)&&queued==0&&!overflow;
}
template<class Event, unsigned Capacity> class Ring {
    static_assert(Capacity && !(Capacity & (Capacity - 1)), "power of two capacity required");
public:
    bool Push(const Event& event) {
        const unsigned write = write_.load(std::memory_order_relaxed);
        if (write - read_.load(std::memory_order_acquire) >= Capacity) return false;
        events_[write & (Capacity - 1)] = event;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }
    bool Pop(Event* event, std::uint64_t deadline = (std::numeric_limits<std::uint64_t>::max)()) {
        const unsigned read = read_.load(std::memory_order_relaxed);
        if (!event || read == write_.load(std::memory_order_acquire)) return false;
        if (events_[read & (Capacity - 1)].qpc > deadline) return false;
        *event = events_[read & (Capacity - 1)];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }
    void DiscardFromConsumer() { read_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }
    void ResetQuiescent(unsigned index = 0) { read_.store(index); write_.store(index); }
    unsigned Size() const {
        const unsigned write = write_.load(std::memory_order_acquire);
        const unsigned read = read_.load(std::memory_order_acquire);
        const unsigned size = write - read;
        return size <= Capacity ? size : 0;
    }
private:
    std::array<Event, Capacity> events_{};
    alignas(64) std::atomic<unsigned> read_{0}, write_{0};
};

struct ControlState {
    std::uint64_t epoch = 0;
    bool active = false, focused = false, parked = true;
    bool Refresh(bool requested, bool nextParked, bool nextFocused, std::uint64_t nextEpoch,
        KHZ_InputResetReason* reason) {
        const bool nextActive = requested && !nextParked && nextFocused;
        const bool changed = nextEpoch != epoch || nextFocused != focused || nextParked != parked || nextActive != active;
        *reason = nextEpoch != epoch ? KHZ_InputResetReason::Control
            : nextParked != parked && nextParked ? KHZ_InputResetReason::Park
            : nextFocused != focused ? (nextFocused ? KHZ_InputResetReason::FocusGained : KHZ_InputResetReason::FocusLost)
            : KHZ_InputResetReason::Control;
        epoch = nextEpoch; active = nextActive; focused = nextFocused; parked = nextParked;
        return changed;
    }
};

// This timeline belongs exclusively to the frontend/control owner. Its times
// are GetTickCount64 milliseconds, while Win32 MSG.time is its low DWORD.
// Closed endpoints deliberately include their boundary millisecond: an old
// gameplay message cannot leak into a newly opened menu. The separate owner
// navigation queue bypasses this legacy-message filter.
class OwnershipTimeline {
public:
    static constexpr unsigned Capacity = 128;
    void Set(bool owned, std::uint64_t epoch, std::uint64_t milliseconds) {
        if (milliseconds < lastTime_) milliseconds = lastTime_;
        lastTime_ = milliseconds;
        if (owned_ && (!owned || epoch != epoch_)) {
            intervals_[(write_ - 1) & (Capacity - 1)].end = milliseconds;
            owned_ = false;
        }
        if (owned && !owned_) {
            if (write_ >= Capacity) {
                const auto& retired = intervals_[write_ & (Capacity - 1)];
                discardedThrough_ = retired.end;
                discarded_ = true;
            }
            intervals_[write_ & (Capacity - 1)] = { milliseconds, (std::numeric_limits<std::uint64_t>::max)(), epoch };
            ++write_;
            owned_ = true;
            epoch_ = epoch;
        }
    }
    bool Suppress(unsigned messageTime, std::uint64_t now) const {
        const unsigned age = unsigned(now) - messageTime;
        // A future/ambiguous timestamp cannot be proven to belong to a menu.
        // Conservatively reject it once gameplay ownership has existed.
        if (age > 0x7fffffffu || now < age) return write_ != 0;
        const std::uint64_t timestamp = now - age;
        if (discarded_ && timestamp <= discardedThrough_) return true;
        const std::uint64_t count = write_ < Capacity ? write_ : Capacity;
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto& interval = intervals_[(write_ - 1 - i) & (Capacity - 1)];
            if (timestamp >= interval.begin && timestamp <= interval.end) return true;
        }
        return false;
    }
private:
    struct Interval { std::uint64_t begin = 0, end = 0, epoch = 0; };
    std::array<Interval, Capacity> intervals_{};
    std::uint64_t write_ = 0, epoch_ = 0, lastTime_ = 0, discardedThrough_ = 0;
    bool owned_ = false, discarded_ = false;
};

inline bool ModelSelfTest() {
    if(WindowStartPolicy(1,1,true,false,false)!=WindowStart::Reuse
        ||WindowStartPolicy(1,2,true,true,true)!=WindowStart::Recycle
        ||WindowStartPolicy(1,2,true,true,false)!=WindowStart::AwaitPark
        ||WindowStartPolicy(1,2,true,false,true)!=WindowStart::Reject
        ||WindowStartPolicy(1,2,false,true,true)!=WindowStart::Reject
        ||WindowStartPolicy(1,0,true,true,true)!=WindowStart::Reject)return false;
    if(ParkAcknowledged(1,1,true,true,false)||ParkAcknowledged(2,0,true,true,false)
        ||ParkAcknowledged(2,2,false,true,false)||ParkAcknowledged(2,2,true,false,false)
        ||ParkAcknowledged(2,2,true,true,true)||!ParkAcknowledged(2,2,true,true,false))return false;
    if(CheckpointParked(2,2,true,true,false,1,false)||CheckpointParked(2,2,true,true,false,0,true)
        ||!CheckpointParked(2,2,true,true,false,0,false))return false;
    if(ParkAcknowledged(0,UINT64_MAX,true,true,false)||!ParkAcknowledged(0,0,true,true,false))return false;
    Ring<KHZ_OwnedInputEvent, 4096> ring;
    KHZ_OwnedInputEvent event{}, result{};
    ring.ResetQuiescent((std::numeric_limits<unsigned>::max)() - 2048);
    if (ring.Pop(&result)) return false;
    for (unsigned i = 0; i < 4096; ++i) {
        event.sequence = i + 1; event.controlEpoch = 7; event.qpc = 1000 + i;
        if (!ring.Push(event)) return false;
    }
    if (ring.Size() != 4096 || ring.Push(event)) return false;
    for (unsigned i = 0; i < 4096; ++i)
        if (!ring.Pop(&result) || result.sequence != i + 1 || result.controlEpoch != 7 || result.qpc != 1000 + i) return false;
    if (ring.Size() || ring.Pop(&result)) return false;
    for (unsigned i = 0; i < 8192; ++i) {
        event.sequence = i; if (!ring.Push(event) || !ring.Pop(&result) || result.sequence != i) return false;
    }
    event.qpc = 2000;
    if (!ring.Push(event) || ring.Pop(&result, 1999) || !ring.Pop(&result, 2000)) return false;
    ring.Push(event); ring.DiscardFromConsumer();
    if (ring.Size() || ring.Pop(&result)) return false;

    ControlState control;
    KHZ_InputResetReason reason{};
    if (!control.Refresh(true, false, true, 7, &reason) || !control.active || reason != KHZ_InputResetReason::Control) return false;
    if (control.Refresh(true, false, true, 7, &reason)) return false;
    if (!control.Refresh(true, false, false, 7, &reason) || control.active || reason != KHZ_InputResetReason::FocusLost) return false;
    if (!control.Refresh(true, false, true, 7, &reason) || reason != KHZ_InputResetReason::FocusGained) return false;
    if (!control.Refresh(true, true, true, 7, &reason) || control.active || reason != KHZ_InputResetReason::Park) return false;
    if (!control.Refresh(true, false, true, 8, &reason) || control.epoch != 8 || !control.active || reason != KHZ_InputResetReason::Control) return false;

    OwnershipTimeline history;
    history.Set(true, 7, 100); history.Set(false, 7, 200);
    if (history.Suppress(99, 300) || !history.Suppress(100, 300) || !history.Suppress(199, 300)
        || !history.Suppress(200, 300) || history.Suppress(201, 300)) return false;
    history.Set(true, 8, 400); history.Set(true, 9, 500); history.Set(false, 9, 600);
    if (!history.Suppress(450, 800) || !history.Suppress(500, 800) || history.Suppress(650, 800)) return false;
    const std::uint64_t wrap = 0x100000000ull;
    OwnershipTimeline wrapHistory;
    wrapHistory.Set(true, 1, wrap - 5); wrapHistory.Set(false, 1, wrap + 5);
    if (!wrapHistory.Suppress(0xfffffffeu, wrap + 20) || !wrapHistory.Suppress(2, wrap + 20)
        || wrapHistory.Suppress(6, wrap + 20) || wrapHistory.Suppress(0xfffffffau, wrap + 20)) return false;
    for (unsigned i = 0; i < 129; ++i) { history.Set(true, i + 10, 1000 + 10 * i); history.Set(false, i + 10, 1005 + 10 * i); }
    if (!history.Suppress(150, 3000) || !history.Suppress(2281, 3000) || history.Suppress(2500, 3000)) return false;
    return true;
}
}
