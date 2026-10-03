#include "khz_services_client.h"
#include "khz_browser.h"
#include "khz_content_protocol.h"
#include "khz_client_owner.h"
#include "khz_paths.h"
#include "khz_runtime.h"
#include <service_control_policy.hpp>
#include <client_mp/client_mp.h>
#include <client/client.h>
#include <qcommon/cmd.h>
#include <qcommon/files.h>
#include <universal/q_shared.h>
#include <ui_mp/ui_mp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <thread>

void __cdecl CL_InitServerInfo(serverInfo_t* server, netadr_t address);
void __cdecl CL_NextDownload(int localClientNum);

namespace {
using khz::browser::Endpoint;
using khz::browser::Server;
namespace content = khz::content;
namespace wire = khz::content::wire;
constexpr unsigned JobCapacity=64, ResultCapacity=256, CommandCapacity=8, WorkerCount=4;
enum class JobKind { Query, Discover, Master, Favorites };
struct Job { JobKind kind=JobKind::Query; Endpoint endpoint{}; unsigned source=0; std::uint64_t epoch=0; };
enum class ResultKind { Server, Endpoint, Finished, FavoriteSaved };
struct Result { ResultKind kind=ResultKind::Server; Server server{}; Endpoint endpoint{}; unsigned source=0; std::uint64_t epoch=0; bool success=false; };
struct PendingPing { Endpoint endpoint{}; unsigned source=0; std::uint64_t epoch=0; bool active=false; };
struct Download {
    wire::Query query{}; char origin[256]{}; std::uint64_t started=0, nextRequest=0;
    unsigned requests=0; bool pending=false, submitted=false;
};
struct State {
    std::mutex lock, favoriteStorageLock; std::condition_variable wake, available;
    std::array<Job,JobCapacity> jobs{}; std::array<Result,ResultCapacity> results{};
    unsigned jobRead=0,jobWrite=0,resultRead=0,resultWrite=0;
    std::array<std::array<char,wire::MaximumText+1>,CommandCapacity> commands{};
    unsigned commandRead=0,commandWrite=0;
    std::array<std::thread,WorkerCount> workers;
    std::atomic<bool> stopping{false};
    std::array<std::atomic<std::uint64_t>,4> epochs;
    std::array<PendingPing,JobCapacity> pings{};
    khz::browser::Favorites loadedFavorites{}, saveFavorites{};
    std::filesystem::path favoriteFile;
    bool favoritesPending=false, favoritesImported=false;
    std::uint64_t favoriteHash=0,nextFavoriteCheck=0,sequence=0;
    Download download;
    const dvar_t* downloadOrigin=nullptr; const dvar_t* masters=nullptr;
    std::atomic<unsigned> requests{0},completed{0},rejected{0},committed{0},browserFailures{0},favoriteFailures{0};
    std::atomic<unsigned> activeBrowserJobs{0},metadataReceived{0};
    std::atomic<bool> downloadPending{false};
    unsigned mastersPending=0;
    unsigned masterEndpoints=0,staleBrowserResults=0,shellBrowserDrops=0;
};
State* state=nullptr;
std::uint64_t now() { return khz::counter()*1000/khz::frequency(); }
bool pushJob(const Job& job) {
    if (!state || state->stopping.load()) return false;
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->jobWrite-state->jobRead==JobCapacity) return false;
    state->jobs[state->jobWrite++&(JobCapacity-1)]=job; state->wake.notify_one(); return true;
}
bool pushResult(const Result& result) {
    std::unique_lock<std::mutex> guard(state->lock);
    state->available.wait(guard,[]{return state->stopping.load() || state->resultWrite-state->resultRead<ResultCapacity;});
    if (state->stopping.load()) return false;
    state->results[state->resultWrite++&(ResultCapacity-1)]=result; return true;
}
bool current(const Job& job) { return job.source<4 && job.epoch==state->epochs[job.source].load(); }
void worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> guard(state->lock);
            state->wake.wait(guard,[]{return state->stopping.load() || state->jobRead!=state->jobWrite;});
            if (state->stopping.load()) return;
            job=state->jobs[state->jobRead++&(JobCapacity-1)];
        }
        if (!current(job)) continue;
        struct ActiveJob {std::atomic<unsigned>& count;explicit ActiveJob(std::atomic<unsigned>& value):count(value){++count;}~ActiveJob(){--count;}} active(state->activeBrowserJobs);
        Result result; result.source=job.source; result.epoch=job.epoch; result.endpoint=job.endpoint;
        if (job.kind==JobKind::Query) {
            result.kind=ResultKind::Server;
            result.success=khz::browser::PrivateEndpoint(job.endpoint)&&khz::browser::Query(job.endpoint,&result.server,250);
            pushResult(result);
        } else if (job.kind==JobKind::Discover) {
            std::array<Server,khz::browser::MaxLocalServers> servers{}; unsigned count=0;
            const bool success=khz::browser::Discover(&servers,&count);
            if (success) for (unsigned i=0;i<count&&current(job);++i) {
                result.kind=ResultKind::Server; result.server=servers[i]; result.endpoint=servers[i].address; result.success=true;
                if (!pushResult(result)) return;
            }
            result.kind=ResultKind::Finished; result.success=success; pushResult(result);
        } else if (job.kind==JobKind::Master) {
            std::array<Endpoint,khz::browser::MaxServers> endpoints{}; unsigned count=0;
            const bool success=khz::browser::PrivateEndpoint(job.endpoint)&&khz::browser::QueryMaster(job.endpoint,&endpoints,&count,500);
            if (success) for (unsigned i=0;i<count&&current(job);++i) {
                if (!khz::browser::PrivateEndpoint(endpoints[i])) continue;
                result.kind=ResultKind::Endpoint; result.endpoint=endpoints[i]; result.success=true;
                if (!pushResult(result)) return;
            }
            result.kind=ResultKind::Finished; result.success=success; pushResult(result);
        } else {
            // Serialize coalesced favorite writes before taking the latest
            // snapshot. A delayed older worker cannot overwrite a newer save.
            std::lock_guard<std::mutex> storage(state->favoriteStorageLock);
            khz::browser::Favorites snapshot;
            { std::lock_guard<std::mutex> guard(state->lock); snapshot=state->saveFavorites; state->favoritesPending=false; }
            result.kind=ResultKind::FavoriteSaved; result.success=snapshot.Save(state->favoriteFile); pushResult(result);
        }
    }
}
netadr_t native(const Endpoint& endpoint) {
    netadr_t address{}; address.type=NA_IP;
    std::memcpy(address.ip,endpoint.ip.data(),4); address.port=BigShort(endpoint.port); return address;
}
bool endpoint(const netadr_t& address, Endpoint* result) {
    if (!result || address.type!=NA_IP) return false;
    Endpoint value; std::memcpy(value.ip.data(),address.ip,4); value.port=std::uint16_t(BigShort(address.port));
    if (!khz::browser::PrivateEndpoint(value)) return false;
    *result=value; return true;
}
serverInfo_t* list(unsigned source,int** count,unsigned* maximum) {
    if (source==0) { *count=&cls.numlocalservers; *maximum=128; return cls.localServers; }
    if (source==1) { *count=&cls.numglobalservers; *maximum=khz::browser::MaxServers; return cls.globalServers; }
    if (source==2) { *count=&cls.numfavoriteservers; *maximum=128; return cls.favoriteServers; }
    return nullptr;
}
void initialize(serverInfo_t& server,const Endpoint& address) { CL_InitServerInfo(&server,native(address)); server.dirty=1; }
void publish(const Result& result) {
    int* count=nullptr; unsigned maximum=0; auto* servers=list(result.source,&count,&maximum); if (!servers) return;
    *count=(std::max)(0,(std::min)(*count,int(maximum)));
    unsigned index=0;
    for (;index<unsigned(*count);++index) { Endpoint address; if (endpoint(servers[index].adr,&address)&&address==result.endpoint) break; }
    if (index==unsigned(*count)) {
        if (index==maximum || !result.success) return;
        initialize(servers[index],result.endpoint); ++*count;
    }
    auto& server=servers[index];
    if (result.kind==ResultKind::Endpoint) return;
    if (!result.success || !result.server.compatible) { server.ping=0; server.dirty=0; return; }
    const auto& value=result.server;
    std::snprintf(server.hostName,sizeof(server.hostName),"%s",value.hostname);
    std::snprintf(server.mapName,sizeof(server.mapName),"%s",value.map);
    std::snprintf(server.gameType,sizeof(server.gameType),"%s",value.gameType);
    std::snprintf(server.game,sizeof(server.game),"%s",value.mod);
    server.clients=value.clients; server.maxClients=value.maximum;
    server.ping=short((std::min)(32767u,(std::max)(1u,value.pingMs))); server.netType=1;
    server.pure=value.pure; server.bPassword=value.password; server.mod=value.mod[0]!=0;
    server.dirty=1; server.requestCount=0;
}
bool queuePing(const Endpoint& address,unsigned source) {
    if (!state || source>3 || !khz::browser::PrivateEndpoint(address)) return false;
    const auto epoch=state->epochs[source].load();
    for (const auto& ping:state->pings) if (ping.active&&ping.source==source&&ping.epoch==epoch&&ping.endpoint==address) return true;
    for (auto& ping:state->pings) if (!ping.active) {
        if (!pushJob({JobKind::Query,address,source,epoch})) return false;
        ping={address,source,epoch,true}; return true;
    }
    return false;
}
void clearSource(unsigned source) {
    ++state->epochs[source];
    for (auto& ping:state->pings) if (ping.source==source) ping.active=false;
}
void failDownload(const char* reason) {
    if (state) { content::Cancel(state->download.query.session,state->download.query.generation); state->download.pending=false; state->downloadPending=false; ++state->rejected; }
    Com_Error(ERR_DROP,"Verified custom download failed: %s",reason);
}
bool currentDownload() {
    if (!state || !state->download.pending) return false;
    const auto& query=state->download.query;
    return query.session==KHZ_AcceptedClientSession()&&query.generation==std::uint64_t(std::uint32_t(CL_GetLocalClientGlobals(0)->serverId))
        && CL_GetLocalClientConnectionState(0)>=CA_CONNECTED&&CL_GetLocalClientConnectionState(0)<CA_ACTIVE;
}
void command(const char* text) {
    if (!state->download.pending) return;
    wire::Metadata metadata; wire::Rejection rejection; auto& pending=state->download;
    if (wire::ParseMetadata(text,&metadata)) {
        if (metadata.query.id!=pending.query.id) return;
        content::Request request;
        if (!currentDownload() || !wire::MakeRequest(metadata,pending.query,pending.origin,
            std::uint32_t(CL_GetLocalClientConnection(0)->checksumFeed),&request)) { failDownload("metadata session, generation, path, origin, feed or pure contract mismatch"); return; }
        if (pending.submitted) return;
        if (!content::Submit(request)) { failDownload("bounded verified transfer queue rejected metadata"); return; }
        pending.submitted=true; cls.downloadSize=int(metadata.bytes); legacyHacks.cl_downloadSize=int(metadata.bytes);
        ++state->requests;
    } else if (wire::ParseRejection(text,&rejection)) {
        if (rejection.query.id!=pending.query.id || rejection.query.session!=pending.query.session || rejection.query.generation!=pending.query.generation) return;
        if (rejection.error==wire::Error::Busy&&!pending.submitted&&pending.requests<3) return;
        failDownload("custom server rejected requested content metadata");
    } else failDownload("malformed content metadata response");
}
std::uint64_t hashFavorites(khz::browser::Favorites* snapshot) {
    std::uint64_t hash=1469598103934665603ull;
    const unsigned count=unsigned((std::max)(0,(std::min)(cls.numfavoriteservers,128)));
    for (unsigned i=0;i<count;++i) {
        Endpoint address; const auto& server=cls.favoriteServers[i];
        if (!endpoint(server.adr,&address)||!std::memchr(server.hostName,0,sizeof(server.hostName))) continue;
        if (!snapshot->Add(address,server.hostName)) continue;
        for (const auto byte:address.ip) hash=(hash^byte)*1099511628211ull;
        hash=(hash^address.port)*1099511628211ull;
        for (const unsigned char* c=reinterpret_cast<const unsigned char*>(server.hostName);*c;++c) hash=(hash^*c)*1099511628211ull;
    }
    return hash;
}
}
void KHZ_InitServicesClientFeatures() {
    if (state) return;
    state=new(std::nothrow) State; if (!state) return;
    for (auto& epoch:state->epochs) epoch.store(1);
    state->downloadOrigin=Dvar_RegisterString("khz_download_origin","",DVAR_ARCHIVE,"Explicit trusted HTTP(S) origin for custom private-server content; empty disables transfers");
    state->masters=Dvar_RegisterString("khz_master_servers","",DVAR_ARCHIVE,"Explicit private IPv4:port masters separated by semicolons; empty disables global discovery");
    std::filesystem::path stage;
    const std::filesystem::path project(KHZ_ProjectRoot()),home(KHZ_HomeRoot());
    if (!content::ConfinedPath(project,"runtime/content-stage/root-check",&stage,true)
        || !content::ConfinedPath(home,"services/favorites.txt",&state->favoriteFile,true)
        || !content::Start(home,stage.parent_path())) { delete state; state=nullptr; return; }
    state->loadedFavorites.Load(state->favoriteFile); // Startup, before active ticks.
    try { for (auto& thread:state->workers) thread=std::thread(worker); }
    catch (...) {
        state->stopping=true; state->wake.notify_all(); state->available.notify_all();
        for (auto& thread:state->workers) if (thread.joinable()) thread.join();
        content::Stop(); delete state; state=nullptr;
        Com_Printf(CON_CHANNEL_CLIENT,"Client service workers could not start\n");
    }
}
void KHZ_ServicesClientService() {
    if (!state || KHZ_IsClientSimulationThread()) return;
    for (;;) {
        std::array<char,wire::MaximumText+1> text{};
        { std::lock_guard<std::mutex> guard(state->lock); if (state->commandRead==state->commandWrite) break; text=state->commands[state->commandRead++&(CommandCapacity-1)]; }
        command(text.data());
    }
    if (state->download.pending) {
        auto& pending=state->download;
        if (!currentDownload()) { content::Cancel(pending.query.session,pending.query.generation); pending.pending=false; state->downloadPending=false; }
        else if (!pending.submitted&&now()>=pending.nextRequest) {
            if (pending.requests==3) { failDownload("content metadata response timed out"); return; }
            std::string text; if (!wire::FormatQuery(pending.query,&text)) { failDownload("invalid pending content query"); return; }
            CL_AddReliableCommand(0,text.c_str()); ++pending.requests; pending.nextRequest=now()+1000;
        }
    }
    content::Completion completion;
    while (content::Poll(&completion)) {
        const auto& query=state->download.query;
        if (!currentDownload()||completion.request.id!=query.id||completion.request.session!=query.session||completion.request.generation!=query.generation) { content::Discard(completion); continue; }
        ++state->completed;
        if (completion.status!=content::Status::Staged) { failDownload(completion.detail); return; }
        // Initial content admission precedes map load. Active maps cannot enter
        // this branch; root Service must hold its normal lifecycle/FS barrier.
        if (KHZ_ClientOwnerRunning()) { content::Discard(completion); failDownload("gameplay owner must stop before content filesystem commit"); return; }
        if (!content::Commit(completion,query.session,query.generation)) { content::Discard(completion); failDownload("verified content filesystem commit rejected"); return; }
        ++state->committed; state->download.pending=false; state->downloadPending=false;
        cls.downloadRestart=1; // Native completion remounts FS and renegotiates pure state.
        cls.downloadCount=int(completion.bytes); legacyHacks.cl_downloadCount=int(completion.bytes);
        cls.downloadName[0]=0; cls.downloadTempName[0]=0; legacyHacks.cl_downloadName[0]=0;
        CL_NextDownload(0);
    }
    const auto progress=content::GetProgress();
    if (state->download.pending&&progress.id==state->download.query.id) {
        cls.downloadCount=int((std::min)(progress.bytes,std::uint64_t((std::numeric_limits<int>::max)())));
        legacyHacks.cl_downloadCount=cls.downloadCount;
    }
    static_assert(CA_DISCONNECTED==0&&CA_CINEMATIC==1&&CA_LOGO==2&&CA_CONNECTING==3);
    const bool menu=khz::service_control::DisconnectedShell(CL_GetLocalClientConnectionState(0))&&!KHZ_ClientOwnerRunning();
    for (;;) {
        Result result;
        { std::lock_guard<std::mutex> guard(state->lock); if (state->resultRead==state->resultWrite) break; result=state->results[state->resultRead++&(ResultCapacity-1)]; state->available.notify_one(); }
        if (result.kind==ResultKind::FavoriteSaved) { if (!result.success) ++state->favoriteFailures; continue; }
        if (result.source>3 || result.epoch!=state->epochs[result.source].load()) {++state->staleBrowserResults;continue;}
        if (result.kind==ResultKind::Server) for (auto& ping:state->pings)
            if (ping.active&&ping.source==result.source&&ping.epoch==result.epoch&&ping.endpoint==result.endpoint) ping.active=false;
        if (!result.success) ++state->browserFailures;
        if (result.source==3) {
            char address[32]{}; khz::browser::FormatEndpoint(result.endpoint,address,sizeof(address));
            if (result.success) Com_Printf(CON_CHANNEL_CLIENT,"%u ms from %s (%s, protocol %u)\n",result.server.pingMs,address,result.server.hostname,result.server.protocol);
            else Com_Printf(CON_CHANNEL_CLIENT,"Browser ping timed out for %s\n",address);
            continue;
        }
        if (!menu) {++state->shellBrowserDrops;continue;}
        if (result.kind==ResultKind::Endpoint) ++state->masterEndpoints;
        if (result.kind==ResultKind::Finished) { if (result.source==1) { if (state->mastersPending) --state->mastersPending; cls.waitglobalserverresponse=state->mastersPending!=0; CL_SortGlobalServers(); } }
        else publish(result);
    }
    // A master completion can arrive after the stock UI decides its refresh
    // has ended. Keep the current disconnected browser source's dirty work
    // moving; the same bounded per-pass queue/deduplication serves native UI.
    if (menu) KHZ_ServicesClientUpdateDirtyPings(cls.pingUpdateSource);
    if (!menu || !CL_GetLocalClientUIGlobals(0)->isRunning) return;
    if (!state->favoritesImported) {
        if (state->loadedFavorites.Count()) {
            cls.numfavoriteservers=int((std::min)(state->loadedFavorites.Count(),128u));
            for (unsigned i=0;i<unsigned(cls.numfavoriteservers);++i) {
                const auto& favorite=*state->loadedFavorites.At(i); initialize(cls.favoriteServers[i],favorite.address);
                std::snprintf(cls.favoriteServers[i].hostName,sizeof(cls.favoriteServers[i].hostName),"%s",favorite.label);
            }
        }
        state->favoritesImported=true;
    }
    if (now()>=state->nextFavoriteCheck) {
        state->nextFavoriteCheck=now()+1000; khz::browser::Favorites snapshot; const auto hash=hashFavorites(&snapshot);
        if (hash!=state->favoriteHash) {
            std::lock_guard<std::mutex> guard(state->lock);
            state->saveFavorites=snapshot;
            if (state->favoritesPending) state->favoriteHash=hash;
            else if (state->jobWrite-state->jobRead<JobCapacity) {
                state->jobs[state->jobWrite++&(JobCapacity-1)]={JobKind::Favorites,{},2,state->epochs[2].load()};
                state->favoritesPending=true; state->favoriteHash=hash; state->wake.notify_one();
            }
        }
    }
}
void KHZ_ServicesClientDisconnect() {
    if (!state) return;
    if (state->download.pending) content::Cancel(state->download.query.session,state->download.query.generation);
    state->download.pending=false; state->downloadPending=false;
    std::lock_guard<std::mutex> guard(state->lock); state->commandRead=state->commandWrite;
}
void KHZ_ShutdownServicesClientFeatures() {
    if (!state) return;
    khz::browser::Favorites finalFavorites;
    const bool saveFavorites=state->favoritesImported;
    if (saveFavorites) hashFavorites(&finalFavorites);
    KHZ_ServicesClientDisconnect(); state->stopping=true; state->wake.notify_all(); state->available.notify_all();
    for (auto& thread:state->workers) if (thread.joinable()) thread.join();
    // Shutdown is lifecycle work. Persist the final native menu changes after
    // joining writers, including changes made within the one-second UI poll.
    if (saveFavorites&&!finalFavorites.Save(state->favoriteFile)) ++state->favoriteFailures;
    content::Stop(); delete state; state=nullptr;
}
bool KHZ_ServicesClientBeginDownload(const char* localName,const char* remoteName) {
    if (!state || !localName || !remoteName || KHZ_IsClientSimulationThread()) { failDownload("client service unavailable"); return true; }
    if (state->download.pending) { failDownload("overlapping content request"); return true; }
    if (!cl_allowDownload->current.enabled || std::strcmp(localName,remoteName) || !content::SafeAssetPath(localName)
        || std::strlen(localName)>=sizeof(state->download.query.relative)) { failDownload("unsafe or mismatched requested content path"); return true; }
    const auto session=KHZ_AcceptedClientSession(); const auto generation=std::uint32_t(CL_GetLocalClientGlobals(0)->serverId);
    Endpoint address; const auto* origin=state->downloadOrigin->current.string;
    if (!session || !generation || !endpoint(CL_GetLocalClientConnection(0)->serverAddress,&address)
        || !origin || !*origin || std::strlen(origin)>=sizeof(state->download.origin)) { failDownload("accepted private session and explicit khz_download_origin required"); return true; }
    state->download={}; auto& pending=state->download;
    pending.query.id=++state->sequence; pending.query.session=session; pending.query.generation=generation;
    std::strcpy(pending.query.relative,localName); std::strcpy(pending.origin,origin);
    pending.pending=true; state->downloadPending=true; pending.started=now(); pending.nextRequest=now();
    std::snprintf(cls.downloadName,sizeof(cls.downloadName),"%s",localName); cls.downloadTempName[0]=0; cls.wwwDlInProgress=0;
    cls.downloadSize=cls.downloadCount=0; legacyHacks.cl_downloadSize=legacyHacks.cl_downloadCount=0;
    std::snprintf(legacyHacks.cl_downloadName,sizeof(legacyHacks.cl_downloadName),"%s",remoteName);
    legacyHacks.cl_downloadTime=cls.realtime;
    return true;
}
bool KHZ_ServicesClientServerCommand(const char* command) {
    if (!KHZ_ServicesClientIsServerCommand(command)) return false;
    if (!state) return true;
    const auto length=std::strlen(command); if (length>wire::MaximumText) { ++state->rejected; return true; }
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->commandWrite-state->commandRead==CommandCapacity) { ++state->rejected; return true; }
    auto& slot=state->commands[state->commandWrite++&(CommandCapacity-1)]; std::memcpy(slot.data(),command,length+1); ++state->metadataReceived; return true;
}
bool KHZ_ServicesClientIsServerCommand(const char* command){
    if(!command)return false;
    const auto length=strnlen(command,wire::MaximumText+1);
    return length<=wire::MaximumText&&khz::service_control::ContentReliable(std::string_view(command,length));
}
void KHZ_ServicesClientLocalServers() {
    if (!state) return;
    clearSource(0); cls.numlocalservers=0; cls.pingUpdateSource=0;
    pushJob({JobKind::Discover,{},0,state->epochs[0].load()});
    Com_Printf(CON_CHANNEL_CLIENT,"Scanning private LAN and loopback with verified browser challenges...\n");
}
void KHZ_ServicesClientGlobalServers() {
    if (!state) return;
    clearSource(1); state->mastersPending=0; cls.numglobalservers=0; cls.pingUpdateSource=1; cls.waitglobalserverresponse=0;
    std::string_view masters(state->masters->current.string); unsigned count=0;
    while (!masters.empty()&&count<8) {
        const auto split=masters.find(';'); Endpoint address;
        if (!khz::browser::ParseEndpoint(masters.substr(0,split),&address)||!khz::browser::PrivateEndpoint(address)) {
            Com_Printf(CON_CHANNEL_CLIENT,"khz_master_servers requires canonical private IPv4:port entries\n"); return;
        }
        if (pushJob({JobKind::Master,address,1,state->epochs[1].load()})) { ++count; ++state->mastersPending; cls.waitglobalserverresponse=1; }
        if (split==masters.npos) break; masters.remove_prefix(split+1);
    }
    if (!count) Com_Printf(CON_CHANNEL_CLIENT,"Global discovery is disabled; configure khz_master_servers explicitly\n");
}
void KHZ_ServicesClientPing(const char* text) {
    Endpoint address;
    if (!text || !khz::browser::ParseEndpoint(text,&address)||!khz::browser::PrivateEndpoint(address)||!queuePing(address,3))
        Com_Printf(CON_CHANNEL_CLIENT,"ping requires an available private IPv4:port browser query\n");
}
int KHZ_ServicesClientUpdateDirtyPings(int source) {
    if (!state || source<0 || source>2 || !khz::service_control::DisconnectedShell(CL_GetLocalClientConnectionState(0))) return 0;
    int* count=nullptr; unsigned maximum=0; auto* servers=list(unsigned(source),&count,&maximum);
    if (!servers) return 0; *count=(std::max)(0,(std::min)(*count,int(maximum))); cls.pingUpdateSource=source;
    unsigned scheduled=0;
    for (int i=0;i<*count&&scheduled<16;++i) if (servers[i].dirty&&servers[i].ping==-1) {
        Endpoint address; if (endpoint(servers[i].adr,&address)&&queuePing(address,unsigned(source))) ++scheduled;
        else { servers[i].ping=0; servers[i].dirty=0; }
    }
    for (const auto& ping:state->pings) if (ping.active&&ping.source==unsigned(source)) return 1;
    return scheduled!=0;
}
const char* KHZ_ServicesClientStatusJSON() {
    static thread_local char text[1024];
    if (!state) return "{\"available\":false}";
    std::lock_guard<std::mutex> guard(state->lock);
    std::snprintf(text,sizeof(text),"{\"available\":true,\"content_pending\":%s,\"content_requests\":%u,\"content_completed\":%u,\"content_committed\":%u,\"content_rejected\":%u,\"content_metadata_received\":%u,\"content_query_attempts\":%u,\"browser_jobs\":%u,\"browser_active_jobs\":%u,\"browser_results\":%u,\"browser_failures\":%u,\"favorite_failures\":%u,\"browser_master_pending\":%u,\"browser_master_endpoints\":%u,\"browser_stale_results\":%u,\"browser_shell_drops\":%u,\"browser_global_count\":%d,\"browser_favorite_count\":%d,\"native_connection_state\":%d,\"scope\":\"accepted private session; unsigned content metadata; native adapters pending regression\"}",
        state->downloadPending.load()?"true":"false",state->requests.load(),state->completed.load(),state->committed.load(),state->rejected.load(),
        state->metadataReceived.load(),state->download.requests,state->jobWrite-state->jobRead,state->activeBrowserJobs.load(),state->resultWrite-state->resultRead,state->browserFailures.load(),state->favoriteFailures.load(),
        state->mastersPending,state->masterEndpoints,state->staleBrowserResults,state->shellBrowserDrops,cls.numglobalservers,cls.numfavoriteservers,int(CL_GetLocalClientConnectionState(0)));
    return text;
}
