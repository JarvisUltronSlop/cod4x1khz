#include "khz_plugin_http.h"
#define KHZ_PLUGIN_HOST
#include "../../../sdk/include/pinc.h"
#include "../../../sdk/include/public_http_api.h"
#include <plugin_io_policy.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib,"winhttp.lib")

namespace khz::plugin_http {
namespace {
constexpr unsigned Workers=4,OwnerCount=16,HeaderLimit=16384;
constexpr std::uint64_t DeadlineMs=5000;
enum class Phase:unsigned { Free,Queued,Running,Ready,Published };
struct Slot {
    std::atomic<Phase> phase{Phase::Free};
    std::atomic<unsigned> owner{0};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<bool> cancelled{false},released{false};
    std::atomic_flag guard=ATOMIC_FLAG_INIT;
    ftRequest_t publicView{};
    std::wstring host,path,method,headers;
    std::vector<unsigned char> payload,response;
    std::uint64_t deadline=0;
    unsigned charge=0,headerBytes=0,bodyBytes=0,statusCode=0;
    INTERNET_PORT port=0;
    bool secure=false,success=false;
    char status[32]{},contentType[64]{};
};
struct State {
    std::mutex allocation;
    std::array<Slot,MaxRequests> slots;
    std::array<std::atomic<std::uint64_t>,OwnerCount> retired{};
    std::array<std::atomic<std::uint64_t>,OwnerCount> discarded{};
    std::array<std::thread,Workers> workers;
    HANDLE wake=nullptr;
    std::atomic<bool> stopping{false};
    std::atomic<unsigned> active{0},buffered{0};
    std::atomic<std::uint64_t> submitted{0},completed{0},cancelled{0},rejected{0};
};
// Lifecycle is owned by the host, before plugin callbacks and after pinned jobs
// have exited. Neither request workers nor Poll consult the plugin registry.
std::atomic<State*> current{nullptr};
std::mutex lifecycle;

struct Guard {
    Slot& slot; bool held;
    explicit Guard(Slot& value):slot(value),held(!slot.guard.test_and_set(std::memory_order_acquire)){}
    ~Guard(){if(held)slot.guard.clear(std::memory_order_release);}
};
bool text(const char* value,unsigned maximum,std::string_view* output){
    if(!value)return false;
    const auto length=strnlen(value,maximum+1);
    if(length>maximum)return false;
    *output=std::string_view(value,length);return true;
}
std::wstring wide(std::string_view value){
    if(value.empty())return {};
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);
    if(count<1)return {};
    std::wstring result(count,L'\0');
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),result.data(),count)!=count)return {};
    return result;
}
bool url(std::string_view input,std::wstring* host,std::wstring* path,INTERNET_PORT* port,bool* secure){
    if(input.empty())return false;
    for(unsigned char c:input)if(c<=32||c>=127||c=='\\'||c=='#')return false;
    if(input.substr(0,7)!="http://"&&input.substr(0,8)!="https://")return false;
    auto native=wide(input);URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);
    parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=parts.dwUserNameLength=parts.dwPasswordLength=DWORD(-1);
    if(!WinHttpCrackUrl(native.c_str(),DWORD(native.size()),0,&parts)||!parts.dwHostNameLength||parts.dwUserNameLength||parts.dwPasswordLength)return false;
    if(parts.nScheme!=INTERNET_SCHEME_HTTP&&parts.nScheme!=INTERNET_SCHEME_HTTPS)return false;
    *host=std::wstring(parts.lpszHostName,parts.dwHostNameLength);
    *path=parts.dwUrlPathLength?std::wstring(parts.lpszUrlPath,parts.dwUrlPathLength):L"/";
    if(parts.dwExtraInfoLength)path->append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
    *port=parts.nPort;*secure=parts.nScheme==INTERNET_SCHEME_HTTPS;
    return *port&&path->size()<=1023;
}
bool retired(const State& state,const Slot& slot){return slot.generation.load(std::memory_order_relaxed)<=state.retired[slot.owner.load(std::memory_order_relaxed)].load(std::memory_order_acquire);}
bool stopped(const State& state,const Slot& slot){return state.stopping.load(std::memory_order_acquire)||slot.cancelled.load(std::memory_order_acquire)||retired(state,slot)||GetTickCount64()>=slot.deadline;}

