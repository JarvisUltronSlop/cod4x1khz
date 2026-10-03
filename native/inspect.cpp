#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>
#include <DbgHelp.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstddef>
#include <client_mp/client_mp.h>
#include <core.hpp>
#include <gfx_d3d/r_scene.h>

template<class T> static void rendererMember(HANDLE process, const char* object, const char* member, size_t offset) {
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 1024]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 1023;
    T value{}; SIZE_T received = 0;
    if (SymFromName(process, object, symbol) && symbol->Size >= offset + sizeof(T) &&
        ReadProcessMemory(process, reinterpret_cast<void*>(static_cast<std::uintptr_t>(symbol->Address + offset)),
            &value, sizeof(value), &received) && received == sizeof(value))
        std::printf("RendererMember %s.%s=%lld\n", object, member, static_cast<long long>(value));
}
static void rendererState(HANDLE process) {
    rendererMember<unsigned>(process, "scene", "sceneDObjCount", offsetof(GfxScene, sceneDObjCount));
    rendererMember<unsigned>(process, "scene", "sceneModelCount", offsetof(GfxScene, sceneModelCount));
    rendererMember<unsigned>(process, "scene", "sceneBrushCount", offsetof(GfxScene, sceneBrushCount));
    rendererMember<unsigned>(process, "scene", "sceneDynModelCount", offsetof(GfxScene, sceneDynModelCount));
    rendererMember<unsigned>(process, "scene", "sceneDynBrushCount", offsetof(GfxScene, sceneDynBrushCount));
}

static bool symbolAddress(HANDLE process, const char* name, DWORD64* address, ULONG* bytes = nullptr) {
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 1024]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 1023;
    if (!SymFromName(process, name, symbol)) return false;
    *address = symbol->Address; if (bytes) *bytes = symbol->Size; return true;
}
template<class T> static bool remoteValue(HANDLE process, DWORD64 address, T* value) {
    SIZE_T received = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)),
        value, sizeof(T), &received) && received == sizeof(T);
}
template<class T> static void nativeGlobal(HANDLE process, const char* name) {
    DWORD64 address = 0; ULONG bytes = 0; T value{};
    if (symbolAddress(process, name, &address, &bytes) && bytes == sizeof(T) && remoteValue(process, address, &value))
        std::printf("NativeScalar name=%s address=%llx bytes=%lu value=%lld\n", name, address, bytes, static_cast<long long>(value));
    else std::printf("NativeScalar name=%s unavailable\n", name);
}
static void predictionTransport(HANDLE process) {
    DWORD64 address = 0; ULONG bytes = 0;
    // Compile the actual current MP type offsets. Never infer a native field
    // from decompiler size comments or copied console state.
    if (symbolAddress(process, "khz_clients", &address, &bytes) && bytes >= sizeof(clientActive_t)) {
        struct Field { const char* name; std::size_t offset; };
        const Field fields[] = {{"cmd_number", offsetof(clientActive_t, cmdNumber)},
            {"server_time", offsetof(clientActive_t, serverTime)},
            {"snap_valid", offsetof(clientActive_t, snap) + offsetof(clSnapshot_t, valid)},
            {"snap_message", offsetof(clientActive_t, snap) + offsetof(clSnapshot_t, messageNum)},
            {"snap_time", offsetof(clientActive_t, snap) + offsetof(clSnapshot_t, serverTime)},
            {"snap_command_time", offsetof(clientActive_t, snap) + offsetof(clSnapshot_t, ps) + offsetof(playerState_s, commandTime)}};
        std::printf("NativeClient address=%llx compiled_bytes=%zu symbol_bytes=%lu", address, sizeof(clientActive_t), bytes);
        for (const auto& field : fields) { int value = 0;
            if (remoteValue(process, address + field.offset, &value)) std::printf(" %s=%d", field.name, value);
            else std::printf(" %s=unavailable", field.name);
        } std::putchar('\n');
    } else std::puts("NativeClient exact typed storage unavailable");
    nativeGlobal<int>(process, "inputTime");
    nativeGlobal<std::uint64_t>(process, "predictionSteps");
    nativeGlobal<std::uint64_t>(process, "predictionTicks");
    for (const auto* name : {"predictionHeader", "serverHeader"}) {
        khz::WireHeader header{};
        if (symbolAddress(process, name, &address, &bytes) && bytes == sizeof(header) && remoteValue(process, address, &header))
            std::printf("NativeWire name=%s session=%llu tick=%llu input=%llu ack=%llu\n", name, header.session, header.tick, header.input, header.ack);
        else std::printf("NativeWire name=%s unavailable\n", name);
    }
}

