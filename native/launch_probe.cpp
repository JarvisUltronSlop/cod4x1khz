#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include "debug_context.hpp"

// A developer-only debugger for failures before the engine installs its handler.
// It owns and terminates only the process it creates.
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if (argc != 4&&argc!=5) { std::puts("khz_launch_probe <executable> <cwd> <symbol directory> [extra arguments]"); return 2; }
    wchar_t command[4096];
    if (std::swprintf(command, 4096, L"\"%ls\" allowdupe +set r_fullscreen 0 +set net_port 28994 %ls", argv[1],argc==5?argv[4]:L"") < 0) return 2;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    char hideWindow[2]{};
    if (GetEnvironmentVariableA("KHZ_PROBE_HIDE_WINDOW", hideWindow, sizeof(hideWindow)) && hideWindow[0] == '1') {
        startup.dwFlags |= STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(argv[1], command, nullptr, nullptr, FALSE, DEBUG_ONLY_THIS_PROCESS,
        nullptr, argv[2], &startup, &process)) { std::printf("CreateProcess error=%lu\n", GetLastError()); return 2; }
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitializeW(process.hProcess, argv[3], FALSE)) std::printf("SymInitialize error=%lu\n", GetLastError());
    char duration[16]{};unsigned durationSeconds=15;
    if(GetEnvironmentVariableA("KHZ_PROBE_SECONDS",duration,sizeof(duration))) {
        const auto requested=std::strtoul(duration,nullptr,10);if(requested>=1&&requested<=360)durationSeconds=unsigned(requested);
    }
    std::printf("Owned client PID=%lu\n",process.dwProcessId);std::fflush(stdout);
    const auto until = GetTickCount64() + durationSeconds*1000;
    bool fault = false, exited = false, sawInitialBreakpoint = false;
#if defined(_WIN64)
    const bool watchHunk = _wgetenv(L"KHZ_WATCH_HUNK") != nullptr;
    const char* watchSymbol = std::getenv("KHZ_WATCH_SYMBOL");
    DWORD64 watchedHunk = 0;
#endif
    while (!exited && GetTickCount64() < until) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, 10)) {
#if defined(_WIN64)
            if ((watchHunk || watchSymbol) && !watchedHunk) {
                alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256]{};
                auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 255;
                if (SymFromName(process.hProcess, watchSymbol ? watchSymbol : "g_debugUser", symbol)) {
                    SIZE_T bytes = 0; DWORD64 user = 0;
                    if (ReadProcessMemory(process.hProcess, reinterpret_cast<void*>(symbol->Address), &user, sizeof(user), &bytes) && user) {
                        SuspendThread(process.hThread);
                        CONTEXT registers{}; registers.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                        if (GetThreadContext(process.hThread, &registers)) {
                            registers.Dr0 = watchSymbol ? symbol->Address : user; registers.Dr6 = 0; registers.Dr7 = 0x90001;
                            if (SetThreadContext(process.hThread, &registers)) {
                                watchedHunk = watchSymbol ? symbol->Address : user;
                                DWORD64 header[8]{};
                                ReadProcessMemory(process.hProcess, reinterpret_cast<void*>(watchedHunk), header, sizeof(header), &bytes);
                                std::printf("Watching native address %llx (%s) value=%llx\n", watchedHunk, watchSymbol ? watchSymbol : "debug hunk", header[0]);
                            }
                        }
                        ResumeThread(process.hThread);
                    }
                }
            }
#endif
            continue;
        }
        DWORD continuation = DBG_CONTINUE;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            SymLoadModuleExW(process.hProcess, event.u.CreateProcessInfo.hFile, argv[1], nullptr,
                reinterpret_cast<DWORD64>(event.u.CreateProcessInfo.lpBaseOfImage), 0, nullptr, 0);
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
        }
        if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile) {
            SymLoadModuleEx(process.hProcess, event.u.LoadDll.hFile, nullptr, nullptr,
                reinterpret_cast<DWORD64>(event.u.LoadDll.lpBaseOfDll), 0, nullptr, 0);
        }
        if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const auto& exception = event.u.Exception;
            const auto code = exception.ExceptionRecord.ExceptionCode;
            if (code != EXCEPTION_BREAKPOINT && code != EXCEPTION_SINGLE_STEP) continuation = DBG_EXCEPTION_NOT_HANDLED;
            const bool engineBreakpoint = code == EXCEPTION_BREAKPOINT && sawInitialBreakpoint;
#if defined(_WIN64)
            const bool hunkWrite = watchedHunk && code == EXCEPTION_SINGLE_STEP;
#else
            const bool hunkWrite = false;