struct Async {
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    std::atomic<unsigned> notices{0};
    std::atomic<DWORD> available{0},readBytes{0},error{0};
};
constexpr unsigned Sent=1,Headers=2,Available=4,Read=8,Error=16,Closed=32;
void CALLBACK callback(HINTERNET,DWORD_PTR pointer,DWORD notice,void* information,DWORD length){
    auto* context=reinterpret_cast<Async*>(pointer);if(!context)return;
    unsigned bit=0;
    switch(notice){
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:bit=Sent;break;
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:bit=Headers;break;
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
        if(information&&length>=sizeof(DWORD))context->available.store(*static_cast<DWORD*>(information),std::memory_order_relaxed);bit=Available;break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:context->readBytes.store(length,std::memory_order_relaxed);bit=Read;break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        if(information&&length>=sizeof(WINHTTP_ASYNC_RESULT))context->error.store(static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError,std::memory_order_relaxed);bit=Error;break;
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:bit=Closed;break;
    default:return;
    }
    if(bit==Closed){SetEvent(context->event);context->notices.fetch_or(bit,std::memory_order_release);return;}
    context->notices.fetch_or(bit,std::memory_order_release);SetEvent(context->event);
}
bool wait(State& state,Slot& slot,Async& context,unsigned notice){
    for(;;){
        const auto value=context.notices.load(std::memory_order_acquire);
        if(value&(Error|Closed))return false;
        if(value&notice){context.notices.fetch_and(~notice,std::memory_order_acq_rel);return !stopped(state,slot);}
        if(stopped(state,slot))return false;
        const auto now=GetTickCount64();if(now>=slot.deadline)return false;const auto remaining=slot.deadline-now;
        WaitForSingleObject(context.event,DWORD(std::min<std::uint64_t>(remaining,25)));
    }
}
bool asyncResult(BOOL result){return result||GetLastError()==ERROR_IO_PENDING;}
struct Handles {
    HINTERNET session=nullptr,connection=nullptr,request=nullptr;
    Async context;
    bool callbackInstalled=false;
    ~Handles(){
        if(request){
            WinHttpCloseHandle(request);
            // HANDLE_CLOSING is the final callback. Payload/read buffers stay
            // owned by the slot until it arrives, including cancellation.
            if(callbackInstalled)while(!(context.notices.load(std::memory_order_acquire)&Closed))WaitForSingleObject(context.event,25);
        }
        if(connection)WinHttpCloseHandle(connection);
        if(session)WinHttpCloseHandle(session);
        if(context.event)CloseHandle(context.event);
    }
};
bool responseHeaders(HINTERNET request,Slot& slot){
    DWORD size=sizeof(DWORD),status=0;
    if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)||status<100||status>599)return false;
    slot.statusCode=status;
    size=0;WinHttpQueryHeaders(request,WINHTTP_QUERY_RAW_HEADERS_CRLF,WINHTTP_HEADER_NAME_BY_INDEX,nullptr,&size,WINHTTP_NO_HEADER_INDEX);
    if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER||size<2||size>HeaderLimit*2)return false;
    std::vector<wchar_t> native((size+sizeof(wchar_t)-1)/sizeof(wchar_t));
    if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_RAW_HEADERS_CRLF,WINHTTP_HEADER_NAME_BY_INDEX,native.data(),&size,WINHTTP_NO_HEADER_INDEX))return false;
    const auto chars=wcsnlen(native.data(),native.size());
    const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,native.data(),int(chars),nullptr,0,nullptr,nullptr);
    if(bytes<1||bytes>int(HeaderLimit-4))return false;
    if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,native.data(),int(chars),reinterpret_cast<char*>(slot.response.data()),bytes,nullptr,nullptr)!=bytes)return false;
    slot.headerBytes=unsigned(bytes);
    if(slot.headerBytes<4||std::memcmp(slot.response.data()+slot.headerBytes-4,"\r\n\r\n",4)){
        std::memcpy(slot.response.data()+slot.headerBytes,"\r\n\r\n",4);slot.headerBytes+=4;
    }
    auto copyHeader=[&](DWORD query,char* out,unsigned capacity){
        std::array<wchar_t,128> value{};DWORD length=DWORD(value.size()*sizeof(wchar_t));
        if(!WinHttpQueryHeaders(request,query,WINHTTP_HEADER_NAME_BY_INDEX,value.data(),&length,WINHTTP_NO_HEADER_INDEX))return;
        const auto count=wcsnlen(value.data(),value.size());
        const int needed=WideCharToMultiByte(CP_UTF8,0,value.data(),int(count),nullptr,0,nullptr,nullptr);
        if(needed>0&&needed<int(capacity))WideCharToMultiByte(CP_UTF8,0,value.data(),int(count),out,needed,nullptr,nullptr);
    };
    copyHeader(WINHTTP_QUERY_STATUS_TEXT,slot.status,sizeof(slot.status));
    copyHeader(WINHTTP_QUERY_CONTENT_TYPE,slot.contentType,sizeof(slot.contentType));
    return true;
}
bool transfer(State& state,Slot& slot){
    if(stopped(state,slot))return false;
    Handles native;if(!native.context.event)return false;
    native.session=WinHttpOpen(L"cod4x1khz-plugin/1",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC);
    if(!native.session)return false;
    WinHttpSetTimeouts(native.session,5000,5000,5000,5000);
    native.connection=WinHttpConnect(native.session,slot.host.c_str(),slot.port,0);if(!native.connection)return false;
    native.request=WinHttpOpenRequest(native.connection,slot.method.c_str(),slot.path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,slot.secure?WINHTTP_FLAG_SECURE:0);
    if(!native.request)return false;
    DWORD_PTR pointer=reinterpret_cast<DWORD_PTR>(&native.context);
    if(!WinHttpSetOption(native.request,WINHTTP_OPTION_CONTEXT_VALUE,&pointer,sizeof(pointer)))return false;
    if(WinHttpSetStatusCallback(native.request,callback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0)==WINHTTP_INVALID_STATUS_CALLBACK)return false;
    native.callbackInstalled=true;
    DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_NEVER,autologon=WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if(!WinHttpSetOption(native.request,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect))||!WinHttpSetOption(native.request,WINHTTP_OPTION_AUTOLOGON_POLICY,&autologon,sizeof(autologon)))return false;
    if(!slot.headers.empty()&&!WinHttpAddRequestHeaders(native.request,slot.headers.c_str(),DWORD(slot.headers.size()),WINHTTP_ADDREQ_FLAG_ADD))return false;
    if(!asyncResult(WinHttpSendRequest(native.request,WINHTTP_NO_ADDITIONAL_HEADERS,0,slot.payload.empty()?WINHTTP_NO_REQUEST_DATA:slot.payload.data(),DWORD(slot.payload.size()),DWORD(slot.payload.size()),pointer))||!wait(state,slot,native.context,Sent))return false;
    if(!asyncResult(WinHttpReceiveResponse(native.request,nullptr))||!wait(state,slot,native.context,Headers)||!responseHeaders(native.request,slot))return false;
    // Content-Length is checked before reading, except for HEAD responses;
    // transfer/chunk framing is WinHTTP's, not a second handwritten parser.
    std::uint64_t expected=UINT64_MAX;
    if(slot.method!=L"HEAD"){
        std::array<wchar_t,32> length{};DWORD bytes=DWORD(length.size()*sizeof(wchar_t));
        if(WinHttpQueryHeaders(native.request,WINHTTP_QUERY_CONTENT_LENGTH,WINHTTP_HEADER_NAME_BY_INDEX,length.data(),&bytes,WINHTTP_NO_HEADER_INDEX)){
            const auto count=wcsnlen(length.data(),length.size());if(!count||count==length.size())return false;
            expected=0;for(unsigned i=0;i<count;++i){if(length[i]<L'0'||length[i]>L'9'||expected>MaxResponse/10)return false;expected=expected*10+unsigned(length[i]-L'0');}
            if(expected>MaxResponse-slot.headerBytes)return false;
        }else if(GetLastError()!=ERROR_WINHTTP_HEADER_NOT_FOUND)return false;
    }
    for(;;){
        if(!asyncResult(WinHttpQueryDataAvailable(native.request,nullptr))||!wait(state,slot,native.context,Available))return false;
        const auto available=native.context.available.load(std::memory_order_relaxed);if(!available)break;
        const unsigned left=MaxResponse-slot.headerBytes-slot.bodyBytes;if(!left)return false;
        const DWORD wanted=std::min<unsigned>(available,left);
        if(!asyncResult(WinHttpReadData(native.request,slot.response.data()+slot.headerBytes+slot.bodyBytes,wanted,nullptr))||!wait(state,slot,native.context,Read))return false;
        const auto received=native.context.readBytes.load(std::memory_order_relaxed);if(!received||received>wanted)return false;
        slot.bodyBytes+=received;
    }
    if(expected!=UINT64_MAX&&slot.bodyBytes!=expected)return false;
    slot.response[slot.headerBytes+slot.bodyBytes]=0;
    return !stopped(state,slot);
}
void reclaim(State& state,Slot& slot){
    Guard publicGuard(slot);if(!publicGuard.held)return;
    std::lock_guard<std::mutex> lock(state.allocation);
    const auto phase=slot.phase.load(std::memory_order_acquire);
    if(!slot.released.load(std::memory_order_acquire)||(phase!=Phase::Ready&&phase!=Phase::Published))return;
    std::vector<unsigned char>().swap(slot.payload);std::vector<unsigned char>().swap(slot.response);
    std::wstring().swap(slot.host);std::wstring().swap(slot.path);std::wstring().swap(slot.method);std::wstring().swap(slot.headers);
    state.buffered.fetch_sub(slot.charge,std::memory_order_relaxed);state.active.fetch_sub(1,std::memory_order_relaxed);
    slot.charge=0;slot.phase.store(Phase::Free,std::memory_order_release);
}
void worker(State* state){
    while(!state->stopping.load(std::memory_order_acquire)){
        for(auto& slot:state->slots){
            {
                Guard guard(slot);
                const auto phase=slot.phase.load(std::memory_order_acquire);
                if(guard.held&&phase!=Phase::Free&&retired(*state,slot)){
                    slot.cancelled.store(true,std::memory_order_release);
                    if(phase!=Phase::Published||slot.generation.load(std::memory_order_relaxed)<=state->discarded[slot.owner.load(std::memory_order_relaxed)].load(std::memory_order_acquire))slot.released.store(true,std::memory_order_release);
                }
            }
            auto phase=slot.phase.load(std::memory_order_acquire);
            if(phase==Phase::Queued){
                if(!slot.phase.compare_exchange_strong(phase,Phase::Running,std::memory_order_acq_rel))continue;
                try{slot.success=transfer(*state,slot);}catch(...){slot.success=false;}
                if(retired(*state,slot)){slot.cancelled.store(true,std::memory_order_release);slot.released.store(true,std::memory_order_release);}
                if(!slot.success)std::strcpy(slot.status,slot.cancelled.load()?"cancelled":GetTickCount64()>=slot.deadline?"deadline":"transport error");
                state->completed.fetch_add(1,std::memory_order_relaxed);slot.phase.store(Phase::Ready,std::memory_order_release);
            }
            phase=slot.phase.load(std::memory_order_acquire);
            if((phase==Phase::Ready||phase==Phase::Published)&&slot.released.load(std::memory_order_acquire))reclaim(*state,slot);
        }
        WaitForSingleObject(state->wake,10);
    }
}
Slot* find(State& state,unsigned owner,std::uint64_t generation,ftRequest_t* pointer){
    if(owner>=OwnerCount||!generation||!pointer)return nullptr;
    for(auto& slot:state.slots)if(&slot.publicView==pointer&&slot.phase.load(std::memory_order_acquire)!=Phase::Free&&slot.owner.load(std::memory_order_relaxed)==owner&&slot.generation.load(std::memory_order_relaxed)==generation)return &slot;
    return nullptr;
}
void publish(Slot& slot){
    auto& view=slot.publicView;view.complete=1;view.active=view.transferactive=0;
    view.code=slot.success?int(slot.statusCode):0;
    std::memcpy(view.status,slot.status,sizeof(view.status));std::memcpy(view.contentType,slot.contentType,sizeof(view.contentType));
    if(slot.success){
        view.headerLength=int(slot.headerBytes);view.contentLength=view.contentLengthArrived=int(slot.bodyBytes);
        view.totalreceivedbytes=view.finallen=int(slot.headerBytes+slot.bodyBytes);
        view.recvmsg.readonly=1;view.recvmsg.data=slot.response.data();view.recvmsg.maxsize=MaxResponse;view.recvmsg.cursize=view.finallen;view.extrecvmsg=&view.recvmsg;
    }
    slot.phase.store(Phase::Published,std::memory_order_release);
}
}

