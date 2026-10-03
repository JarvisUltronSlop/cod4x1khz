#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <intrin.h>
#include "core.hpp"

namespace khz {
std::uint64_t counter() { LARGE_INTEGER value; QueryPerformanceCounter(&value); return value.QuadPart; }
std::uint64_t frequency() { static const auto value = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<std::uint64_t>(f.QuadPart); }(); return value; }
bool waitUntilOrEvent(std::uint64_t deadline, void* event) {
    struct Timer {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* high resolution */, TIMER_ALL_ACCESS);
        Timer() { if (!handle) handle = CreateWaitableTimerW(nullptr, FALSE, nullptr); }
        ~Timer() { if (handle) CloseHandle(handle); }
    };
    static thread_local Timer timer;
    const auto wakeEvent = static_cast<HANDLE>(event);
    const auto f = frequency(); const auto spin = f / 10000; // final 100 us
    bool armed = false;
    const auto finish = [&](bool eventReady) {
        // An early renderer wake must not leave an old timer armed for the
        // next scheduler wait on this thread.
        if (armed) CancelWaitableTimer(timer.handle);
        return eventReady;
    };
    for (;;) {
        const auto now = counter(); if (now >= deadline) return finish(false);
        if (wakeEvent && WaitForSingleObject(wakeEvent, 0) == WAIT_OBJECT_0)
            return finish(counter() < deadline);
        const auto remaining = deadline - now;
        if (remaining > spin) {
            if (!timer.handle) { SwitchToThread(); continue; }
            LARGE_INTEGER relative; relative.QuadPart = -static_cast<LONGLONG>(((remaining - spin) * 10000000ULL) / f);
            if (SetWaitableTimer(timer.handle, &relative, 0, nullptr, nullptr, FALSE)) {
                armed = true;
                // The QPC check remains authoritative. A finite timeout also
                // keeps a failed or unexpectedly delayed timer from blocking
                // the main gameplay scheduler indefinitely.
                const auto milliseconds = (remaining / f) * 1000
                    + ((remaining % f) * 1000 + f - 1) / f;
                const auto timeout = static_cast<DWORD>((std::min)(milliseconds, std::uint64_t(MAXDWORD - 1)));
                DWORD result;
                if (wakeEvent) {
                    const HANDLE handles[] = {wakeEvent, timer.handle};
                    result = WaitForMultipleObjects(2, handles, FALSE, timeout);
                    if (result == WAIT_OBJECT_0) return finish(counter() < deadline);
                    if (result == WAIT_OBJECT_0 + 1) armed = false;
                } else {
                    result = WaitForSingleObject(timer.handle, timeout);
                    if (result == WAIT_OBJECT_0) armed = false;
                }
                if (result == WAIT_FAILED) SwitchToThread();
            } else SwitchToThread();
        } else _mm_pause();
    }
}
void waitUntil(std::uint64_t deadline) { waitUntilOrEvent(deadline, nullptr); }
}
