#include <universal/q_shared.h>
#include "khz_equipment_inspection.h"
#include "khz_runtime.h"
#include "khz_client_owner.h"
#include "khz_controls.h"
#include "khz_paths.h"
#include "khz_host.h"
#include "khz_bots.h"
#include "khz_admin_server.h"
#include "khz_identity.h"
#include <equipment_policy.hpp>
#include <unlock_audit_policy.hpp>
#include <client/client.h>
#include <client_mp/client_mp.h>
#include <cgame_mp/cg_local_mp.h>
#include <game_mp/g_main_mp.h>
#include <server_mp/server_mp.h>
#include <qcommon/threads.h>
#include <win32/win_storage.h>
#include <Windows.h>
#include <cstring>
#include <cstdio>

extern const char* bg_perkNames[PERK_COUNT];
namespace {
using namespace khz::equipment;
static_assert(PERK_COUNT==PerkCapacity);
static_assert(sizeof(playerState_s::weapons)==4*sizeof(std::uint32_t));
static_assert(sizeof(playerState_s::weaponmodels)==WeaponCapacity);
static_assert(sizeof(WeaponDef::gunXModel)/sizeof(XModel*)==ModelCapacity);
static_assert(sizeof(WeaponDef::worldModel)/sizeof(XModel*)==ModelCapacity);
char output[JsonCapacity]; Player serverPlayer,acceptedPlayer,predictedPlayer;
KHZ_EquipmentClientFrame clientFrame; // Loader-zeroed BSS; no giant stack copy.
struct Catalogue {
    unsigned count()const{return BG_GetNumWeapons();}
    bool describe(unsigned index,unsigned model,Weapon& output)const {
        // No registering/name-to-index getter. Caller checks both indices before
        // native lookup; catalogue array is immutable for the loaded asset life.
        if(index>=count()||index>=WeaponCapacity||model>=ModelCapacity)return false;
        const auto* definition=BG_GetWeaponDef(index);if(!definition)return false;
        output.alternate=definition->altWeaponIndex;output.inventoryType=static_cast<int>(definition->inventoryType);
        output.offhandClass=static_cast<int>(definition->offhandClass);
        output.ammoIndex=definition->iAmmoIndex;output.clipIndex=definition->iClipIndex;
        if(!copyText(output.name,definition->szInternalName))return false;
        const auto* gun=modelAt(definition->gunXModel,model);const auto* world=modelAt(definition->worldModel,model);
        output.gunModelPresent=gun!=nullptr;output.worldModelPresent=world!=nullptr;
        if(gun&&!copyText(output.gunModel,gun->name))return false;
        if(world&&!copyText(output.worldModel,world->name))return false;
        return true;
    }
};
const char* reject(const char* reason) {
    std::snprintf(output,sizeof(output),"{\"schema\":1,\"complete\":false,\"reason\":\"%s\",\"full_weapon_acceptance\":false,\"objective_acceptance\":false,\"full_world_replay_verified\":false}",reason);return output;
}
void perksJSON(Json& json,unsigned mask){
    json.raw(",\"native_perk_names\":[");bool first=true;
    for(unsigned i=0;i<PerkCapacity;++i)if(mask&(1u<<i)){
        char name[TextCapacity];if(!copyText(name,bg_perkNames[i])){json.raw(nullptr);return;}
        if(!first)json.raw(",");json.number("{\"bit\":%u,\"name\":",i);json.text(name);json.raw("}");first=false;
    }json.raw("]");
}
unsigned stat(const unsigned char* bytes,unsigned index){
    if(index<2000)return bytes[index+4];unsigned value;
    std::memcpy(&value,bytes+index*4-5996,4);return value;
}
bool diagnosticAllowed(){return Dvar_GetBool("khz_benchmark")&&Dvar_GetBool("khz_test_controls")&&KHZ_IsBenchmarkHome();}
void scope(Json& json){json.raw(",\"full_weapon_acceptance\":false,\"objective_acceptance\":false,\"full_world_replay_verified\":false,\"scope\":\"read-only actual native inventory and received/accepted/predicted state; no firing/projectile/damage/complete weapon attestation\"}");}
}

