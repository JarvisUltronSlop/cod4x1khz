#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "debug_context.hpp"
#include <cstdio>
#include <cwchar>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && !std::wcscmp(argv[1], L"--child")) return 0;
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, 32768)) return 1;
    // An optional executable exercises an x64 probe inspecting a WOW64 child.
    const wchar_t* child = argc == 2 ? argv[1] : executable;
    std::wstring command = L"\"" + std::wstring(child) + L"\" --child";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(child, command.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 1;
    BOOL wow64 = FALSE;
    const bool architectureKnown = IsWow64Process(process.hProcess, &wow64) != FALSE;
    khz::DebugContext context;
    const bool captured = context.capture(process.hProcess, process.hThread);
#if defined(_WIN64)
    const auto expectedMachine = wow64 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
#else
    const auto expectedMachine = IMAGE_FILE_MACHINE_I386;
#endif
    const bool passed = architectureKnown && captured && context.machine == expectedMachine &&
        context.frame.AddrPC.Offset && context.frame.AddrStack.Offset;
    if (!TerminateProcess(process.hProcess, 0) || WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
        CloseHandle(process.hThread); CloseHandle(process.hProcess); return 1;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    std::printf("probe_bits=%zu target_machine=%04lx pc=%llx sp=%llx captured=%d\n",
        sizeof(void*) * 8, context.machine, context.frame.AddrPC.Offset,
        context.frame.AddrStack.Offset, passed);
    return passed ? 0 : 1;
}
