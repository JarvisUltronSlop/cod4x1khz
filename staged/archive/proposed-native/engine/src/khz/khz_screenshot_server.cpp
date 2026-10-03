#include <universal/q_shared.h>
#include "khz_screenshot.h"
#include "khz_screenshot_protocol.h"
#include "khz_screenshot_image.h"
#include "khz_admin_server.h"
#include "khz_bots.h"
#include "khz_paths.h"
#include "khz_plugins.h"
#include "khz_runtime.h"
#include <server_mp/server_mp.h>
#include <qcommon/threads.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <thread>

namespace {
namespace wire=khz::screenshot;
constexpr unsigned Concurrent=4,PendingCapacity=64;
constexpr std::uint64_t TimeoutMs=30000,QueueTimeoutMs=120000;
enum class Phase:unsigned { Free,Receiving,VerifyReady,Verifying,ResultReady,DiscardReady,Discarding };
struct Ticket {
    wire::Request request{};wire::Connection connection{};KHZ_ScreenshotRequester requester{};
    std::uint64_t epoch=0,queued=0,deadline=0;
    char basename[49]{},relative[192]{};wchar_t root[512]{};
};
struct Slot {
    std::atomic<Phase> phase{Phase::Free};std::atomic<bool> cancel{false};
    Ticket ticket{};wire::Assembly assembly;std::unique_ptr<std::uint8_t[]> payload;
    wire::Metadata metadata{};wire::Error error=wire::Error::Storage;
    bool success=false,saved=false;char path[1024]{};
};
struct Counters {std::uint64_t requested=0,queued=0,sent=0,metadata=0,chunks=0,duplicates=0,rejected=0,verified=0,saved=0,errors=0,stale=0,timeouts=0;};
struct State {
    std::array<Slot,Concurrent> slots;
    std::array<Ticket,PendingCapacity> pending;
    unsigned read=0,write=0;std::uint64_t sequence=0,session=0;std::uint32_t map=0;
    std::atomic<std::uint64_t> epoch{1};std::atomic<bool> stopping{false};
    HANDLE wake=nullptr;std::thread worker;Counters counters;
};
State* state=nullptr;
void owner(){iassert(Sys_IsMainThread());}
bool privateAddress(const netadr_t& address){
    if(address.type==NA_LOOPBACK)return true;if(address.type!=NA_IP||!address.port)return false;
    const auto* ip=address.ip;return ip[0]==127||ip[0]==10||(ip[0]==172&&ip[1]>=16&&ip[1]<=31)||(ip[0]==192&&ip[1]==168);
}
int clientSlot(const client_t* client){
    if(!client||!svs.clients||!sv_maxclients)return -1;
    const auto first=reinterpret_cast<std::uintptr_t>(svs.clients),value=reinterpret_cast<std::uintptr_t>(client);
    if(value<first||(value-first)%sizeof(client_t))return -1;
    const auto index=(value-first)/sizeof(client_t);return index<64&&index<unsigned(sv_maxclients->current.integer)?int(index):-1;
}
wire::Connection connection(const client_t* client){
    wire::Connection result;const int index=clientSlot(client);if(index<0)return result;
    result.slot=unsigned(index);result.session=KHZ_CurrentServerSession();result.generation=std::uint32_t(sv_serverId_value);
    result.lifecycle=KHZ_AdminClientGeneration(client);result.active=client->header.state==CS_ACTIVE;
    result.privateAddress=privateAddress(client->header.netchan.remoteAddress);result.bot=KHZ_IsNativeBot(client);
    if(const auto* identity=KHZ_AdminClientIdentity(client))result.fingerprint=*identity;return result;
}
bool current(const Ticket& ticket){return state&&ticket.epoch==state->epoch.load(std::memory_order_acquire)&&svs.clients&&ticket.connection.slot<64
    &&wire::SameConnection(ticket.connection,connection(svs.clients+ticket.connection.slot));}
bool cancelled(const Slot& slot){return state->stopping.load(std::memory_order_acquire)||slot.cancel.load(std::memory_order_acquire)
    ||slot.ticket.epoch!=state->epoch.load(std::memory_order_acquire)||GetTickCount64()>slot.ticket.deadline;}
void sendError(const Ticket& ticket,wire::Error error){
    if(!current(ticket)){++state->counters.stale;return;}
    char text[wire::MaximumText+1];if(wire::FormatRejection({ticket.request,error},text,sizeof(text))){
        SV_SendServerCommand(svs.clients+ticket.connection.slot,SV_CMD_RELIABLE,"%s",text);++state->counters.errors;}
}
bool sameFilePath(HANDLE file,const std::filesystem::path& path,BY_HANDLE_FILE_INFORMATION* info){
    wchar_t actual[1024]{};const DWORD count=GetFinalPathNameByHandleW(file,actual,unsigned(std::size(actual)),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    return count&&count<std::size(actual)&&!std::wcsncmp(actual,L"\\\\?\\",4)&&!_wcsicmp(actual+4,path.c_str())
        &&GetFileInformationByHandle(file,info)&&!(info->dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY));
}
bool deleteOpened(HANDLE file){FILE_DISPOSITION_INFO deletion{TRUE};return SetFileInformationByHandle(file,FileDispositionInfo,&deletion,sizeof(deletion))!=FALSE;}
bool save(Slot& slot){
    std::filesystem::path path;
    if(cancelled(slot)||!khz::content::ConfinedPath(slot.ticket.root,slot.ticket.relative,&path,true))return false;
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info{};DWORD written=0;
    const bool target=sameFilePath(file,path,&info);
    const bool encoded=target&&WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path.c_str(),-1,slot.path,unsigned(sizeof(slot.path)),nullptr,nullptr)>0;
    const bool result=encoded&&!cancelled(slot)&&WriteFile(file,slot.payload.get(),slot.metadata.bytes,&written,nullptr)
        &&written==slot.metadata.bytes&&FlushFileBuffers(file)&&!cancelled(slot);
    // CREATE_NEW owns this exact handle. Mark failure for deletion while it is
    // still open, rather than deleting a path which could have been replaced.
    if(!result)deleteOpened(file);CloseHandle(file);if(!result)return false;
    slot.saved=true;return true;
}
void discard(Slot& slot){
    // Hold the opened file against writers/renames while comparing the exact
    // payload. Deletion affects that handle, never a later path replacement.
    if(slot.saved){try{std::filesystem::path path;
        if(khz::content::ConfinedPath(slot.ticket.root,slot.ticket.relative,&path)){
            HANDLE file=CreateFileW(path.c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file!=INVALID_HANDLE_VALUE){BY_HANDLE_FILE_INFORMATION info{};
                bool owned=sameFilePath(file,path,&info)&&!info.nFileSizeHigh&&info.nFileSizeLow==slot.metadata.bytes;
                std::array<std::uint8_t,4096> bytes{};unsigned offset=0;
                while(owned&&offset<slot.metadata.bytes){DWORD read=0;const unsigned expected=(std::min)(unsigned(bytes.size()),slot.metadata.bytes-offset);
                    owned=ReadFile(file,bytes.data(),expected,&read,nullptr)&&read==expected&&!std::memcmp(bytes.data(),slot.payload.get()+offset,expected);offset+=expected;}
                if(owned)deleteOpened(file);CloseHandle(file);
            }
        }}catch(...){}}
    slot.saved=false;
}
void work(){
    while(!state->stopping.load(std::memory_order_acquire)){
        bool handled=false;
        for(auto& slot:state->slots){
            auto expected=Phase::DiscardReady;
            if(slot.phase.compare_exchange_strong(expected,Phase::Discarding,std::memory_order_acq_rel)){
                discard(slot);slot.phase.store(Phase::Free,std::memory_order_release);handled=true;continue;}
            expected=Phase::VerifyReady;
            if(!slot.phase.compare_exchange_strong(expected,Phase::Verifying,std::memory_order_acq_rel))continue;
            handled=true;slot.success=false;slot.saved=false;slot.path[0]=0;slot.error=wire::Error::Checksum;
            try{khz::content::Digest digest{};
                if(!cancelled(slot)&&khz::content::Sha256(slot.payload.get(),slot.metadata.bytes,&digest)&&digest==slot.metadata.sha256
                    &&wire::ValidateJpeg(slot.payload.get(),slot.metadata.bytes,slot.metadata.width,slot.metadata.height)){
                    slot.error=wire::Error::Storage;slot.success=save(slot);
                }
            }catch(...){slot.success=false;}
            if(cancelled(slot)){discard(slot);slot.success=false;slot.error=wire::Error::Stale;}
            slot.phase.store(Phase::ResultReady,std::memory_order_release);
        }
        if(!handled)WaitForSingleObject(state->wake,100);
    }
    for(auto& slot:state->slots)if(slot.saved&&!slot.success)discard(slot);
}
void cancelSlot(Slot& slot){
    slot.cancel.store(true,std::memory_order_release);
    auto phase=slot.phase.load(std::memory_order_acquire);
    if(phase==Phase::Receiving){slot.assembly.Reset();slot.phase.store(Phase::Free,std::memory_order_release);}
    else if(phase==Phase::VerifyReady){auto expected=phase;slot.phase.compare_exchange_strong(expected,Phase::Free,std::memory_order_acq_rel);}
    else if(phase==Phase::ResultReady){if(slot.saved){slot.phase.store(Phase::DiscardReady,std::memory_order_release);SetEvent(state->wake);}
        else slot.phase.store(Phase::Free,std::memory_order_release);}
}
void reset(){++state->epoch;state->read=state->write=0;for(auto& slot:state->slots)cancelSlot(slot);SetEvent(state->wake);}
bool requesterValid(const KHZ_ScreenshotRequester& value){
    if(unsigned(value.kind)>unsigned(KHZ_ScreenshotRequesterKind::WebAdmin)||value.power<0||value.power>100||value.slot<-1||value.slot>=64)return false;
    bool identity=false;for(auto byte:value.fingerprint)identity|=byte!=0;
    if(value.kind==KHZ_ScreenshotRequesterKind::PlayerAdmin)return value.slot>=0&&identity;
    if(value.kind==KHZ_ScreenshotRequesterKind::WebAdmin)return value.slot==-1&&identity;
    return value.slot==-1;
}
void defaultName(const char* name,char* output){
    unsigned written=0;for(unsigned i=0;name&&name[i]&&written<48;++i){const char c=name[i];
        if(c=='^'&&name[i+1]>='0'&&name[i+1]<='9'){++i;continue;}
        output[written++]=((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')?c:'_';}
    output[written]=0;if(!written)std::strcpy(output,"client");
}
bool enqueue(client_t* client,const char* basename,const KHZ_ScreenshotRequester& requester){
    const auto admitted=connection(client);if(!wire::Admissible(admitted)||state->sequence==UINT64_MAX||state->write-state->read>=PendingCapacity)return false;
    for(unsigned n=state->read;n!=state->write;++n)if(wire::SameConnection(state->pending[n%PendingCapacity].connection,admitted))return false;
    for(const auto& slot:state->slots)if(slot.phase.load(std::memory_order_acquire)!=Phase::Free&&wire::SameConnection(slot.ticket.connection,admitted))return false;
    Ticket ticket;ticket.connection=admitted;ticket.request={++state->sequence,admitted.session,admitted.generation};ticket.requester=requester;
    ticket.epoch=state->epoch.load(std::memory_order_acquire);ticket.queued=GetTickCount64();
    if(basename&&*basename)std::strcpy(ticket.basename,basename);else defaultName(client->name,ticket.basename);
    std::snprintf(ticket.relative,sizeof(ticket.relative),"screenshots/%s-%llu-%llu.jpg",ticket.basename,
        static_cast<unsigned long long>(ticket.request.session),static_cast<unsigned long long>(ticket.request.id));
    try{const auto root=std::filesystem::absolute(KHZ_HomeRoot()).lexically_normal().wstring();if(root.size()>=std::size(ticket.root))return false;
        std::wcscpy(ticket.root,root.c_str());}catch(...){return false;}
    state->pending[state->write++%PendingCapacity]=ticket;++state->counters.queued;return true;
}
Slot* receiving(const client_t* client,const wire::Request& request){
    const auto admitted=connection(client);if(!wire::Admissible(admitted))return nullptr;
    for(auto& slot:state->slots)if(slot.phase.load(std::memory_order_acquire)==Phase::Receiving&&slot.ticket.request==request
        &&wire::SameConnection(slot.ticket.connection,admitted)&&current(slot.ticket))return &slot;return nullptr;
}
}
void KHZ_ScreenshotServerInit(){
    owner();if(state)return;try{state=new State;state->session=KHZ_CurrentServerSession();state->map=std::uint32_t(sv_serverId_value);
        for(auto& slot:state->slots)slot.payload=std::make_unique<std::uint8_t[]>(wire::MaximumBytes);
        state->wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!state->wake)throw 1;state->worker=std::thread(work);
    }catch(...){if(state){if(state->wake)CloseHandle(state->wake);delete state;state=nullptr;}}
}
bool KHZ_ScreenshotServerRequest(client_t* client,const char* basename,const KHZ_ScreenshotRequester& requester){
    owner();if(!state||!requesterValid(requester)||(basename&&*basename&&!wire::SafeBasename(basename)))return false;
    ++state->counters.requested;bool accepted=false;
    if(client)accepted=enqueue(client,basename,requester);
    else if(svs.clients&&sv_maxclients)for(int i=0;i<sv_maxclients->current.integer&&i<64;++i)accepted=enqueue(svs.clients+i,basename,requester)||accepted;
    if(!accepted)++state->counters.rejected;return accepted;
}
bool KHZ_ScreenshotServerClientCommand(client_t* client,const char* text){
    if(!text||std::strncmp(text,"khz_ss_",7))return false;owner();if(!state)return true;
    wire::Metadata metadata;wire::Chunk chunk;wire::Rejection error;
    if(wire::ParseMetadata(text,&metadata)){
        auto* slot=receiving(client,metadata.request);if(!slot){++state->counters.rejected;return true;}
        if(slot->assembly.GetMetadata().bytes){const auto& old=slot->assembly.GetMetadata();
            if(old.width!=metadata.width||old.height!=metadata.height||old.bytes!=metadata.bytes||old.sha256!=metadata.sha256){sendError(slot->ticket,wire::Error::Invalid);cancelSlot(*slot);++state->counters.rejected;}
            else ++state->counters.duplicates;return true;}
        if(!slot->assembly.Begin(metadata,slot->payload.get(),wire::MaximumBytes)){++state->counters.rejected;return true;}
        slot->metadata=metadata;++state->counters.metadata;return true;
    }
    if(wire::ParseChunk(text,&chunk)){
        auto* slot=receiving(client,chunk.request);if(!slot){++state->counters.rejected;return true;}
        const auto result=slot->assembly.Put(chunk);
        if(result==wire::Assembly::Accepted::Rejected){sendError(slot->ticket,wire::Error::Invalid);cancelSlot(*slot);++state->counters.rejected;}
        else if(result==wire::Assembly::Accepted::Duplicate)++state->counters.duplicates;
        else {++state->counters.chunks;if(result==wire::Assembly::Accepted::Complete){slot->phase.store(Phase::VerifyReady,std::memory_order_release);SetEvent(state->wake);}}
        return true;
    }
    if(wire::ParseRejection(text,&error)){if(auto* slot=receiving(client,error.request)){++state->counters.errors;cancelSlot(*slot);}else ++state->counters.rejected;return true;}
    ++state->counters.rejected;return true;
}
void KHZ_ScreenshotServerService(){
    owner();if(!state)return;const auto session=KHZ_CurrentServerSession();const auto map=std::uint32_t(sv_serverId_value);const auto time=GetTickCount64();
    if(session!=state->session||map!=state->map){reset();state->session=session;state->map=map;}
    for(auto& slot:state->slots){const auto phase=slot.phase.load(std::memory_order_acquire);
        if(phase==Phase::Free||phase==Phase::DiscardReady||phase==Phase::Discarding)continue;
        if(!current(slot.ticket)){++state->counters.stale;cancelSlot(slot);continue;}
        if(phase==Phase::ResultReady){
            if(slot.success){char text[wire::MaximumText+1];if(wire::FormatDone(slot.ticket.request,text,sizeof(text)))
                SV_SendServerCommand(svs.clients+slot.ticket.connection.slot,SV_CMD_RELIABLE,"%s",text);
                ++state->counters.verified;++state->counters.saved;
                KHZ_PluginEvent(KHZPluginEvent::ScreenshotArrived,reinterpret_cast<std::uintptr_t>(svs.clients+slot.ticket.connection.slot),reinterpret_cast<std::uintptr_t>(slot.path));
                Com_Printf(CON_CHANNEL_SERVER,"Screenshot saved: %s (slot %u, requester %u/%i, power %i)\n",slot.path,slot.ticket.connection.slot,
                    unsigned(slot.ticket.requester.kind),slot.ticket.requester.slot,slot.ticket.requester.power);
                slot.saved=false;slot.phase.store(Phase::Free,std::memory_order_release);
            }else {sendError(slot.ticket,slot.error);slot.phase.store(Phase::Free,std::memory_order_release);}continue;
        }
        if(time>slot.ticket.deadline&&!slot.cancel.exchange(true)){++state->counters.timeouts;sendError(slot.ticket,wire::Error::Timeout);cancelSlot(slot);}
    }
    for(auto& slot:state->slots){if(state->read==state->write)break;if(slot.phase.load(std::memory_order_acquire)!=Phase::Free)continue;
        const auto ticket=state->pending[state->read++%PendingCapacity];if(!current(ticket)||time>ticket.queued+QueueTimeoutMs){++state->counters.stale;continue;}
        slot.ticket=ticket;slot.ticket.deadline=time+TimeoutMs;slot.assembly.Reset();slot.metadata={};slot.success=slot.saved=false;slot.path[0]=0;
        slot.cancel.store(false,std::memory_order_release);slot.phase.store(Phase::Receiving,std::memory_order_release);
        char text[wire::MaximumText+1];if(wire::FormatRequest(slot.ticket.request,text,sizeof(text))){
            SV_SendServerCommand(svs.clients+slot.ticket.connection.slot,SV_CMD_RELIABLE,"%s",text);++state->counters.sent;}
        else cancelSlot(slot);
    }
}
void KHZ_ScreenshotServerErrorCleanup(){owner();if(state)reset();}
void KHZ_ScreenshotServerShutdown(){
    owner();if(!state)return;reset();state->stopping.store(true,std::memory_order_release);SetEvent(state->wake);
    if(state->worker.joinable())state->worker.join();for(auto& slot:state->slots)if(slot.saved)discard(slot);
    CloseHandle(state->wake);delete state;state=nullptr;
}
const char* KHZ_ScreenshotServerStatsJSON(){
    owner();static char text[1024];if(!state)return "{\"initialized\":false}";const auto& c=state->counters;unsigned active=0;
    for(const auto& slot:state->slots)active+=slot.phase.load(std::memory_order_acquire)!=Phase::Free;
    std::snprintf(text,sizeof(text),"{\"initialized\":true,\"requested\":%llu,\"queued\":%llu,\"sent\":%llu,\"metadata\":%llu,\"chunks\":%llu,\"duplicates\":%llu,\"rejected\":%llu,\"verified\":%llu,\"saved\":%llu,\"errors\":%llu,\"stale\":%llu,\"timeouts\":%llu,\"active\":%u,\"pending\":%u,\"payload_bound_bytes\":%u}",
        c.requested,c.queued,c.sent,c.metadata,c.chunks,c.duplicates,c.rejected,c.verified,c.saved,c.errors,c.stale,c.timeouts,active,state->write-state->read,Concurrent*wire::MaximumBytes);return text;
}

#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
#include "khz/khz_archive_probe_screenshot.inl"
#endif