const char* KHZ_EquipmentServerJSON(unsigned slot,std::uint64_t session,std::uint64_t humanGeneration){
#if defined(KISAK_DEDICATED)
    if(!Sys_IsMainThread())return "{\"schema\":1,\"complete\":false,\"reason\":\"requires_actual_dedicated_owner\"}";
    if(!diagnosticAllowed())return reject("requires_benchmark_and_test_controls");
    if(!com_sv_running||!com_sv_running->current.enabled||!svs.initialized||sv.state!=SS_GAME||
        com_inServerFrame||level.initializing||level.clientIsSpawning||!KHZ_ServerTickNumber())return reject("requires_completed_loaded_server_tick");
    if(!sv_maxclients||sv_maxclients->current.integer<1||sv_maxclients->current.integer>16||slot>=static_cast<unsigned>(sv_maxclients->current.integer)||
        !svs.clients||!level.clients||level.gentities!=g_entities||level.maxclients!=sv_maxclients->current.integer)
        return reject("native_client_entity_storage_invalid");
    const auto tick=KHZ_ServerTickNumber();const auto currentSession=KHZ_CurrentServerSession();
    if(!session||currentSession!=session||!humanGeneration||!sv_serverid||!sv_serverid->current.integer)return reject("expected_native_session_mismatch");
    const auto* client=&svs.clients[slot];const auto& entity=g_entities[slot];const auto& game=level.clients[slot];
    const auto* identity=KHZ_AdminClientIdentity(client);
    const Admission admission{currentSession,session,KHZ_AdminClientGeneration(client),humanGeneration,GenerationNamespace::HumanAdmission,
        client->statPacketsReceived,client->header.state==CS_ACTIVE&&!KHZ_IsNativeBot(client)&&game.sess.connected==CON_CONNECTED,
        identity&&!khz::identity::IsEmpty(*identity),client->gentity==&entity&&entity.client==&game&&entity.s.number==static_cast<int>(slot)&&
            entity.s.clientNum==static_cast<int>(slot)&&game.ps.clientNum==static_cast<int>(slot),
        game.sess.sessionState==SESS_STATE_PLAYING,game.sess.cs.team>=TEAM_FREE&&game.sess.cs.team<TEAM_SPECTATOR,entity.health,game.lastSpawnTime};
    if(const auto* reason=admissionFailure(admission))return reject(reason);
    const auto error=read(game.ps,Catalogue{},serverPlayer);if(error!=Failure::None)return reject(failureName(error));
    char identityHash[65];
    if(!khz::identity::EncodeHex(identity->data(),identity->size(),identityHash,sizeof(identityHash)))return reject("native_identity_format_failure");
    unsigned header;std::memcpy(&header,client->stats,4);
    const unsigned checksum=static_cast<unsigned>(LiveStorage_ChecksumGamerStats(const_cast<unsigned char*>(client->stats)+4,8188));
    unsigned classStats[49];for(unsigned i=0;i<49;++i)classStats[i]=stat(client->stats,201+i);
    if(tick!=KHZ_ServerTickNumber()||session!=KHZ_CurrentServerSession()||client->header.state!=CS_ACTIVE||
        KHZ_AdminClientGeneration(client)!=humanGeneration||client->statPacketsReceived!=127)return reject("native_owner_frame_changed");
    Json json(output,sizeof(output));json.number("{\"schema\":1,\"complete\":true,\"reason\":\"none\",\"source\":\"dedicated_authority\",\"process_id\":%lu,\"session\":%llu,\"server_id\":%d,\"tick\":%llu,\"server_time\":%d,\"slot\":%u,\"generation_namespace\":\"human_admission\",\"human_generation\":%llu,\"entity_use_count\":%d,\"last_connect_time\":%d,\"last_spawn_time\":%d,\"connection\":%d,\"team\":%d,\"session_state\":%d,\"health\":%d,\"received_stats_packet_mask\":127,\"received_stats_header\":%u,\"received_stats_crc32\":%u,\"received_stats_fnv1a\":%u,\"identity_fingerprint\":",GetCurrentProcessId(),session,sv_serverid->current.integer,tick,svs.time,slot,humanGeneration,entity.useCount,client->lastConnectTime,game.lastSpawnTime,game.sess.connected,game.sess.cs.team,game.sess.sessionState,entity.health,header,checksum,khz::unlock_audit::fnv(client->stats,sizeof(client->stats)));
    json.text(identityHash);json.number(",\"received_xp\":%u,\"received_class_stats_201_249\":",stat(client->stats,2301));json.array(classStats);
    json.raw(",\"player\":");playerJSON(json,serverPlayer,"authoritative_native_ps");perksJSON(json,serverPlayer.perks);scope(json);
    return json.ok()?output:reject("bounded_json_capacity");
#else
    (void)slot;(void)session;(void)humanGeneration;return reject("requires_dedicated_build");
#endif
}
const char* KHZ_EquipmentClientJSON(std::uint64_t session,std::uint64_t ownerGeneration){
#if !defined(KISAK_DEDICATED)
    if(!Sys_IsMainThread())return "{\"schema\":1,\"complete\":false,\"reason\":\"requires_main_controls_barrier\"}";
    if(!diagnosticAllowed())return reject("requires_benchmark_and_test_controls");
    // The native source helper refuses a frontend mirror or unacknowledged park.
    KHZ_CommandControlScope controls;
    if(!KHZ_ClientOwnerReadEquipment(clientFrame,session,ownerGeneration))return reject("actual_owner_barrier_or_lifetime_invalid");
    const auto first=read(clientFrame.accepted,Catalogue{},acceptedPlayer),second=read(clientFrame.predicted,Catalogue{},predictedPlayer);
    if(first!=Failure::None)return reject(failureName(first));if(second!=Failure::None)return reject(failureName(second));
    Json json(output,sizeof(output));json.number("{\"schema\":1,\"complete\":true,\"reason\":\"none\",\"source\":\"acknowledged_private_owner_barrier\",\"process_id\":%lu,\"session\":%llu,\"server_id\":%d,\"generation_namespace\":\"client_owner\",\"owner_generation\":%llu,\"owner_tick\":%llu,\"accepted_current_message\":%d,\"accepted_next_message\":%d,\"accepted_current_time\":%d,\"accepted_next_time\":%d,\"accepted\":",GetCurrentProcessId(),clientFrame.session,clientFrame.serverId,clientFrame.generation,clientFrame.tick,clientFrame.currentSnapshot,clientFrame.nextSnapshot,clientFrame.currentTime,clientFrame.nextTime);
    playerJSON(json,acceptedPlayer,"accepted_gameplay_next_snapshot");json.raw(",\"predicted\":");playerJSON(json,predictedPlayer,"actual_private_predicted_ps");perksJSON(json,predictedPlayer.perks);scope(json);
    return json.ok()?output:reject("bounded_json_capacity");
#else
    (void)session;(void)ownerGeneration;return reject("requires_rendered_client_build");
#endif
}
bool KHZ_EquipmentReconnectEligible(std::uint64_t session,std::uint64_t ownerGeneration){
#if !defined(KISAK_DEDICATED)
    return Sys_IsMainThread()&&diagnosticAllowed()&&KHZ_ClientOwnerControlsActive()&&
        session&&ownerGeneration&&KHZ_AcceptedClientSession()==session&&KHZ_ClientOwnerGeneration()==ownerGeneration&&
        KHZ_HasOwnedHost()&&KHZ_IsOwnedHostConnection()&&CL_GetLocalClientConnectionState(0)==CA_ACTIVE;
#else
    (void)session;(void)ownerGeneration;return false;
#endif
}
