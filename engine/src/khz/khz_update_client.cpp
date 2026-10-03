#include "khz_update_client.h"
#include "khz_update_bootstrap.h"
#include "khz_update_policy.h"
#include "khz_paths.h"
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace {
namespace update=khz::update;
struct Checked {
    bool success=false,privateFixture=false;
    std::uint64_t version=0,completedMs=0;
    khz::content::Digest manifestHash{};
    char detail[192]{};
};
struct State {
    std::filesystem::path root;
    std::thread thread;
    std::mutex mutex;
    Checked result{};
    std::atomic<bool> running{false},ready{false};
    bool helperStarted=false;
};
std::unique_ptr<State> state;
cmd_function_s checkCommand{},applyCommand{},rollbackCommand{},statusCommand{};
bool noArguments(const char* usage){if(Cmd_Argc()==1)return true;Com_Printf(CON_CHANNEL_CLIENT,"Usage: %s\n",usage);return false;}
Checked snapshot(){if(!state)return {};std::lock_guard<std::mutex> lock(state->mutex);return state->result;}
void Check(){
    if(!state||!noArguments("khz_update_check"))return;
    if(state->helperStarted||state->running.load(std::memory_order_acquire)){Com_Printf(CON_CHANNEL_CLIENT,"An update operation is already active.\n");return;}
    if(state->thread.joinable())state->thread.join();
    const auto root=state->root;
    // The explicit fixture flag is accepted for checking only. Production
    // installation never allows private HTTP or a fixture executable package.
    const bool fixture=Dvar_GetBool("khz_test_controls");
    state->ready.store(false,std::memory_order_release);state->running.store(true,std::memory_order_release);
    {std::lock_guard<std::mutex> lock(state->mutex);state->result={};}
    State* owner=state.get();
    owner->thread=std::thread([owner,root,fixture]{
        Checked result;update::Policy policy;update::Manifest manifest;std::uint64_t installed=0;std::string reason;
        if(!update::LoadPolicy(root,fixture,&policy,&reason))std::snprintf(result.detail,sizeof(result.detail),"%s",reason.c_str());
        else if(!update::Configured(policy))std::snprintf(result.detail,sizeof(result.detail),"Updates are disabled; configure config/update-policy.txt with a trusted project key and HTTPS origin.");
        else if(!update::ReadInstalledVersion(root,&installed))std::snprintf(result.detail,sizeof(result.detail),"Installed version record is invalid.");
        else if(!update::FetchManifest(policy,installed,&manifest))std::snprintf(result.detail,sizeof(result.detail),"Manifest fetch, signature, origin, project or version verification failed.");
        else{std::string body;result.success=update::CanonicalBody(manifest,&body)&&khz::content::Sha256(body.data(),body.size(),&result.manifestHash);result.version=manifest.version;result.privateFixture=policy.privateHttpFixture;std::snprintf(result.detail,sizeof(result.detail),"Verified project update version %llu is available.",static_cast<unsigned long long>(result.version));}
        result.completedMs=GetTickCount64();{std::lock_guard<std::mutex> lock(owner->mutex);owner->result=result;}
        owner->ready.store(true,std::memory_order_release);owner->running.store(false,std::memory_order_release);
    });
    Com_Printf(CON_CHANNEL_CLIENT,"Checking the configured trusted project update off the gameplay thread.\n");
}
void Apply(){
    if(!state||!noArguments("khz_update_apply"))return;
    const auto checked=snapshot();
    if(state->running.load(std::memory_order_acquire)||state->helperStarted||!checked.success||GetTickCount64()-checked.completedMs>60000){Com_Printf(CON_CHANNEL_CLIENT,"Run khz_update_check and obtain a verified update within 60 seconds before applying.\n");return;}
    if(checked.privateFixture){Com_Printf(CON_CHANNEL_CLIENT,"Private HTTP fixtures can install only into a confined results fixture root using the standalone helper.\n");return;}
    std::string reason;
    if(!update::SpawnHelper(state->root,"apply",checked.version,true,&reason,&checked.manifestHash)){Com_PrintError(CON_CHANNEL_CLIENT,"Update: %s\n",reason.c_str());return;}
    state->helperStarted=true;
    Com_Printf(CON_CHANNEL_CLIENT,"Closing the game; the helper will wait for every project game to exit, install the signed pair and restart with these settings.\n");
    Cbuf_AddText(0,"quit\n");
}
void Rollback(){
    if(!state||!noArguments("khz_update_rollback"))return;
    if(state->running.load(std::memory_order_acquire)||state->helperStarted){Com_Printf(CON_CHANNEL_CLIENT,"An update operation is already active.\n");return;}
    update::Policy policy;std::string reason;
    if(!update::LoadPolicy(state->root,false,&policy,&reason)||!update::Configured(policy)){Com_PrintError(CON_CHANNEL_CLIENT,"Rollback: %s\n",reason.empty()?"Trusted production update policy is unconfigured":reason.c_str());return;}
    if(!update::SpawnHelper(state->root,"rollback",0,true,&reason)){Com_PrintError(CON_CHANNEL_CLIENT,"Rollback: %s\n",reason.c_str());return;}
    state->helperStarted=true;Com_Printf(CON_CHANNEL_CLIENT,"Closing the game; the helper will verify and restore the saved binary set, then restart.\n");Cbuf_AddText(0,"quit\n");
}
void Status(){if(!noArguments("khz_update_status"))return;const auto checked=snapshot();Com_Printf(CON_CHANNEL_CLIENT,"Update worker=%s, verified version=%llu, helper=%s. %s\n",state&&state->running.load()?"running":"idle",static_cast<unsigned long long>(checked.version),state&&state->helperStarted?"started":"idle",checked.detail);}
}
void KHZ_InitUpdateClientFeatures(){
#ifndef KISAK_DEDICATED
    if(state)return;state=std::make_unique<State>();state->root=std::filesystem::path(KHZ_ProjectRoot());
    Cmd_AddCommandInternal("khz_update_check",Check,&checkCommand);Cmd_AddCommandInternal("khz_update_apply",Apply,&applyCommand);
    Cmd_AddCommandInternal("khz_update_rollback",Rollback,&rollbackCommand);Cmd_AddCommandInternal("khz_update_status",Status,&statusCommand);
#endif
}
void KHZ_UpdateClientService(){if(!state||!state->ready.exchange(false,std::memory_order_acq_rel))return;const auto checked=snapshot();Com_Printf(CON_CHANNEL_CLIENT,"%s%s\n",checked.detail,checked.success&&!checked.privateFixture?" Use khz_update_apply to install and restart.":"");}
void KHZ_ShutdownUpdateClientFeatures(){if(!state)return;if(state->thread.joinable())state->thread.join();state.reset();}
const char* KHZ_UpdateClientStatusJSON(){thread_local char text[384];const auto checked=snapshot();std::snprintf(text,sizeof(text),"{\"configured_check_passed\":%s,\"checking\":%s,\"verified_version\":%llu,\"private_fixture\":%s,\"helper_started\":%s,\"native_regression_verified\":false}",checked.success?"true":"false",state&&state->running.load()?"true":"false",static_cast<unsigned long long>(checked.version),checked.privateFixture?"true":"false",state&&state->helperStarted?"true":"false");return text;}
