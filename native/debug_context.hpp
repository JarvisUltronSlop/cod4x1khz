#pragma once
#include <Windows.h>
#include <DbgHelp.h>

namespace khz {
// The caller suspends the thread or owns its current debug event.
// An x64 probe can inspect both native x64 and WOW64 engine processes.
struct DebugContext {
    CONTEXT native{};
#if defined(_WIN64)
    WOW64_CONTEXT wow64{};
#endif
    STACKFRAME64 frame{};
    DWORD machine = 0;

    void* registers() noexcept {
#if defined(_WIN64)
        if (machine == IMAGE_FILE_MACHINE_I386) return &wow64;
#endif
        return &native;
    }

    bool capture(HANDLE process, HANDLE thread) noexcept {
        BOOL targetWow64 = FALSE;
        if (!IsWow64Process(process, &targetWow64)) return false;
        frame = {};
#if defined(_WIN64)
        if (targetWow64) {
            machine = IMAGE_FILE_MACHINE_I386;
            wow64.ContextFlags = WOW64_CONTEXT_FULL;
            if (!Wow64GetThreadContext(thread, &wow64)) return false;
            frame.AddrPC = {wow64.Eip, 0, AddrModeFlat};
            frame.AddrFrame = {wow64.Ebp, 0, AddrModeFlat};
            frame.AddrStack = {wow64.Esp, 0, AddrModeFlat};
            return true;
        }
        machine = IMAGE_FILE_MACHINE_AMD64;
        native.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(thread, &native)) return false;
        frame.AddrPC = {native.Rip, 0, AddrModeFlat};
        frame.AddrFrame = {native.Rbp, 0, AddrModeFlat};
        frame.AddrStack = {native.Rsp, 0, AddrModeFlat};
#else
        BOOL probeWow64 = FALSE;
        if (!IsWow64Process(GetCurrentProcess(), &probeWow64)) return false;
        if (probeWow64 && !targetWow64) {
            SetLastError(ERROR_NOT_SUPPORTED);
            return false;
        }
        machine = IMAGE_FILE_MACHINE_I386;
        native.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(thread, &native)) return false;
        frame.AddrPC = {native.Eip, 0, AddrModeFlat};
        frame.AddrFrame = {native.Ebp, 0, AddrModeFlat};
        frame.AddrStack = {native.Esp, 0, AddrModeFlat};
#endif
        return true;
    }
};
}