bool Init(){
    std::lock_guard<std::mutex> lock(lifecycle);if(current.load(std::memory_order_acquire))return true;
    auto state=std::unique_ptr<State>(new(std::nothrow) State);if(!state)return false;
    state->wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!state->wake)return false;
    try{for(auto& thread:state->workers)thread=std::thread(worker,state.get());}
    catch(...){state->stopping.store(true);SetEvent(state->wake);for(auto& thread:state->workers)if(thread.joinable())thread.join();CloseHandle(state->wake);return false;}
    current.store(state.release(),std::memory_order_release);return true;
}
ftRequest_t* Make(unsigned owner,std::uint64_t generation,const char* address,const char* method,const unsigned char* payload,int length,const char* headers){
    auto* state=current.load(std::memory_order_acquire);if(!state)return nullptr;
    auto reject=[&]()->ftRequest_t*{state->rejected.fetch_add(1,std::memory_order_relaxed);return nullptr;};
    std::string_view urlText,methodText,headerText;
    if(owner>=OwnerCount||!generation||generation<=state->retired[owner].load(std::memory_order_acquire)||length<0||unsigned(length)>MaxPayload||(length&&!payload)
        ||!text(address,1023,&urlText)||!text(method,16,&methodText)||!plugins::io::Method(methodText)||!text(headers?headers:"",HeaderLimit,&headerText)||!plugins::io::Headers(headerText))return reject();
    std::wstring host,path;INTERNET_PORT port=0;bool secure=false;
    if(!url(urlText,&host,&path,&port,&secure))return reject();
    std::unique_lock<std::mutex> lock(state->allocation,std::try_to_lock);
    if(!lock.owns_lock()||state->stopping.load()||generation<=state->retired[owner].load(std::memory_order_acquire))return reject();
    unsigned owned=0;Slot* selected=nullptr;
    for(auto& slot:state->slots){const auto phase=slot.phase.load(std::memory_order_acquire);if(phase==Phase::Free){if(!selected)selected=&slot;}else if(slot.owner.load(std::memory_order_relaxed)==owner)++owned;}
    const unsigned charge=unsigned(length)+MaxResponse+1+2*HeaderLimit+16384;
    if(!selected||owned>=MaxPerPlugin||charge>MaxBuffered-state->buffered.load(std::memory_order_relaxed))return reject();
    auto& slot=*selected;
    Guard guard(slot);if(!guard.held)return reject();
    try{
        slot.host=std::move(host);slot.path=std::move(path);slot.method=wide(methodText);slot.headers=wide(headerText);
        if(length)slot.payload.assign(payload,payload+length);else slot.payload.clear();slot.response.assign(MaxResponse+1,0);
    }catch(...){std::vector<unsigned char>().swap(slot.payload);std::vector<unsigned char>().swap(slot.response);return reject();}
    if(slot.method.empty()||(!headerText.empty()&&slot.headers.empty())){std::vector<unsigned char>().swap(slot.payload);std::vector<unsigned char>().swap(slot.response);return reject();}
    slot.owner.store(owner,std::memory_order_relaxed);slot.generation.store(generation,std::memory_order_relaxed);
    slot.cancelled.store(false,std::memory_order_relaxed);slot.released.store(false,std::memory_order_relaxed);
    slot.deadline=GetTickCount64()+DeadlineMs;slot.port=port;slot.secure=secure;slot.success=false;slot.charge=charge;
    slot.headerBytes=slot.bodyBytes=slot.statusCode=0;std::memset(slot.status,0,sizeof(slot.status));std::memset(slot.contentType,0,sizeof(slot.contentType));
    auto& view=slot.publicView;std::memset(&view,0,sizeof(view));view.active=view.transferactive=1;view.socket=view.transfersocket=-1;
    view.protocol=secure?FT_PROTO_HTTPS:FT_PROTO_HTTP;view.startTime=int(slot.deadline-DeadlineMs);view.sentBytes=length;
    std::memcpy(view.url,urlText.data(),urlText.size());std::memcpy(view.address,urlText.data(),urlText.size());
    state->active.fetch_add(1,std::memory_order_relaxed);state->buffered.fetch_add(charge,std::memory_order_relaxed);state->submitted.fetch_add(1,std::memory_order_relaxed);
    slot.phase.store(Phase::Queued,std::memory_order_release);
    if(generation<=state->retired[owner].load(std::memory_order_acquire)){slot.cancelled.store(true,std::memory_order_release);slot.released.store(true,std::memory_order_release);SetEvent(state->wake);return reject();}
    SetEvent(state->wake);return &view;
}
int Poll(unsigned owner,std::uint64_t generation,ftRequest_t* request){
    auto* state=current.load(std::memory_order_acquire);if(!state)return -1;
    auto* slot=find(*state,owner,generation,request);if(!slot||slot->released.load(std::memory_order_acquire)||slot->cancelled.load(std::memory_order_acquire)||generation<=state->retired[owner].load(std::memory_order_acquire))return -1;
    Guard guard(*slot);if(!guard.held)return 0;
    const auto phase=slot->phase.load(std::memory_order_acquire);
    if(slot->owner.load()!=owner||slot->generation.load()!=generation||phase==Phase::Free||slot->released.load())return -1;
    if(phase==Phase::Ready)publish(*slot);
    else if(phase!=Phase::Published){
        if(GetTickCount64()>=slot->deadline){slot->cancelled.store(true,std::memory_order_release);return -1;}return 0;
    }
    return slot->success?1:-1;
}
void Free(unsigned owner,std::uint64_t generation,ftRequest_t* request){
    auto* state=current.load(std::memory_order_acquire);if(!state)return;
    auto* slot=find(*state,owner,generation,request);if(!slot)return;
    if(slot->released.load(std::memory_order_acquire))return;
    slot->cancelled.store(true,std::memory_order_release);slot->released.store(true,std::memory_order_release);SetEvent(state->wake);
}
void Cancel(unsigned owner,std::uint64_t generation){
    auto* state=current.load(std::memory_order_acquire);if(!state||owner>=OwnerCount||!generation)return;
    auto old=state->retired[owner].load(std::memory_order_relaxed);
    while(old<generation&&!state->retired[owner].compare_exchange_weak(old,generation,std::memory_order_release,std::memory_order_relaxed)){}
    for(auto& slot:state->slots){Guard guard(slot);const auto phase=slot.phase.load(std::memory_order_acquire);if(guard.held&&phase!=Phase::Free&&slot.owner.load()==owner&&slot.generation.load()<=generation){slot.cancelled.store(true,std::memory_order_release);if(phase!=Phase::Published)slot.released.store(true,std::memory_order_release);state->cancelled.fetch_add(1,std::memory_order_relaxed);}}
    SetEvent(state->wake);
}
void Discard(unsigned owner,std::uint64_t generation){
    auto* state=current.load(std::memory_order_acquire);if(!state||owner>=OwnerCount||!generation)return;
    Cancel(owner,generation);
    auto old=state->discarded[owner].load(std::memory_order_relaxed);
    while(old<generation&&!state->discarded[owner].compare_exchange_weak(old,generation,std::memory_order_release,std::memory_order_relaxed)){}
    // Final unload guarantees no further DLL reads. The worker performs the
    // actual destruction, including slots briefly held by a host-side guard.
    SetEvent(state->wake);
}
void Shutdown(){
    std::lock_guard<std::mutex> lock(lifecycle);auto* state=current.exchange(nullptr,std::memory_order_acq_rel);if(!state)return;
    state->stopping.store(true,std::memory_order_release);for(auto& slot:state->slots)slot.cancelled.store(true,std::memory_order_release);SetEvent(state->wake);
    for(auto& thread:state->workers)if(thread.joinable())thread.join();CloseHandle(state->wake);delete state;
}
Statistics Stats(){
    Statistics result;auto* state=current.load(std::memory_order_acquire);if(!state)return result;
    result.active=state->active.load();result.buffered=state->buffered.load();result.submitted=state->submitted.load();result.completed=state->completed.load();result.cancelled=state->cancelled.load();result.rejected=state->rejected.load();return result;
}
}
