#include "khz_plugin_tcp.h"
#include <plugin_tcp_policy.hpp>
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <new>
#include <thread>
#pragma comment(lib,"ws2_32.lib")

namespace khz::plugin_tcp {
namespace {
enum class Phase:unsigned {Closed,Connecting,Connected,Retiring};
struct Resolver {
    OVERLAPPED overlapped{};
    HANDLE cancellation=nullptr;
    PADDRINFOEXW addresses=nullptr;
    wchar_t host[256]{},service[6]{};
    ADDRINFOEXW hints{};
    timeval timeout{};
    bool pending=false,completed=false,cancelled=false;
    int result=WSA_IO_PENDING;
};
struct Slot {
    std::atomic_flag guard=ATOMIC_FLAG_INIT;
    std::atomic<Phase> phase{Phase::Closed};
    std::atomic<std::uint64_t> generation{0},operation{0},cancelThrough{0};
    std::atomic<bool> callerActive{false};
    SOCKET socket=INVALID_SOCKET;
    Resolver* resolver=nullptr;
};
struct State {
    std::array<Slot,MaxOwners*ConnectionsPerOwner> slots;
    std::array<std::atomic<std::uint64_t>,MaxOwners> retired{};
    std::atomic<bool> stopping{false},done{false};
    std::atomic<std::uint64_t> submitted{0},completed{0},failed{0},cancelled{0},rejected{0},bytesSent{0},bytesReceived{0};
    HANDLE wake=nullptr;
    std::thread retireThread;
};
std::atomic<State*> current{nullptr};
std::mutex lifecycle;
#ifdef KHZ_PLUGIN_TCP_TESTS
std::atomic<unsigned> testResolutionDelay{0};
#endif
struct Guard {
    Slot& slot;bool held;
    explicit Guard(Slot& value):slot(value),held(!slot.guard.test_and_set(std::memory_order_acquire)){}
    ~Guard(){if(held)slot.guard.clear(std::memory_order_release);}
};
void maximum(std::atomic<std::uint64_t>& target,std::uint64_t value){
    auto previous=target.load(std::memory_order_acquire);
    while(previous<value&&!target.compare_exchange_weak(previous,value,std::memory_order_acq_rel)){}
}
bool identity(unsigned owner,std::uint64_t generation,int connection){
    return owner<MaxOwners&&generation&&connection>=0&&connection<int(ConnectionsPerOwner);
}
Slot& slotFor(State& state,unsigned owner,int connection){return state.slots[owner*ConnectionsPerOwner+unsigned(connection)];}
bool cancelled(State& state,Slot& slot,unsigned owner,std::uint64_t generation,std::uint64_t operation){
    return state.stopping.load(std::memory_order_acquire)||state.retired[owner].load(std::memory_order_acquire)>=generation
        ||slot.cancelThrough.load(std::memory_order_acquire)>=operation;
}
void cancelResolver(Resolver& resolver){
    if(resolver.pending&&!resolver.completed&&!resolver.cancelled){
        resolver.cancelled=true;if(resolver.cancellation)GetAddrInfoExCancel(&resolver.cancellation);
    }
}
bool resolverComplete(Resolver& resolver){
    if(resolver.completed)return true;
    if(!resolver.pending||WaitForSingleObject(resolver.overlapped.hEvent,0)==WAIT_OBJECT_0){
        resolver.result=resolver.pending?GetAddrInfoExOverlappedResult(&resolver.overlapped):resolver.result;
        resolver.completed=true;return true;
    }
    return false;
}
void freeResolver(Resolver* resolver){
    if(!resolver)return;
    if(resolver->addresses)FreeAddrInfoExW(resolver->addresses);
    if(resolver->overlapped.hEvent)CloseHandle(resolver->overlapped.hEvent);
    delete resolver;
}
void retire(State* state){
    for(;;){
        bool empty=true;
        for(unsigned index=0;index<state->slots.size();++index){
            auto& slot=state->slots[index];Guard guard(slot);if(!guard.held){empty=false;continue;}
            const auto generation=slot.generation.load(std::memory_order_acquire),operation=slot.operation.load(std::memory_order_acquire);
            if(slot.phase.load()==Phase::Closed)continue;
            empty=false;
            if(cancelled(*state,slot,index/ConnectionsPerOwner,generation,operation))slot.phase=Phase::Retiring;
            if(slot.phase.load()!=Phase::Retiring||slot.callerActive.load(std::memory_order_acquire))continue;
            if(slot.resolver){cancelResolver(*slot.resolver);if(!resolverComplete(*slot.resolver))continue;freeResolver(slot.resolver);slot.resolver=nullptr;}
            if(slot.socket!=INVALID_SOCKET){closesocket(slot.socket);slot.socket=INVALID_SOCKET;}
            slot.phase.store(Phase::Closed,std::memory_order_release);
        }
        if(state->stopping.load(std::memory_order_acquire)&&empty)break;
        WaitForSingleObject(state->wake,10);
    }
    state->done.store(true,std::memory_order_release);
}
void dispose(State* state){
    if(state->retireThread.joinable())state->retireThread.join();
    CloseHandle(state->wake);delete state;WSACleanup();
}
void requestRetirement(State& state,Slot& slot,std::uint64_t operation){
    maximum(slot.cancelThrough,operation);SetEvent(state.wake);
}
SOCKET connectAddress(State& state,Slot& slot,unsigned owner,std::uint64_t generation,std::uint64_t operation,
    const ADDRINFOEXW& address,ULONGLONG deadline){
    if((address.ai_family!=AF_INET&&address.ai_family!=AF_INET6)||!address.ai_addr)return INVALID_SOCKET;
    SOCKET socket=::socket(address.ai_family,SOCK_STREAM,IPPROTO_TCP);if(socket==INVALID_SOCKET)return INVALID_SOCKET;
    u_long nonblocking=1;
    if(ioctlsocket(socket,FIONBIO,&nonblocking)){closesocket(socket);return INVALID_SOCKET;}
    bool connected=::connect(socket,address.ai_addr,int(address.ai_addrlen))==0;
    if(!connected){const int error=WSAGetLastError();if(error!=WSAEWOULDBLOCK&&error!=WSAEINPROGRESS){closesocket(socket);return INVALID_SOCKET;}}
    while(!connected&&GetTickCount64()<deadline&&!cancelled(state,slot,owner,generation,operation)){
        fd_set writable,errors;FD_ZERO(&writable);FD_ZERO(&errors);FD_SET(socket,&writable);FD_SET(socket,&errors);
        timeval timeout{};timeout.tv_usec=5000;
        const int ready=select(0,nullptr,&writable,&errors,&timeout);
        if(ready==SOCKET_ERROR)break;
        if(ready){int error=0;int length=sizeof(error);if(getsockopt(socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&length)||error)break;connected=FD_ISSET(socket,&writable)!=0;}
    }
    if(!connected||GetTickCount64()>=deadline||cancelled(state,slot,owner,generation,operation)){closesocket(socket);return INVALID_SOCKET;}
    const BOOL noDelay=TRUE;setsockopt(socket,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&noDelay),sizeof(noDelay));
    return socket;
}
}

