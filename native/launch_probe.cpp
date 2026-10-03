#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <cstdio>
#include <cwchar>

// A developer-only debugger for failures before the engine installs its handler.
// It owns and terminates only the process it creates.
int wmain(int argc, wchar_t** argv) {
    if (argc != 4) { std::puts("khz_launch_probe <executable> <cwd> <symbol directory>"); return 2; }
    wchar_t command[2048];
    if (std::swprintf(command, 2048, L"\"%ls\" allowdupe +set r_fullscreen 0 +set net_port 28994", argv[1]) < 0) return 2;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(argv[1], command, nullptr, nullptr, FALSE, DEBUG_ONLY_THIS_PROCESS,
        nullptr, argv[2], &startup, &process)) { std::printf("CreateProcess error=%lu\n", GetLastError()); return 2; }
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitializeW(process.hProcess, argv[3], TRUE);
    const auto until = GetTickCount64() + 15000;
    bool fault = false, exited = false;
    while (!exited && GetTickCount64() < until) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, 100)) continue;
        DWORD continuation = DBG_CONTINUE;
        if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const auto& exception = event.u.Exception;
            const auto code = exception.ExceptionRecord.ExceptionCode;
            if (code != EXCEPTION_BREAKPOINT && code != EXCEPTION_SINGLE_STEP) continuation = DBG_EXCEPTION_NOT_HANDLED;
            if (!exception.dwFirstChance || code == EXCEPTION_STACK_OVERFLOW) {
                fault = true;
                std::printf("Exception %08lx firstChance=%lu at %p\n", code, exception.dwFirstChance, exception.ExceptionRecord.ExceptionAddress);
                HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, event.dwThreadId);
                CONTEXT context{}; context.ContextFlags = CONTEXT_FULL;
                if (thread && GetThreadContext(thread, &context)) {
                    STACKFRAME64 frame{};
                    frame.AddrPC = {context.Eip, 0, AddrModeFlat}; frame.AddrFrame = {context.Ebp, 0, AddrModeFlat}; frame.AddrStack = {context.Esp, 0, AddrModeFlat};
                    for (unsigned i = 0; i < 48 && frame.AddrPC.Offset; ++i) {
                        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 1024]{};
                        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage); symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 1023;
                        DWORD64 displacement = 0; IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line); DWORD lineOffset = 0;
                        if (SymFromAddr(process.hProcess, frame.AddrPC.Offset, &displacement, symbol)) {
                            std::printf("  %s + %llu", symbol->Name, displacement);
                            if (SymGetLineFromAddr64(process.hProcess, frame.AddrPC.Offset, &lineOffset, &line)) std::printf(" (%s:%lu)", line.FileName, line.LineNumber);
                            std::putchar('\n');
                        } else std::printf("  %llx\n", frame.AddrPC.Offset);
                        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, process.hProcess, thread, &frame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
                    }
                }
                if (thread) CloseHandle(thread);
                TerminateProcess(process.hProcess, code);
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            std::printf("Exit=%08lx\n", event.u.ExitProcess.dwExitCode); exited = true;
        } else if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            SymLoadModuleExW(process.hProcess, event.u.CreateProcessInfo.hFile, argv[1], nullptr,
                reinterpret_cast<DWORD64>(event.u.CreateProcessInfo.lpBaseOfImage), 0, nullptr, 0);
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
        }
        else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation);
    }
    if (!exited) { std::puts("Startup probe reached 15 seconds without a fatal exception."); TerminateProcess(process.hProcess, 0); }
    SymCleanup(process.hProcess); CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return fault ? 1 : 0;
}
