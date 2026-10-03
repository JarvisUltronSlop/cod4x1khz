#include <universal/q_shared.h>
#include "khz_screenshot.h"
#include "khz_screenshot_protocol.h"
#include "khz_screenshot_image.h"
#include "khz_client_owner.h"
#include "khz_runtime.h"
#include <client_mp/client_mp.h>
#include <client/client.h>
#include <qcommon/threads.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

namespace {
namespace wire=khz::screenshot;
enum class Phase:unsigned { Free,Captured,Encoding,Encoded };
enum class ControlKind { Request,Done,Error };
struct Control {ControlKind kind=ControlKind::Request;wire::Request request{};std::uint64_t generation=0;wire::Error error=wire::Error::Invalid;};
struct State {
    khz::SpscQueue<Control,8> controls;
    std::atomic<bool> stopping{false};std::atomic<std::uint64_t> epoch{1};std::atomic<Phase> phase{Phase::Free};
    HANDLE wake=nullptr;std::thread worker;std::unique_ptr<std::uint8_t[]> pixels;
    // Main publishes these once in Captured; the worker publishes Encoded once.
    wire::Request job{};std::uint64_t jobGeneration=0,jobEpoch=0;unsigned width=0,height=0;
    std::vector<std::uint8_t> jpeg;wire::Metadata metadata{};bool encoded=false;
    // Main service owns the transfer cursor, timeout and error publication.
    wire::Request active{};std::uint64_t activeGeneration=0,deadline=0,nextSend=0;
    unsigned nextChunk=0;bool metadataSent=false,awaitingDone=false;
    bool errorPending=false;wire::Rejection pendingError{};std::uint64_t errorGeneration=0;
    // The sole reliable packet owner tracks monotonic requests per owner life.
    std::uint64_t producerGeneration=0,highestId=0;
    std::atomic<std::uint64_t> requests{0},duplicates{0},rejected{0},captured{0},encodedCount{0},chunks{0},completed{0},errors{0},stale{0},queueDrops{0};
};
State* state=nullptr;
void owner(){iassert(Sys_IsMainThread());}
bool privateAddress(const netadr_t& address){if(address.type==NA_LOOPBACK)return true;if(address.type!=NA_IP||!address.port)return false;
    const auto* ip=address.ip;return ip[0]==127||ip[0]==10||(ip[0]==172&&ip[1]>=16&&ip[1]<=31)||(ip[0]==192&&ip[1]==168);}
// Called only by the native packet owner. Session/serverId are its live CL
// storage; main never races a Win32 64-bit serverHeader read.
bool accepted(const wire::Request& request){return KHZ_IsClientSimulationThread()&&KHZ_ClientOwnerRunning()
    &&CL_GetLocalClientConnectionState(0)==CA_ACTIVE&&request.session==KHZ_AcceptedClientSession()
    &&request.generation==std::uint32_t(CL_GetLocalClientGlobals(0)->serverId)
    &&privateAddress(CL_GetLocalClientConnection(0)->serverAddress);}
bool current(std::uint64_t generation){return generation&&KHZ_ClientOwnerRunning()&&!KHZ_ClientOwnerHadFault()
    &&generation==KHZ_ClientOwnerGeneration();}
void work(){
    while(!state->stopping.load(std::memory_order_acquire)){
        auto expected=Phase::Captured;
        if(!state->phase.compare_exchange_strong(expected,Phase::Encoding,std::memory_order_acq_rel)){WaitForSingleObject(state->wake,100);continue;}
        state->encoded=false;state->jpeg.clear();state->metadata={};
        try{
            if(state->jobEpoch==state->epoch.load(std::memory_order_acquire)&&wire::EncodeJpeg(state->pixels.get(),state->width,state->height,
                &state->jpeg,&state->metadata.width,&state->metadata.height)){
                state->metadata.request=state->job;state->metadata.bytes=unsigned(state->jpeg.size());
                state->metadata.chunks=(state->metadata.bytes+wire::ChunkBytes-1)/wire::ChunkBytes;
                state->encoded=khz::content::Sha256(state->jpeg.data(),state->jpeg.size(),&state->metadata.sha256)&&wire::ValidMetadata(state->metadata);
            }
        }catch(...){state->encoded=false;state->jpeg.clear();}
        if(state->jobEpoch!=state->epoch.load(std::memory_order_acquire))state->encoded=false;
        state->phase.store(Phase::Encoded,std::memory_order_release);
    }
}
void defer(const wire::Request& request,wire::Error error,std::uint64_t generation){if(!state->errorPending){state->pendingError={request,error};state->errorGeneration=generation;state->errorPending=true;}++state->errors;}
void clearTransfer(){state->active={};state->activeGeneration=0;state->awaitingDone=false;state->metadataSent=false;state->nextChunk=0;}
void releaseEncoded(){if(state->phase.load(std::memory_order_acquire)==Phase::Encoded){state->jpeg.clear();state->phase.store(Phase::Free,std::memory_order_release);}}
void cancel(){++state->epoch;clearTransfer();state->errorPending=false;releaseEncoded();Control control;while(state->controls.pop(control)){};}
void control(const Control& value){
    if(!current(value.generation)){++state->stale;return;}
    if(value.kind==ControlKind::Done||value.kind==ControlKind::Error){
        if(state->active==value.request&&state->activeGeneration==value.generation){
            if(value.kind==ControlKind::Done&&state->awaitingDone)++state->completed;else ++state->errors;
            clearTransfer();releaseEncoded();
        }else ++state->stale;return;
    }
    if(state->active.id||state->phase.load(std::memory_order_acquire)!=Phase::Free){defer(value.request,wire::Error::Busy,value.generation);return;}
    unsigned width=0,height=0;
    if(KHZ_IsHeadless()){defer(value.request,wire::Error::Unavailable,value.generation);return;}
    if(!KHZ_CaptureScreenshotPixels(state->pixels.get(),wire::MaximumCaptureBytes,&width,&height)
        ||!width||width>wire::MaximumCaptureWidth||!height||height>wire::MaximumCaptureHeight){defer(value.request,wire::Error::Capture,value.generation);return;}
    // Renderer synchronization may take time, but gameplay continues. Recheck
    // the published owner generation before handing these pixels to the worker.
    if(!current(value.generation)){++state->stale;return;}
    state->active=value.request;state->activeGeneration=value.generation;state->deadline=GetTickCount64()+30000;
    state->job=value.request;state->jobGeneration=value.generation;state->jobEpoch=state->epoch.load(std::memory_order_acquire);
    state->width=width;state->height=height;state->metadataSent=false;state->nextChunk=0;state->awaitingDone=false;
    ++state->captured;state->phase.store(Phase::Captured,std::memory_order_release);SetEvent(state->wake);
}
}
void KHZ_ScreenshotClientInit(){
    owner();if(state)return;try{state=new State;state->pixels=std::make_unique<std::uint8_t[]>(wire::MaximumCaptureBytes);
        state->jpeg.reserve(wire::MaximumBytes);state->wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!state->wake)throw 1;
        state->worker=std::thread(work);
    }catch(...){if(state){if(state->wake)CloseHandle(state->wake);delete state;state=nullptr;}}
}
bool KHZ_ScreenshotClientServerCommand(const char* text){
    if(!text||std::strncmp(text,"khz_ss_",7))return false;if(!state)return true;
    // The immutable reliable journal is replayed once for stock presentation.
    // This private service command was already handled by the packet owner.
    if(!KHZ_IsClientSimulationThread())return true;
    wire::Request request;wire::Rejection error;Control value;
    if(wire::ParseRequest(text,&request)){value.kind=ControlKind::Request;value.request=request;}
    else if(wire::ParseDone(text,&request)){value.kind=ControlKind::Done;value.request=request;}
    else if(wire::ParseRejection(text,&error)){value.kind=ControlKind::Error;value.request=error.request;value.error=error.error;}
    else {++state->rejected;return true;}
    if(!accepted(value.request)){++state->rejected;return true;}
    value.generation=KHZ_ClientOwnerGeneration();
    if(value.kind==ControlKind::Request){
        if(value.generation!=state->producerGeneration){state->producerGeneration=value.generation;state->highestId=0;}
        if(value.request.id<=state->highestId){++state->duplicates;return true;}
        state->highestId=value.request.id;++state->requests;
    }
    if(!state->controls.push(value)){++state->queueDrops;++state->rejected;}return true;
}
void KHZ_ScreenshotClientService(){
    owner();if(!state)return;Control value;for(unsigned i=0;i<8&&state->controls.pop(value);++i)control(value);
    const auto time=GetTickCount64();
    if(state->active.id&&!current(state->activeGeneration)){++state->stale;cancel();}
    if(state->phase.load(std::memory_order_acquire)==Phase::Encoded&&!state->active.id)releaseEncoded();
    if(state->active.id&&time>state->deadline){defer(state->active,wire::Error::Timeout,state->activeGeneration);++state->epoch;clearTransfer();releaseEncoded();}
    // At most one reliable chunk per millisecond and one per presentation pass,
    // leaving the owner's reliable queue room for native control traffic.
    if(time<state->nextSend)return;
    char text[wire::MaximumText+1];
    if(state->errorPending){
        if(!current(state->errorGeneration)){state->errorPending=false;++state->stale;return;}
        if(wire::FormatRejection(state->pendingError,text,sizeof(text))&&KHZ_ClientOwnerTryQueueReliable(text)){
            state->errorPending=false;state->nextSend=time+1;}
        return;
    }
    if(!state->active.id||state->awaitingDone||state->phase.load(std::memory_order_acquire)!=Phase::Encoded)return;
    if(!current(state->activeGeneration)){++state->stale;cancel();return;}
    if(!state->encoded){defer(state->active,wire::Error::Encoding,state->activeGeneration);clearTransfer();releaseEncoded();return;}
    if(!state->metadataSent){
        if(wire::FormatMetadata(state->metadata,text,sizeof(text))&&KHZ_ClientOwnerTryQueueReliable(text)){
            state->metadataSent=true;++state->encodedCount;state->nextSend=time+1;}return;
    }
    wire::Chunk chunk;chunk.request=state->active;chunk.index=state->nextChunk;const auto offset=chunk.index*wire::ChunkBytes;
    chunk.bytes=(std::min)(wire::ChunkBytes,state->metadata.bytes-offset);std::memcpy(chunk.data.data(),state->jpeg.data()+offset,chunk.bytes);
    if(wire::FormatChunk(chunk,text,sizeof(text))&&KHZ_ClientOwnerTryQueueReliable(text)){
        ++state->chunks;state->nextSend=time+1;state->awaitingDone=++state->nextChunk==state->metadata.chunks;
    }
}
void KHZ_ScreenshotClientDisconnect(){owner();if(state){cancel();state->producerGeneration=state->highestId=0;}}
void KHZ_ScreenshotClientShutdown(){
    owner();if(!state)return;cancel();state->stopping.store(true,std::memory_order_release);SetEvent(state->wake);
    if(state->worker.joinable())state->worker.join();CloseHandle(state->wake);delete state;state=nullptr;
}
const char* KHZ_ScreenshotClientStatsJSON(){
    owner();static char text[768];if(!state)return "{\"initialized\":false}";
    std::snprintf(text,sizeof(text),"{\"initialized\":true,\"requests\":%llu,\"duplicates\":%llu,\"rejected\":%llu,\"captured\":%llu,\"encoded\":%llu,\"chunks\":%llu,\"completed\":%llu,\"errors\":%llu,\"stale\":%llu,\"queue_drops\":%llu,\"pending\":%s,\"awaiting_done\":%s,\"phase\":%u,\"capture_bound_bytes\":%u,\"encoded_bound_bytes\":%u}",
        state->requests.load(),state->duplicates.load(),state->rejected.load(),state->captured.load(),state->encodedCount.load(),state->chunks.load(),state->completed.load(),state->errors.load(),state->stale.load(),state->queueDrops.load(),
        state->active.id?"true":"false",state->awaitingDone?"true":"false",unsigned(state->phase.load()),wire::MaximumCaptureBytes,wire::MaximumBytes);return text;
}