bool Init(){
    std::lock_guard<std::mutex> lock(lifecycle);
    if(auto* state=current.load(std::memory_order_acquire)){
        if(!state->stopping.load())return true;
        if(!state->done.load())return false;
        current=nullptr;dispose(state);
    }
    WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))return false;
    auto* state=new(std::nothrow)State;
    if(!state){WSACleanup();return false;}
    state->wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!state->wake){delete state;WSACleanup();return false;}
    try{state->retireThread=std::thread(retire,state);}catch(...){CloseHandle(state->wake);delete state;WSACleanup();return false;}
    current.store(state,std::memory_order_release);return true;
}
bool Connect(unsigned owner,std::uint64_t generation,int connection,const char* remote){
    auto* state=current.load(std::memory_order_acquire);policy::Endpoint endpoint;
    if(!state||state->stopping.load())return false;
    if(!identity(owner,generation,connection)||!remote||strnlen(remote,256)>255
        ||!policy::Parse(remote,&endpoint)||state->retired[owner].load()>=generation){++state->rejected;return false;}
    auto& slot=slotFor(*state,owner,connection);std::uint64_t operation;
    {
        Guard guard(slot);if(!guard.held||slot.phase.load()!=Phase::Closed){++state->rejected;return false;}
        operation=slot.operation.load()+1;if(!operation){++state->rejected;return false;}
        slot.generation=generation;slot.operation=operation;slot.callerActive=true;slot.phase=Phase::Connecting;
    }
    ++state->submitted;const auto deadline=GetTickCount64()+DeadlineMilliseconds;
    auto* resolver=new(std::nothrow)Resolver;SOCKET connected=INVALID_SOCKET;
    if(resolver){
        resolver->overlapped.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        for(unsigned index=0;endpoint.host[index];++index)resolver->host[index]=static_cast<unsigned char>(endpoint.host[index]);
        _snwprintf_s(resolver->service,_TRUNCATE,L"%u",unsigned(endpoint.port));
        resolver->hints.ai_family=AF_UNSPEC;resolver->hints.ai_socktype=SOCK_STREAM;resolver->hints.ai_protocol=IPPROTO_TCP;
        resolver->hints.ai_flags=AI_NUMERICSERV;resolver->timeout.tv_sec=DeadlineMilliseconds/1000;
        if(resolver->overlapped.hEvent){
            resolver->result=GetAddrInfoExW(resolver->host,resolver->service,NS_ALL,nullptr,&resolver->hints,
                &resolver->addresses,&resolver->timeout,&resolver->overlapped,nullptr,&resolver->cancellation);
            resolver->pending=resolver->result==WSA_IO_PENDING;resolver->completed=!resolver->pending;
#ifdef KHZ_PLUGIN_TCP_TESTS
            const auto delayedUntil=GetTickCount64()+testResolutionDelay.load();
            while(GetTickCount64()<delayedUntil&&GetTickCount64()<deadline&&!cancelled(*state,slot,owner,generation,operation))Sleep(1);
#endif
            while(!resolverComplete(*resolver)&&GetTickCount64()<deadline&&!cancelled(*state,slot,owner,generation,operation))
                WaitForSingleObject(resolver->overlapped.hEvent,5);
            if(resolverComplete(*resolver)&&resolver->result==0&&!cancelled(*state,slot,owner,generation,operation)){
                unsigned attempts=0;
                for(auto* address=resolver->addresses;address&&attempts++<16&&GetTickCount64()<deadline;address=address->ai_next){
                    connected=connectAddress(*state,slot,owner,generation,operation,*address,deadline);
                    if(connected!=INVALID_SOCKET)break;
                }
            }
            if(!resolver->completed)cancelResolver(*resolver);
        }else{resolver->completed=true;resolver->result=WSA_NOT_ENOUGH_MEMORY;}
    }
    // Caller owns this slot until publication; retirement only sets atomic
    // cancellation meanwhile. Never expose an in-flight DNS/socket handle.
    while(slot.guard.test_and_set(std::memory_order_acquire))std::this_thread::yield();
    const bool success=connected!=INVALID_SOCKET&&!cancelled(*state,slot,owner,generation,operation);
    if(success){slot.socket=connected;connected=INVALID_SOCKET;slot.phase=Phase::Connected;++state->completed;}
    else{slot.phase=Phase::Retiring;++state->failed;}
    if(connected!=INVALID_SOCKET)closesocket(connected);
    if(resolver&&resolver->completed){freeResolver(resolver);resolver=nullptr;}
    slot.resolver=resolver;
    slot.callerActive.store(false,std::memory_order_release);slot.guard.clear(std::memory_order_release);
    SetEvent(state->wake);return success;
}
int Receive(unsigned owner,std::uint64_t generation,int connection,void* output,int capacity){
    auto* state=current.load(std::memory_order_acquire);
    if(!state||state->stopping.load()||!identity(owner,generation,connection)||!output||capacity<=0)return -1;
    auto& slot=slotFor(*state,owner,connection);Guard guard(slot);if(!guard.held)return 0;
    const auto operation=slot.operation.load();
    if(slot.generation.load()!=generation||slot.phase.load()!=Phase::Connected||cancelled(*state,slot,owner,generation,operation))return -1;
    const int received=recv(slot.socket,static_cast<char*>(output),(std::min)(capacity,int(MaxCallBytes)),0);
    if(received>0){state->bytesReceived.fetch_add(unsigned(received));return received;}
    if(received==SOCKET_ERROR&&WSAGetLastError()==WSAEWOULDBLOCK)return 0;
    requestRetirement(*state,slot,operation);return -1;
}
int Send(unsigned owner,std::uint64_t generation,int connection,const void* input,int length){
    auto* state=current.load(std::memory_order_acquire);
    if(!state||state->stopping.load()||!identity(owner,generation,connection)||!input||length<=0)return -1;
    auto& slot=slotFor(*state,owner,connection);Guard guard(slot);if(!guard.held)return 0;
    const auto operation=slot.operation.load();
    if(slot.generation.load()!=generation||slot.phase.load()!=Phase::Connected||cancelled(*state,slot,owner,generation,operation))return -1;
    const int sent=send(slot.socket,static_cast<const char*>(input),(std::min)(length,int(MaxCallBytes)),0);
    if(sent>0){state->bytesSent.fetch_add(unsigned(sent));return sent;}
    if(sent==SOCKET_ERROR&&WSAGetLastError()==WSAEWOULDBLOCK)return 0;
    requestRetirement(*state,slot,operation);return -1;
}
void Close(unsigned owner,std::uint64_t generation,int connection){
    auto* state=current.load(std::memory_order_acquire);if(!state||!identity(owner,generation,connection))return;
    auto& slot=slotFor(*state,owner,connection);
    const auto operation=slot.operation.load(std::memory_order_acquire);
    if(slot.generation.load(std::memory_order_acquire)==generation&&slot.operation.load(std::memory_order_acquire)==operation)
        requestRetirement(*state,slot,operation);
}
void Cancel(unsigned owner,std::uint64_t generation){
    auto* state=current.load(std::memory_order_acquire);if(!state||owner>=MaxOwners||!generation)return;
    maximum(state->retired[owner],generation);++state->cancelled;SetEvent(state->wake);
}
void Discard(unsigned owner,std::uint64_t generation){Cancel(owner,generation);}
void Shutdown(){
    std::lock_guard<std::mutex> lock(lifecycle);auto* state=current.load(std::memory_order_acquire);if(!state)return;
    state->stopping.store(true,std::memory_order_release);SetEvent(state->wake);
    // A provider that has not yet acknowledged async DNS cancellation retains
    // its bounded state and handle; never free an active OVERLAPPED structure.
    const auto deadline=GetTickCount64()+6000;
    while(!state->done.load(std::memory_order_acquire)&&GetTickCount64()<deadline)Sleep(1);
    if(!state->done.load())return;
    current=nullptr;dispose(state);
}
Statistics Stats(){
    Statistics output;auto* state=current.load(std::memory_order_acquire);if(!state)return output;
    for(const auto& slot:state->slots)switch(slot.phase.load(std::memory_order_acquire)){
        case Phase::Connected:++output.connected;break;case Phase::Connecting:++output.pending;break;case Phase::Retiring:++output.retiring;break;default:break;
    }
    output.submitted=state->submitted.load();output.completed=state->completed.load();output.failed=state->failed.load();
    output.cancelled=state->cancelled.load();output.rejected=state->rejected.load();output.bytesSent=state->bytesSent.load();output.bytesReceived=state->bytesReceived.load();output.stopping=state->stopping.load();return output;
}
#ifdef KHZ_PLUGIN_TCP_TESTS
void TestDelayResolution(unsigned milliseconds){testResolutionDelay.store(milliseconds);}
#endif
}