#endif
            if (code == EXCEPTION_BREAKPOINT) sawInitialBreakpoint = true;
            if (!exception.dwFirstChance || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ACCESS_VIOLATION || engineBreakpoint || hunkWrite) {
                if (!exception.dwFirstChance || code == EXCEPTION_STACK_OVERFLOW || engineBreakpoint || hunkWrite) fault = true;
                SymRefreshModuleList(process.hProcess);
                alignas(SYMBOL_INFO) char errorSymbolData[sizeof(SYMBOL_INFO) + 256]{};
                auto* errorSymbol = reinterpret_cast<SYMBOL_INFO*>(errorSymbolData);
                errorSymbol->SizeOfStruct = sizeof(SYMBOL_INFO); errorSymbol->MaxNameLen = 255;
                if (SymFromName(process.hProcess, "com_errorMessage", errorSymbol)) {
                    char message[4096]{}; SIZE_T messageBytes = 0;
                    if (ReadProcessMemory(process.hProcess, reinterpret_cast<void*>(errorSymbol->Address), message, sizeof(message) - 1, &messageBytes) && message[0])
                        std::printf("Engine error: %s\n", message);
                }
                std::printf("Exception %08lx firstChance=%lu at %p\n", code, exception.dwFirstChance, exception.ExceptionRecord.ExceptionAddress);
                if (code == EXCEPTION_ACCESS_VIOLATION && exception.ExceptionRecord.NumberParameters >= 2)
                    std::printf("  access=%llu address=%llx\n", static_cast<unsigned long long>(exception.ExceptionRecord.ExceptionInformation[0]), static_cast<unsigned long long>(exception.ExceptionRecord.ExceptionInformation[1]));
                HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, event.dwThreadId);
                khz::DebugContext context;
                if (thread && context.capture(process.hProcess, thread)) {
#if defined(_WIN64)
                    if (context.machine == IMAGE_FILE_MACHINE_AMD64) {
                        const auto& regs = context.native;
                        if (watchedHunk) {
                            DWORD64 header[8]{}; SIZE_T headerBytes = 0;
                            ReadProcessMemory(process.hProcess, reinterpret_cast<void*>(watchedHunk), header, sizeof(header), &headerBytes);
                            std::printf("  debug hunk current=%llx next=%llx max=%llx end=%llx pos=%llx name=%llx\n", header[0], header[1], header[2], header[3], header[4], header[5]);
                        }
                        std::printf("  rip=%llx rsp=%llx rbp=%llx rax=%llx rcx=%llx rdx=%llx r8=%llx r9=%llx\n",
                            regs.Rip, regs.Rsp, regs.Rbp, regs.Rax, regs.Rcx, regs.Rdx, regs.R8, regs.R9);
                        DWORD64 stack[96]{}; SIZE_T stackBytes = 0;
                        if (ReadProcessMemory(process.hProcess, reinterpret_cast<void*>(regs.Rsp), stack, sizeof(stack), &stackBytes)) {
                            for (unsigned n = 0; n < stackBytes / sizeof(stack[0]); ++n) {
                                alignas(SYMBOL_INFO) char data[sizeof(SYMBOL_INFO) + 1024]{};
                                auto* entry = reinterpret_cast<SYMBOL_INFO*>(data); entry->SizeOfStruct = sizeof(SYMBOL_INFO); entry->MaxNameLen = 1023;
                                DWORD64 delta = 0;
                                if (SymFromAddr(process.hProcess, stack[n], &delta, entry))
                                    std::printf("  stack[%u]=%llx %s + %llu\n", n, stack[n], entry->Name, delta);
                            }
                        }
                    }
#endif
                    auto& frame = context.frame;
                    for (unsigned i = 0; i < 48 && frame.AddrPC.Offset; ++i) {
                        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 1024]{};
                        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage); symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 1023;
                        DWORD64 displacement = 0; IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line); DWORD lineOffset = 0;
                        if (SymFromAddr(process.hProcess, frame.AddrPC.Offset, &displacement, symbol)) {
                            std::printf("  %s + %llu", symbol->Name, displacement);
                            if (SymGetLineFromAddr64(process.hProcess, frame.AddrPC.Offset, &lineOffset, &line)) std::printf(" (%s:%lu)", line.FileName, line.LineNumber);
                            std::putchar('\n');
                        } else std::printf("  %llx\n", frame.AddrPC.Offset);
                        if (!StackWalk64(context.machine, process.hProcess, thread, &frame, context.registers(), nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
                    }
                }
                if (thread) CloseHandle(thread);
                if (fault) TerminateProcess(process.hProcess, code);
            }
        } else if(event.dwDebugEventCode==OUTPUT_DEBUG_STRING_EVENT) {
            const auto& message=event.u.DebugString;
            if(!message.fUnicode&&message.nDebugStringLength<8192) {
                char text[8192]{};SIZE_T bytes=0;
                if(ReadProcessMemory(process.hProcess,message.lpDebugStringData,text,message.nDebugStringLength,&bytes))std::printf("Debug: %.*s\n",int(bytes),text);
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            std::printf("Exit=%08lx\n", event.u.ExitProcess.dwExitCode); exited = true;
        } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation);
    }
    if (!exited) { std::printf("Startup probe reached %u seconds without a fatal exception.\n",durationSeconds); TerminateProcess(process.hProcess, 0); }
    // Debugger exit notifications must be continued before a terminating
    // debuggee can finish. A plain process wait leaves it locked on Windows.
    const auto cleanupDeadline=GetTickCount64()+10000;
    while(!exited&&GetTickCount64()<cleanupDeadline) {
        DEBUG_EVENT end{};if(!WaitForDebugEvent(&end,100))continue;
        if(end.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT)exited=true;
        if(end.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT&&end.u.LoadDll.hFile)CloseHandle(end.u.LoadDll.hFile);
        ContinueDebugEvent(end.dwProcessId,end.dwThreadId,DBG_CONTINUE);
    }
    if(WaitForSingleObject(process.hProcess,1000)!=WAIT_OBJECT_0){std::puts("Owned startup process did not exit within cleanup deadline.");fault=true;}
    SymCleanup(process.hProcess); CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return fault ? 1 : 0;
}
