// Source-owned: END khz_host.cpp, opt-in dedicated only. This staged owner is
// not enabled until every native provider and pre-effect hook is linked.
#include "khz_server_checkpoint_owner.h"
#include "khz_server_checkpoint_native.h"
#include "khz_server_world_checkpoint.h"
#include "khz_server_schedule_checkpoint.h"
#include "khz_server_runtime_checkpoint.h"
#include "khz_server_network_checkpoint.h"
#include "khz_server_admin_checkpoint.h"
#include "khz_server_animation_checkpoint.h"
#include "khz_server_collision_checkpoint.h"
#include "khz_server_physics_checkpoint.h"
#include "khz_server_dvars_checkpoint.h"
#include "khz_bots_checkpoint.h"
#include <game_mp/g_public_mp.h>
#include <script/scr_main.h>
#include <script/scr_memorytree.h>
#include <script/scr_variable.h>

namespace khz_server_checkpoint_owner_detail {
using namespace khz::server_native_checkpoint;
bool registered, mapBound, physicsHeld, collisionHeld;
unsigned mapGeneration;
int mapServerId;
unsigned mapChecksum;

bool releaseEmptyFences(char* text, unsigned capacity) {
    if (!Sys_IsMainThread() || KHZ_ServerCheckpointPinned())
        return reject(text, capacity, "server_checkpoint_native_fences_require_all_provider_cleanup");
    // Coordinator discard has already finished every provider. Keep both
    // gates closed until that point; neither provider discard reopens a gate.
    if (collisionHeld) { KHZ_ServerCollisionFenceRelease(); collisionHeld = false; }
    if (physicsHeld) {
        if (!KHZ_ServerPhysicsFenceRelease(text, capacity)) return false;
        physicsHeld = false;
    }
    return true;
}
bool observe(std::uint64_t fixture, KHZ_ServerCheckpointFrame* frame, char* text, unsigned capacity) {
    if (!Sys_IsMainThread() || !frame || !registered || !mapBound || !fixture ||
        !host.initialized || !host.managedServer || !host.authenticated || !host.configured ||
        host.phase != Phase::Ready || host.pendingReady || host.quitQueued ||
        !host.checkpointPaused || fixture != host.checkpointFixture ||
        !host.channel.connected || !host.channel.peerVerified || host.channel.failed || host.parentPid == 0 ||
        !Dvar_GetBool("khz_benchmark") || !com_dedicated || !com_dedicated->current.integer ||
        !com_sv_running || !com_sv_running->current.enabled || com_inServerFrame ||
        sv.state != SS_GAME || sv.inFrame || sv.restarting || sv.killServer ||
        !svs.initialized || !svs.clients || !sv_maxclients || sv_maxclients->current.integer != 16 ||
        host.checkpointSession != KHZ_CurrentServerSession() || !KHZ_CurrentServerSession() ||
        mapGeneration != host.transitions || mapServerId != sv_serverId_value || mapChecksum != cm.checksum ||
        !sv_mapname || I_stricmp(sv_mapname->current.string, host.configuration.map) ||
        I_stricmp(Dvar_GetString("g_gametype"), host.configuration.gametype) ||
        std::strstr(KHZ_PluginStatsJSON(), "\"loaded\":0") == nullptr || Dvar_GetBool("sv_webadmin") ||
        !NET_CheckpointQueuesQuiescent(NS_SERVER))
        return reject(text, capacity, "server_checkpoint_actual_authenticated_owner_map_pause_or_idle_fence_invalid");
    unsigned humans = 0;
    for (int i = 0; i < sv_maxclients->current.integer; ++i) {
        const auto& client = svs.clients[i];
        if (client.header.state < CS_CONNECTED || KHZ_IsNativeBot(&client)) continue;
        ++humans;
        const auto& channel = client.header.netchan;
        const khz::host_checkpoint::Channel observed{channel.fragmentLength, channel.unsentLength,
            channel.unsentBufferSize, channel.unsentFragments != 0, client.header.state == CS_ACTIVE,
            client.downloading != 0, client.download != 0, client.downloadingWWW != 0};
        if (khz::host_checkpoint::channelFailure(observed) != khz::host_checkpoint::ChannelFailure::None ||
            !((channel.remoteAddress.type == NA_IP && channel.remoteAddress.ip[0] == 127 && channel.remoteAddress.ip[1] == 0 && channel.remoteAddress.ip[2] == 0 && channel.remoteAddress.ip[3] == 1) || channel.remoteAddress.type == NA_LOOPBACK))
            return reject(text, capacity, "server_checkpoint_requires_one_active_owned_local_human_without_fragment_or_download");
    }
    if (humans != 1 || !scrVarPub.programBuffer || !scrMemTreePub.mt_buffer)
        return reject(text, capacity, "server_checkpoint_native_human_or_script_arena_missing");
    *frame = {};
    frame->fixture = fixture; frame->session = host.checkpointSession;
    frame->mapGeneration = host.transitions;
    // The unique fixture token was acknowledged on the verified private host
    // channel. It remains the same fence identity across branch pause/resume.
    frame->fenceGeneration = host.checkpointFixture;
    frame->logicalTick = KHZ_ServerTickNumber(); frame->nativeTime = svs.time;
    frame->serverId = sv_serverId_value; frame->mapChecksum = cm.checksum;
    frame->serverAddress = reinterpret_cast<std::uintptr_t>(&sv);
    frame->clientsAddress = reinterpret_cast<std::uintptr_t>(svs.clients);
    frame->entitiesAddress = reinterpret_cast<std::uintptr_t>(g_entities);
    frame->scriptProgramAddress = reinterpret_cast<std::uintptr_t>(scrVarPub.programBuffer);
    frame->mtBufferAddress = reinterpret_cast<std::uintptr_t>(scrMemTreePub.mt_buffer);
    if (!KHZ_ServerCheckpointObserveHunk(frame, text, capacity)) return false;
    const bool physicsIdle = KHZ_ServerPhysicsFenceEnter(*frame, text, capacity);
    // Enter may close admission before reporting a pending refusal. Preserve
    // that exact gate ownership for explicit Empty cleanup on capture failure.
    physicsHeld = physicsHeld || KHZ_ServerPhysicsFenceCommitHeld(*frame);
    if (!physicsIdle) return false;
    collisionHeld = true; // Enter publishes closure even if an active query refuses it.
    if (!KHZ_ServerCollisionFenceEnter(text, capacity)) return false;
    return KHZ_ServerPhysicsFenceIdle(*frame, text, capacity);
}
}
void KHZ_ServerCheckpointRegisterNativeOwner() {
    using namespace khz_server_checkpoint_owner_detail;
    if (!Sys_IsMainThread() || registered || !KHZ_InitServerCheckpoint(observe))
        Com_Error(ERR_FATAL, "Native server checkpoint owner initialization failed");
    KHZ_ServerScriptVMRegisterCheckpoint(); KHZ_ServerScriptVariablesRegisterCheckpoint();
    KHZ_ServerMTRegisterCheckpoint(); KHZ_ServerSLRegisterCheckpoint();
    KHZ_ServerEntHandlesRegisterCheckpoint(); KHZ_ServerDObjRegisterCheckpoint();
    KHZ_ServerWorldMainRegisterCheckpoint(); KHZ_ServerWorldScriptRegisterCheckpoint();
    KHZ_ServerWorldHudRegisterCheckpoint(); KHZ_ServerWorldTurretsRegisterCheckpoint();
    KHZ_ServerWorldVehiclesRegisterCheckpoint(); KHZ_ServerWorldMissilesRegisterCheckpoint();
    KHZ_ServerWorldMoversRegisterCheckpoint(); KHZ_ServerWorldVehiclePhysicsRegisterCheckpoint();
    KHZ_ServerScheduleRegisterCheckpoint(); KHZ_ServerRandomRegisterCheckpoint();
    KHZ_ServerNetworkRegisterCheckpoint(); KHZ_ServerSnapshotRegisterCheckpoint(); KHZ_ServerTransportRegisterCheckpoint();
    KHZ_ServerRuntimeRegisterCheckpoint(); KHZ_BotRegisterServerCheckpoint(); KHZ_ServerAdminRegisterCheckpoint();
    KHZ_ServerAnimationRegisterCheckpoint();
    KHZ_ServerCollisionLoadRegisterCheckpoint(); KHZ_ServerCollisionWorldRegisterCheckpoint(); KHZ_ServerCollisionQueryRegisterCheckpoint();
    KHZ_ServerCollisionEnrollQueryHooks(KHZ_ServerCollisionRequiredParts);
    KHZ_ServerPhysicsRegisterCheckpoint(); KHZ_ServerODERegisterCheckpoint(); KHZ_ServerODECollidersRegisterCheckpoint();
    KHZ_ServerODETimersRegisterCheckpoint(); KHZ_ServerPhysicsCapsuleRegisterCheckpoint(); KHZ_ServerODEErrorsRegisterCheckpoint();
    KHZ_ServerODEQuickstepRegisterCheckpoint(); KHZ_ServerPhysicsStepsRegisterCheckpoint();
    KHZ_ServerDvarsRegisterCheckpoint(); registered = true;
}
bool KHZ_ServerCheckpointBindNativeMap(char* text, unsigned capacity) {
    using namespace khz_server_checkpoint_owner_detail;
    if (!Sys_IsMainThread() || !registered || !host.managedServer || !host.authenticated ||
        host.phase != Phase::Ready || com_inServerFrame || sv.state != SS_GAME ||
        KHZ_ServerCheckpointPinned() || !releaseEmptyFences(text, capacity))
        return reject(text, capacity, "server_checkpoint_map_bind_requires_completed_unpinned_native_owner");
    if (mapBound && mapGeneration == host.transitions && mapServerId == sv_serverId_value && mapChecksum == cm.checksum) return true;
    mapBound = false;
    if (!KHZ_ServerCheckpointResetMapRanges() || !KHZ_ServerAnimationBindMapTrees(text, capacity) ||
        !KHZ_ServerCollisionBindMap(text, capacity)) {
        KHZ_ServerCheckpointResetMapRanges(); return false;
    }
    mapGeneration = host.transitions; mapServerId = sv_serverId_value; mapChecksum = cm.checksum;
    mapBound = true; return true;
}
bool KHZ_ServerNativeCheckpointCapture(std::uint64_t fixture) {
    using namespace khz_server_checkpoint_owner_detail;
    if (!Sys_IsMainThread()) return false;
    const bool captured = KHZ_ServerCheckpointCapture(fixture);
    if (!captured && !KHZ_ServerCheckpointPinned()) {
        char reason[256]{};
        if (!releaseEmptyFences(reason, sizeof(reason))) Com_Error(ERR_FATAL, "Native checkpoint capture cleanup: %s", reason);
    }
    return captured;
}
bool KHZ_ServerNativeCheckpointValidateRestore() {
    return Sys_IsMainThread() && KHZ_ServerCheckpointValidateRestore();
}
bool KHZ_ServerNativeCheckpointRestore() {
    return Sys_IsMainThread() && KHZ_ServerCheckpointRestore();
}
bool KHZ_ServerNativeCheckpointDiscard() {
    using namespace khz_server_checkpoint_owner_detail;
    if (!Sys_IsMainThread() || !KHZ_ServerCheckpointDiscard()) return false;
    char reason[256]{};
    if (!releaseEmptyFences(reason, sizeof(reason))) Com_Error(ERR_FATAL, "Native checkpoint discard cleanup: %s", reason);
    return true;
}
bool KHZ_ServerCheckpointPrepareNativeResume(char* text, unsigned capacity) {
    if (!Sys_IsMainThread() || !KHZ_ServerCheckpointPaused() || com_inServerFrame)
        return khz::server_native_checkpoint::reject(text, capacity, "server_checkpoint_resume_requires_actual_authenticated_pause");
    // A pinned replay branch retains native fences. Discard is the only path
    // that opens them, after every provider has released its saved image.
    return KHZ_ServerRuntimeResumeCheckpointClock(text, capacity);
}
