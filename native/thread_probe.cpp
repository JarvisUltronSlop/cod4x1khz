#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <DbgHelp.h>
#include <Psapi.h>
#include <cstdio>
#include <cwchar>

// Inspect only the explicitly named owned fixture. Each thread is resumed
// before moving to the next; no process-wide debugger or persistent hook stays.
int wmain(int argc,wchar_t** argv){
    if(argc!=4)return 2;const DWORD pid=std::wcstoul(argv[1],nullptr,10);
    HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);
    if(!process)return 2;wchar_t image[32768];DWORD chars=32768;
    if(!QueryFullProcessImageNameW(process,0,image,&chars)||_wcsicmp(image,argv[2])){CloseHandle(process);return 2;}
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
    if(GetProcessMemoryInfo(process,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)))std::printf("Working set=%llu private=%llu\n",static_cast<unsigned long long>(memory.WorkingSetSize),static_cast<unsigned long long>(memory.PrivateUsage));
    SymSetOptions(SYMOPT_LOAD_LINES|SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS);SymInitializeW(process,argv[3],TRUE);
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);THREADENTRY32 entry{};entry.dwSize=sizeof(entry);
    if(snapshot!=INVALID_HANDLE_VALUE&&Thread32First(snapshot,&entry))do{
        if(entry.th32OwnerProcessID!=pid)continue;
        HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SUSPEND_RESUME|THREAD_QUERY_INFORMATION,FALSE,entry.th32ThreadID);if(!thread)continue;
        if(SuspendThread(thread)==DWORD(-1)){CloseHandle(thread);continue;}
        std::printf("Thread %lu\n",entry.th32ThreadID);CONTEXT context{};context.ContextFlags=CONTEXT_FULL;
        if(GetThreadContext(thread,&context)){
            STACKFRAME64 frame{};frame.AddrPC={context.Eip,0,AddrModeFlat};frame.AddrFrame={context.Ebp,0,AddrModeFlat};frame.AddrStack={context.Esp,0,AddrModeFlat};
            for(unsigned n=0;n<32&&frame.AddrPC.Offset;++n){
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO)+1024]{};auto* symbol=reinterpret_cast<SYMBOL_INFO*>(buffer);symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=1023;
                DWORD64 offset=0;IMAGEHLP_LINE64 line{};line.SizeOfStruct=sizeof(line);DWORD displacement=0;
                if(SymFromAddr(process,frame.AddrPC.Offset,&offset,symbol)){
                    std::printf("  %s + %llu",symbol->Name,offset);
                    if(SymGetLineFromAddr64(process,frame.AddrPC.Offset,&displacement,&line))std::printf(" (%s:%lu)",line.FileName,line.LineNumber);std::puts("");
                }else std::printf("  %llx\n",frame.AddrPC.Offset);
                if(!StackWalk64(IMAGE_FILE_MACHINE_I386,process,thread,&frame,&context,nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr))break;
            }
        }
        ResumeThread(thread);CloseHandle(thread);
    }while(Thread32Next(snapshot,&entry));
    if(snapshot!=INVALID_HANDLE_VALUE)CloseHandle(snapshot);SymCleanup(process);CloseHandle(process);return 0;
}