static unsigned long long fileTicks(const FILETIME& value) {
    return (static_cast<unsigned long long>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

static void criticalSections(HANDLE process) {
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 1024]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 1023;
    if (!SymFromName(process, "s_criticalSections", symbol)) {
        std::printf("CriticalSections symbol unavailable error=%lu\n", GetLastError()); return;
    }
    constexpr unsigned capacity = 64;
    RTL_CRITICAL_SECTION values[capacity]{}; SIZE_T received = 0;
    const unsigned count = symbol->Size / sizeof(values[0]);
    if (!count || count > capacity || symbol->Size % sizeof(values[0]) ||
        !ReadProcessMemory(process, reinterpret_cast<void*>(static_cast<std::uintptr_t>(symbol->Address)),
            values, symbol->Size, &received) || received != symbol->Size) {
        std::printf("CriticalSections read unavailable address=%llx bytes=%lu received=%zu error=%lu\n",
            symbol->Address, symbol->Size, received, GetLastError()); return;
    }
    std::printf("CriticalSections address=%llx count=%u native_size=%zu\n", symbol->Address, count, sizeof(values[0]));
    for (unsigned i = 0; i != count; ++i)
        std::printf("  CriticalSection index=%u lock=%ld recursion=%ld owning_thread=%zu spin=%zu\n",
            i, values[i].LockCount, values[i].RecursionCount,
            reinterpret_cast<std::uintptr_t>(values[i].OwningThread), values[i].SpinCount);
}

// Developer-only stack sampler. Each thread is resumed before moving on.
int main(int argc, char** argv) {
    if (argc != 3) { std::puts("khz_inspect <pid> <symbol directory>"); return 1; }
    const auto pid = std::strtoul(argv[1], nullptr, 10);
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return 2;
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitialize(process, argv[2], TRUE)) { CloseHandle(process); return 3; }
    LARGE_INTEGER qpc{}, frequency{}; QueryPerformanceCounter(&qpc); QueryPerformanceFrequency(&frequency);
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetProcessTimes(process, &creation, &exit, &kernel, &user))
        std::printf("Process pid=%lu qpc=%lld frequency=%lld kernel_100ns=%llu user_100ns=%llu\n",
            pid, qpc.QuadPart, frequency.QuadPart, fileTicks(kernel), fileTicks(user));
    criticalSections(process);
    predictionTransport(process);
    rendererState(process);
    HANDLE list = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    if (Thread32First(list, &entry)) do {
        if (entry.th32OwnerProcessID != pid) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        FILETIME threadCreation{}, threadExit{}, threadKernel{}, threadUser{};
        const bool hasTimes = GetThreadTimes(thread, &threadCreation, &threadExit, &threadKernel, &threadUser) != FALSE;
        if (SuspendThread(thread) == DWORD(-1)) { CloseHandle(thread); continue; }
        CONTEXT context{}; context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context)) {
            STACKFRAME64 frame{};
            frame.AddrPC = {context.Eip, 0, AddrModeFlat};
            frame.AddrFrame = {context.Ebp, 0, AddrModeFlat};
            frame.AddrStack = {context.Esp, 0, AddrModeFlat};
            std::printf("Thread %lu\n", entry.th32ThreadID);
            if (hasTimes) std::printf("  ThreadCPU kernel_100ns=%llu user_100ns=%llu\n", fileTicks(threadKernel), fileTicks(threadUser));
            for (unsigned i = 0; i < 30 && frame.AddrPC.Offset; ++i) {
                alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 1024]{};
                auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 1023;
                DWORD64 displacement = 0; IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line); DWORD lineOffset = 0;
                if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) {
                    std::printf("  %s + %llu", symbol->Name, displacement);
                    if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineOffset, &line)) std::printf(" (%s:%lu)", line.FileName, line.LineNumber);
                    std::putchar('\n');
                } else std::printf("  %llx\n", frame.AddrPC.Offset);
                if (!StackWalk64(IMAGE_FILE_MACHINE_I386, process, thread, &frame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
            }
        }
        ResumeThread(thread); CloseHandle(thread);
    } while (Thread32Next(list, &entry));
    CloseHandle(list); SymCleanup(process); CloseHandle(process);
}
