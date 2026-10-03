#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <park_wait.hpp>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>

enum class Phase { Running, Parking, Parked, Fault };
struct Fixture {
    std::atomic<Phase> phase{Phase::Running};
    std::atomic<bool> requested{true};
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    unsigned resets = 0, waits = 0;
    std::uint64_t clock = 0;
    bool failReset = false, failWait = false;
    bool ackDuringReset = false, ackAtWait = false, ackAtTimeout = false;
    Fixture() { assert(event); }
    ~Fixture() { CloseHandle(event); }
    std::uint64_t now() { return clock; }
    void ack() { phase.store(Phase::Parked, std::memory_order_release); assert(SetEvent(event)); }
    bool reset() {
        ++resets;
        if (failReset) return false;
        const bool okay = ResetEvent(event) != FALSE;
        if (ackDuringReset) { ackDuringReset = false; ack(); }
        return okay;
    }
    bool wait(unsigned ms) {
        ++waits;
        if (failWait) return false;
        if (ackAtWait) { ackAtWait = false; ack(); }
        // Bound real test waits; advance the controlled deadline on timeout.
        const DWORD result = WaitForSingleObject(event, 0);
        if (result == WAIT_TIMEOUT) { clock += ms; if (ackAtTimeout) ack(); }
        return result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT;
    }
    bool run(unsigned timeout = 20) {
        return khz::park_wait::acknowledged(phase, requested, Phase::Running, Phase::Parking,
                                           Phase::Parked, Phase::Fault, *this, timeout);
    }
};
int main() {
    {
        Fixture f; assert(SetEvent(f.event)); // Old park event left after resume.
        assert(!f.run()); assert(f.waits == 1 && f.phase == Phase::Parking);
    }
    {
        Fixture f; f.phase = Phase::Parked; // Worker wins Running→Parked race.
        assert(f.run()); assert(f.phase == Phase::Parked && f.waits == 0);
    }
    {
        Fixture f; f.ackDuringReset = true; // Ack at reset/check boundary.
        assert(f.run()); assert(f.waits == 0 && f.phase == Phase::Parked);
    }
    {
        Fixture f; f.ackAtWait = true; // Ack after check, before native wait.
        assert(f.run()); assert(f.waits == 1 && f.resets == 2);
    }
    {
        Fixture f; assert(SetEvent(f.event)); f.ackAtWait = true;
        assert(f.run()); assert(f.waits == 1); // Stale event cannot skip fresh ack.
    }
    {
        Fixture f; f.ackAtTimeout = true;
        assert(f.run()); assert(f.waits == 1); // Acquire real ack before deciding timeout.
    }
    {
        Fixture f; f.phase = Phase::Fault; assert(SetEvent(f.event));
        assert(!f.run()); assert(f.phase == Phase::Fault && f.waits == 0);
    }
    { Fixture f; f.failReset = true; assert(!f.run()); }
    { Fixture f; f.failWait = true; assert(!f.run()); }
    { Fixture f; f.phase = Phase::Parked; f.requested = false; assert(!f.run()); }
    { Fixture f; assert(!f.run(0)); assert(f.waits == 0); }
    {
        struct NativeEvent {
            HANDLE handle;
            bool reset() { return ResetEvent(handle) != FALSE; }
            bool wait(unsigned ms) { const DWORD r = WaitForSingleObject(handle, ms); return r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT; }
            std::uint64_t now() { return GetTickCount64(); }
        } event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        assert(event.handle && wake);
        std::atomic<Phase> phase{Phase::Running};
        std::atomic<bool> requested{false}, stop{false};
        unsigned payload = 0;
        std::thread worker([&] {
            for (;;) {
                assert(WaitForSingleObject(wake, 2000) == WAIT_OBJECT_0);
                if (stop.load(std::memory_order_acquire)) break;
                assert(requested.load(std::memory_order_acquire));
                ++payload; // Published mutable state precedes actual parked phase.
                phase.store(Phase::Parked, std::memory_order_release);
                assert(SetEvent(event.handle));
            }
        });
        for (unsigned i = 0; i < 1024; ++i) {
            phase.store(Phase::Running, std::memory_order_release);
            requested.store(true, std::memory_order_release);
            // Deliberately retain the preceding manual-reset signal each run.
            assert(SetEvent(wake));
            assert(khz::park_wait::acknowledged(phase, requested, Phase::Running, Phase::Parking,
                                               Phase::Parked, Phase::Fault, event, 2000));
            assert(phase == Phase::Parked && payload == i + 1);
            requested.store(false, std::memory_order_release);
        }
        stop.store(true, std::memory_order_release); assert(SetEvent(wake)); worker.join();
        CloseHandle(wake); CloseHandle(event.handle);
    }
    std::puts("Park acknowledgements: stale signals, lost-wake boundaries, phase race, failures, deadlines and 1024 native event handoffs passed");
}
