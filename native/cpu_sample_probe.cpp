#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <DbgHelp.h>
#include "debug_context.hpp"
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <vector>

// Diagnostic wall-time sampling of an exact owned benchmark process. Threads
// resume before symbol formatting. The samples include waits and scheduling;
// thread CPU deltas distinguish those from actual processor work.
struct Thread {
    DWORD id;
    HANDLE handle;
    unsigned failures=0,samples=0;
    std::uint64_t initialCpu=0,finalCpu=0;
    std::map<std::vector<DWORD64>,unsigned> stacks;
};
std::uint64_t cpuTime(HANDLE thread) {
    FILETIME creation{},exit{},kernel{},user{};
    if(!GetThreadTimes(thread,&creation,&exit,&kernel,&user))return 0;
    return (std::uint64_t(kernel.dwHighDateTime)<<32|kernel.dwLowDateTime)+
        (std::uint64_t(user.dwHighDateTime)<<32|user.dwLowDateTime);
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=5)return 2;
    const DWORD pid=std::wcstoul(argv[1],nullptr,10);
    const unsigned seconds=std::wcstoul(argv[4],nullptr,10);
    if(!pid||seconds<1||seconds>30)return 2;
    HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|SYNCHRONIZE,FALSE,pid);
    if(!process)return 2;
    wchar_t path[32768];DWORD chars=32768;
    if(!QueryFullProcessImageNameW(process,0,path,&chars)||_wcsicmp(path,argv[2])){CloseHandle(process);return 2;}
    SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_LOAD_LINES);
    if(!SymInitializeW(process,argv[3],TRUE)){CloseHandle(process);return 2;}
    std::vector<Thread> threads;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    THREADENTRY32 entry{};entry.dwSize=sizeof(entry);
    if(snapshot!=INVALID_HANDLE_VALUE&&Thread32First(snapshot,&entry))do {
        if(entry.th32OwnerProcessID!=pid)continue;
        HANDLE handle=OpenThread(THREAD_GET_CONTEXT|THREAD_SUSPEND_RESUME|THREAD_QUERY_INFORMATION,FALSE,entry.th32ThreadID);
        if(handle)threads.push_back({entry.th32ThreadID,handle,0,0,cpuTime(handle)});
    }while(Thread32Next(snapshot,&entry));
    if(snapshot!=INVALID_HANDLE_VALUE)CloseHandle(snapshot);
    LARGE_INTEGER frequency{},started{},now{};
    QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&started);
    const auto deadline=started.QuadPart+frequency.QuadPart*seconds;
    bool exited=false;
    do {
        for(auto& thread:threads) {
            std::vector<DWORD64> stack;stack.reserve(32);
            if(SuspendThread(thread.handle)==DWORD(-1)){++thread.failures;continue;}
            khz::DebugContext context;
            if(context.capture(process,thread.handle)) {
                auto& frame=context.frame;
                for(unsigned n=0;n<32&&frame.AddrPC.Offset;++n) {
                    stack.push_back(frame.AddrPC.Offset);
                    if(!StackWalk64(context.machine,process,thread.handle,&frame,context.registers(),nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr))break;
                }
            }
            // Always resume, including failed context capture/unwinding.
            ResumeThread(thread.handle);
            if(stack.empty())++thread.failures;
            else {++thread.samples;++thread.stacks[stack];}
        }
        exited=WaitForSingleObject(process,10)==WAIT_OBJECT_0;
        QueryPerformanceCounter(&now);
    }while(!exited&&now.QuadPart<deadline);
    std::printf("process=%lu wall_seconds=%.6f exited=%u scope=wall_time_sampling_with_temporary_thread_suspension\n",pid,
        double(now.QuadPart-started.QuadPart)/frequency.QuadPart,unsigned(exited));
    for(auto& thread:threads) {
        thread.finalCpu=cpuTime(thread.handle);
        std::printf("thread=%lu samples=%u failures=%u cpu_ms=%.3f\n",thread.id,thread.samples,thread.failures,
            thread.finalCpu>=thread.initialCpu?double(thread.finalCpu-thread.initialCpu)*.0001:0.);
        for(const auto& [stack,count]:thread.stacks) {
            std::printf(" count=%u",count);
            for(const auto address:stack) {
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO)+1024]{};
                auto* symbol=reinterpret_cast<SYMBOL_INFO*>(buffer);symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=1023;
                DWORD64 displacement=0;IMAGEHLP_LINE64 line{};line.SizeOfStruct=sizeof(line);DWORD offset=0;
                if(SymFromAddr(process,address,&displacement,symbol)) {
                    std::printf(" | %s+%llu",symbol->Name,displacement);
                    if(SymGetLineFromAddr64(process,address,&offset,&line))std::printf("@%s:%lu",line.FileName,line.LineNumber);
                }else std::printf(" | 0x%llx",address);
            }
            std::puts("");
        }
        CloseHandle(thread.handle);
    }
    SymCleanup(process);CloseHandle(process);return exited?1:0;
}
