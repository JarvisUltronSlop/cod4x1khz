#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

// A developer-only debugger for failures before the engine installs its handler.
// It owns and terminates only the process it creates.
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if (argc != 4&&argc!=5) { std::puts("khz_launch_probe <executable> <cwd> <symbol directory> [extra arguments]"); return 2; }
    wchar_t command[4096];
    if (std::swprintf(command, 4096, L"\"%ls\" allowdupe +set r_fullscreen 0 +set net_port 28994 %ls", argv[1],argc==5?argv[4]:L"") < 0) return 2;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(argv[1], command, nullptr, nullptr, FALSE, DEBUG_ONLY_THIS_PROCESS,
        nullptr, argv[2], &startup, &process)) { std::printf("CreateProcess error=%lu\n", GetLastError()); return 2; }
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitializeW(process.hProcess, argv[3], TRUE);
    char duration[16]{};unsigned durationSeconds=15;
    if(GetEnvironmentVariableA("KHZ_PROBE_SECONDS",duration,sizeof(duration))) {
        const auto requested=std::strtoul(duration,nullptr,10);if(requested>=1&&requested<=60)durationSeconds=unsigned(requested);
    }
    std::printf("Owned client PID=%lu\n",process.dwProcessId);std::fflush(stdout);
    const auto until = GetTickCount64() + durationSeconds*1000;
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
        } else if(event.dwDebugEventCode==OUTPUT_DEBUG_STRING_EVENT) {
            const auto& message=event.u.DebugString;
            if(!message.fUnicode&&message.nDebugStringLength<8192) {
                char text[8192]{};SIZE_T bytes=0;
                if(ReadProcessMemory(process.hProcess,message.lpDebugStringData,text,message.nDebugStringLength,&bytes))std::printf("Debug: %.*s\n",int(bytes),text);
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
