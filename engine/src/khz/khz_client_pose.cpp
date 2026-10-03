#include "khz_client_pose.h"
#include "khz_client_body.h"
#include "khz_client_owner.h"
#include "khz_client_visibility.h"
#include "khz_client_projectile.h"
#include "khz_collision.h"
#include "khz_client_checkpoint.h"
#include "khz_canonical.h"
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <client_mp/client_mp.h>
#include <cgame_mp/cg_local_mp.h>
#include <cgame/cg_local.h>
#include <bgame/bg_local.h>
#include <aim_assist/aim_assist.h>
#include <xanim/dobj.h>
#include <xanim/xanim.h>
#include <xanim/xmodel.h>
#include <script/scr_stringlist.h>
#include <cstring>
#include <cstdio>

bool KHZ_ClientPoseCanonical(khz::canonical::Hash& hash);

namespace {
constexpr unsigned PlayerCapacity = 64, BodyCapacity = 128, AnimationCapacity = 2048;
struct ModelMaps {
    XModelNameMap names[256];
    unsigned count;
    struct Record { const XAnimParts* parts; unsigned string; } records[AnimationCapacity];
};
struct PlayerPose {
    bool valid, initialized;
    const DObj_s* body;
    DObj_s object;
    alignas(void*) unsigned char modelStorage[7 * (sizeof(void*) + 1)];
    XAnimTree_s tree;
};
struct PoseDomain {
    bool prepared;
    cg_s* game;
    centity_s* entities;
    PlayerPose players[PlayerCapacity];
    ModelMaps maps[BodyCapacity];
    unsigned mapCount;
    const char* failure;
};
PoseDomain poses; // loader-zeroed POD, no multi-megabyte CRT temporary
PoseDomain checkpointPoses;
unsigned checkpointPlayerPins, checkpointMapPins[BodyCapacity];
bool checkpointCaptured;

const clientState_s* metadata(const snapshot_s* snapshot, unsigned client) {
    for (int i = 0; i < snapshot->numClients; ++i)
        if (snapshot->clients[i].clientIndex == client) return &snapshot->clients[i];
    return nullptr;
}
bool cacheMaps(const DObj_s& object) {
    XModelNameMap names[256]{};
    XAnimInitModelMap(object.models, object.numModels, names);
    for (unsigned i = 0; i < poses.mapCount; ++i)
        if (!std::memcmp(names, poses.maps[i].names, sizeof(names))) return true;
    if (poses.mapCount == BodyCapacity || !object.tree || !object.tree->anims ||
        object.tree->anims->size > AnimationCapacity) { poses.failure = "private_animation_model_map_capacity_or_tree_invalid"; return false; }
    auto& cache = poses.maps[poses.mapCount++];
    cache.count = 0; std::memcpy(cache.names, names, sizeof(names));
    const auto* animations = object.tree->anims;
    for (unsigned i = 0; i < animations->size; ++i) {
        if (!IsLeafNode(&animations->entries[i])) continue;
        const auto* parts = animations->entries[i].parts;
        bool duplicate = false;
        for (unsigned j = 0; j < cache.count; ++j)
            if (cache.records[j].parts == parts) { duplicate = true; break; }
        if (duplicate) continue;
        if (cache.count == AnimationCapacity) { poses.failure = "private_animation_parts_capacity_exhausted"; return false; }
        cache.records[cache.count++] = {parts, XAnimGetAnimMap(parts, cache.names)};
    }
    return true;
}
bool acceptsPlayer(const snapshot_s* snapshot, const entityState_s& state) {
    if (state.eType != ET_PLAYER || (state.lerp.eFlags & EF_DEAD)) return true;
    if (state.number >= PlayerCapacity || state.clientNum != state.number ||
        (state.lerp.eFlags & EF_TURRET_ACTIVE) || state.lerp.pos.trType == TR_PHYSICS ||
        CG_IsRagdollTrajectory(&state.lerp.pos)) { poses.failure = "accepted_player_requires_private_turret_vehicle_or_ragdoll_pose"; return false; }
    const auto* info = metadata(snapshot, state.clientNum);
    if (!info || info->attachedVehEntNum != ENTITYNUM_NONE || !KHZ_ClientBodyForClient(info)) {
        poses.failure = "accepted_player_composition_or_vehicle_is_not_prepared"; return false;
    }
    return true;
}
void releaseObject(PlayerPose& slot) {
    if (slot.object.duplicateParts)
        SL_RemoveRefToStringOfSize(slot.object.duplicateParts, slot.object.duplicatePartsSize);
    slot.object = {}; slot.body = nullptr;
}
void bindDescriptor(PlayerPose& slot, const DObj_s* body, unsigned entity) {
    if (slot.body != body) {
        releaseObject(slot);
        slot.body = body; slot.object = *body;
        std::memcpy(slot.modelStorage, body->models, body->numModels * (sizeof(void*) + 1));
        slot.object.models = reinterpret_cast<XModel**>(slot.modelStorage);
        slot.object.entnum = static_cast<short>(entity + 1);
        slot.object.skel = {}; slot.object.locked = 0;
        if (slot.object.duplicateParts) SL_AddRefToString(slot.object.duplicateParts);
        slot.object.tree = &slot.tree;
        if (slot.tree.children) XAnimResetAnimMap(&slot.object, slot.tree.children);
    }
    slot.valid = true;
}
bool bindPlayer(const snapshot_s* snapshot, centity_s& entity) {
    const auto& state = entity.nextState;
    const auto* info = metadata(snapshot, state.clientNum);
    const auto* body = KHZ_ClientBodyForClient(info);
    if (!body) return false;
    auto& slot = poses.players[state.number];
    auto& client = poses.game->bgs.clientinfo[state.clientNum];
    const bool newlyPresent = !slot.initialized;
    const bool teleported = slot.initialized && ((entity.currentState.eFlags ^ state.lerp.eFlags) & 2);
    const bool modelChanged = slot.body != body;
    bindDescriptor(slot, body, state.number);
    if (newlyPresent || modelChanged) std::memset(entity.pose.player.tag, 254, sizeof(entity.pose.player.tag));
    slot.initialized = true;
    client.pXAnimTree = &slot.tree;
    I_strncpyz(client.model, body->models[0]->name, sizeof(client.model));
    client.iDObjWeapon = (state.lerp.eFlags & 0x300) ? 0 : state.weapon;
    client.weaponModel = state.weaponModel; client.dobjDirty = false;
    entity.pose.localClientNum = 0; entity.pose.eType = ET_PLAYER;
    entity.pose.player.control = &client.control;
    if (newlyPresent) {
        // Initialize from accepted gameplay state; never import render-advanced
        // animation weights, controller phases, or skeleton matrices.
        client.legs = {}; client.torso = {}; client.control = {};
        std::memset(client.clientConditions, 0, sizeof(client.clientConditions));
        client.leftHandGun = 0; client.turnAnimEndTime = 0; client.turnAnimType = 0;
        client.stanceTransitionTime = 0; client.hideWeapon = false; client.usingKnife = false;
    }
    if (newlyPresent || teleported) {
        CG_CalcEntityLerpPositions(0, &entity);
        CG_ResetPlayerEntity(0, poses.game, &entity, 1);
    }
    return true;
}
}

bool KHZ_ClientPosePrepare(cg_s* game, cgs_t* gameStatic, centity_s* entities, const snapshot_s* accepted) {
    iassert(Sys_IsMainThread()); iassert(!KHZ_IsClientSimulationThread());
    if (poses.prepared || !game || !gameStatic || !entities || !accepted) return false;
    poses.failure = "";
    poses.game = game; poses.entities = entities;
    KHZ_XAnimOwnerReset();
    KHZ_ClientPoseResetAimTargets();
    auto* animations = game->bgs.animScriptData.animTree.anims;
    if (!KHZ_ClientBodyPrepare(gameStatic, animations, accepted)) { KHZ_ClientPoseRelease(); return false; }
    for (unsigned i = 0; i < KHZ_ClientBodyCount(); ++i)
        if (!cacheMaps(*KHZ_ClientBodyAt(i))) { KHZ_ClientPoseRelease(); return false; }
    for (unsigned i = 0; i < PlayerCapacity; ++i) {
        poses.players[i].tree = {}; poses.players[i].tree.anims = animations;
        game->bgs.clientinfo[i].pXAnimTree = &poses.players[i].tree;
    }
    game->bgs.GetDObj = [](unsigned handle, int localClientNum) -> DObj_s* {
        iassert(localClientNum == 0); return KHZ_ClientPoseGetDObj(handle);
    };
    poses.prepared = true;
    if (!KHZ_ClientProjectilePrepare(game, entities)) { KHZ_ClientPoseRelease(); return false; }
    if (!KHZ_ClientPoseAccepts(accepted)) { KHZ_ClientPoseRelease(); return false; }
    // Active bolted effects require a private descriptor during migration.
    // Binding immutable models here does not initialize animation/controller
    // phases: the first worker acceptance still performs native gameplay reset.
    for (unsigned i = 0; i < PlayerCapacity; ++i) {
        const auto& entity = entities[i];
        if (!entity.nextValid || entity.nextState.eType != ET_PLAYER || (entity.nextState.lerp.eFlags & EF_DEAD)) continue;
        const auto* body = KHZ_ClientBodyForClient(metadata(accepted, entity.nextState.clientNum));
        if (body) bindDescriptor(poses.players[i], body, i);
    }
    return true;
}
bool KHZ_ClientPoseRefresh(cgs_t* gameStatic, const snapshot_s* accepted) {
    iassert(Sys_IsMainThread());
    if (!poses.prepared || !KHZ_ClientBodyRefresh(gameStatic, accepted) || !KHZ_ClientProjectileRefresh()) return false;
    for (unsigned i = 0; i < KHZ_ClientBodyCount(); ++i)
        if (!cacheMaps(*KHZ_ClientBodyAt(i))) return false;
    return KHZ_ClientPoseAccepts(accepted);
}
const char* KHZ_ClientPoseFailureReason() {
    if (KHZ_ClientBodyFailureReason()[0]) return KHZ_ClientBodyFailureReason();
    if (KHZ_ClientProjectileFailureReason()[0]) return KHZ_ClientProjectileFailureReason();
    return poses.failure ? poses.failure : "";
}
void KHZ_ClientPoseRelease() {
    iassert(Sys_IsMainThread());
    for (auto& slot : poses.players) {
        if (slot.tree.anims && slot.tree.children) KHZ_XAnimOwnerClearTree(&slot.tree);
        releaseObject(slot); slot.valid = slot.initialized = false; slot.tree = {};
    }
    for (unsigned i = 0; i < poses.mapCount; ++i) {
        auto& cache = poses.maps[i];
        for (unsigned j = 0; j < cache.count; ++j) {
            const auto string = cache.records[j].string;
            const auto* value = SL_ConvertToString(string);
            SL_RemoveRefToStringOfSize(string, static_cast<unsigned char>(value[16]) + 17);
        }
        cache.count = 0;
    }
    poses.mapCount = 0; poses.prepared = false; poses.game = nullptr; poses.entities = nullptr;
    KHZ_ClientBodyRelease();
    KHZ_ClientProjectileRelease();
}
bool KHZ_ClientPoseAccepts(const snapshot_s* snapshot) {
    if (!poses.prepared || !snapshot) return false;
    poses.failure = "";
    if (!KHZ_ClientProjectileAccepts(snapshot)) return false;
    for (int i = 0; i < snapshot->numEntities; ++i)
        if (!acceptsPlayer(snapshot, snapshot->entities[i])) return false;
    if ((snapshot->ps.otherFlags & 6) && snapshot->ps.pm_type < PM_DEAD) {
        auto localState = snapshot->ps;
        entityState_s local{}; BG_PlayerStateToEntityState(&localState, &local, 0, 0);
        local.number = static_cast<unsigned short>(snapshot->ps.clientNum);
        if (!acceptsPlayer(snapshot, local)) return false;
    }
    return true;
}
bool KHZ_ClientPoseAccept(const snapshot_s* snapshot) {
    iassert(KHZ_IsClientSimulationThread());
    if (!KHZ_ClientPoseAccepts(snapshot)) return false;
    bool present[PlayerCapacity]{};
    for (unsigned i = 0; i < PlayerCapacity; ++i) {
        auto& entity = poses.entities[i];
        if (!entity.nextValid || entity.nextState.eType != ET_PLAYER || (entity.nextState.lerp.eFlags & EF_DEAD)) continue;
        if (!bindPlayer(snapshot, entity)) return false;
        present[i] = true;
    }
    for (unsigned i = 0; i < PlayerCapacity; ++i) if (!present[i] && poses.players[i].valid) {
        XAnimClearTree(&poses.players[i].tree); releaseObject(poses.players[i]); poses.players[i].valid = poses.players[i].initialized = false;
    }
    return true;
}
DObj_s* KHZ_ClientPoseGetDObj(unsigned handle) {
    if (poses.prepared && handle < PlayerCapacity && poses.players[handle].valid) return &poses.players[handle].object;
    return KHZ_ClientProjectileGetDObj(handle);
}
bool KHZ_ClientPoseOwnsDObj(const DObj_s* object) {
    if (!KHZ_IsClientSimulationThread() || !poses.prepared || !object) return false;
    for (const auto& slot : poses.players) if (slot.valid && object == &slot.object) return true;
    return KHZ_ClientProjectileOwnsDObj(object);
}
std::uint32_t KHZ_ClientPoseCachedAnimMap(const XAnimParts* parts, const XModelNameMap* map) {
    iassert(KHZ_IsClientSimulationThread());
    for (unsigned i = 0; i < poses.mapCount; ++i) {
        const auto& cache = poses.maps[i];
        if (!std::memcmp(cache.names, map, sizeof(cache.names))) {
            for (unsigned j = 0; j < cache.count; ++j) if (cache.records[j].parts == parts) {
                SL_AddRefToString(cache.records[j].string); return cache.records[j].string;
            }
            break;
        }
    }
    Com_Error(ERR_DROP, "Client owner animation/model map was not prepared under the asset barrier"); return 0;
}
bool KHZ_ClientPoseTick() {
    iassert(KHZ_IsClientSimulationThread());
    if (!poses.prepared || !poses.game->snap || !poses.game->nextSnap || !KHZ_HasPredictionCollisionFrame()) return false;
    auto& game = *poses.game; iassert(bgs == &game.bgs);
    auto& client = CL_GetLocalClientGlobals(0)[0];
    client.skelMemPos = 0;
    client.skelMemoryStart = reinterpret_cast<char*>((reinterpret_cast<std::uintptr_t>(client.skelMemory) + 15) & ~std::uintptr_t(15));
    ++client.skelTimeStamp;
    if (game.predictedPlayerState.eFlags & EF_TURRET_ACTIVE) return false;
    // Gameplay eye, command view angles and ADS FOV are deterministic native
    // player-state calculations. Visual camera shake/bob has a separate owner.
    BG_GetPlayerViewOrigin(&game.predictedPlayerState, game.refdef.vieworg, game.time);
    Vec3Copy(game.predictedPlayerState.viewangles, game.refdefViewAngles);
    AnglesToAxis(game.refdefViewAngles, game.refdef.viewaxis);
    Vec3Copy(game.predictedPlayerState.origin, game.refdef.viewOffset);
    CalculateWeaponPostion_PositionToADS(&game, &game.predictedPlayerState);
    CG_CalcFov(0);
    // Local prediction angles/conditions advance once, at the existing end of
    // prediction stage. This registration only suppresses a second advance
    // when the native private DObj updates its animation/tree below.
    BG_RegisterPredictionAnimationOwner(static_cast<unsigned>(game.predictedPlayerState.clientNum));
    for (unsigned i = 0; i < PlayerCapacity; ++i) {
        auto& entity = poses.entities[i];
        auto* object = KHZ_ClientPoseGetDObj(i);
        if (!object || !entity.nextValid) continue;
        CG_CalcEntityLerpPositions(0, &entity);
        BG_PlayerAnimation(0, &entity.nextState, &game.bgs.clientinfo[i]);
        CG_Player_PreControllers(object, &entity);
        DObjUpdateClientInfo(object, .001f, false);
    }
    // Native smoke/FX particle opacity advances before this tick's aim queries,
    // including during arbitrarily skipped presentation frames.
    if (!KHZ_ClientProjectileTick() || !KHZ_ClientVisibilityTick()) return false;
    AimTarget_ClearTargetList(0);
    for (int i = 0; i < game.nextSnap->numEntities; ++i) {
        const auto& state = game.nextSnap->entities[i];
        auto& entity = poses.entities[state.number];
        if (!entity.nextValid || state.number == game.predictedPlayerState.clientNum || (state.lerp.eFlags & (EF_DEAD | 0x20))) continue;
        if (state.eType == ET_PLAYER) {
            if (!KHZ_ClientPoseGetDObj(state.number)) return false;
            AimTarget_ProcessEntity(0, &entity);
        } else if (state.solid == SOLID_BMODEL && (state.lerp.eFlags & EF_AIM_ASSIST)) {
            CG_CalcEntityLerpPositions(0, &entity); AimTarget_ProcessEntity(0, &entity);
        }
    }
    AimTarget_UpdateClientTargets(0);
    AimAssist_UpdateScreenTargets(0, game.refdef.vieworg, game.refdefViewAngles, game.refdef.tanHalfFovX, game.refdef.tanHalfFovY);
    return !KHZ_ClientOwnerNeedsMainThread();
}

namespace {
bool poseCheckpointReject(char* reason, unsigned capacity, const char* text) {
    if (reason && capacity) std::snprintf(reason, capacity, "%s", text); return false;
}
bool poseCheckpointReady(char* reason, unsigned capacity) {
    if (!KHZ_ClientOwnerCheckpointContext() || !poses.prepared || poses.mapCount > BodyCapacity)
        return poseCheckpointReject(reason, capacity, "pose_checkpoint_requires_prepared_parked_worker");
    khz::canonical::Hash hash;
    return KHZ_ClientPoseCanonical(hash) && hash.finish().valid ? true :
        poseCheckpointReject(reason, capacity, "pose_checkpoint_native_tree_controller_or_skeleton_invalid");
}
void poseCheckpointDiscard() {
    for (unsigned i = 0; i < checkpointPlayerPins; ++i) {
        const auto& object = checkpointPoses.players[i].object;
        if (object.duplicateParts) SL_RemoveRefToStringOfSize(object.duplicateParts, object.duplicatePartsSize);
    }
    checkpointPlayerPins = 0;
    for (unsigned i = 0; i < BodyCapacity; ++i) {
        for (unsigned j = 0; j < checkpointMapPins[i]; ++j) {
            const auto string = checkpointPoses.maps[i].records[j].string;
            const auto* bytes = reinterpret_cast<const unsigned char*>(SL_ConvertToString(string));
            SL_RemoveRefToStringOfSize(string, bytes[16] + 17);
        }
        checkpointMapPins[i] = 0;
    }
    checkpointCaptured = false;
}
bool poseCheckpointCapture(const KHZ_ClientCheckpointFrame&, char* reason, unsigned capacity) {
    if (checkpointCaptured || !poseCheckpointReady(reason, capacity)) return false;
    std::memcpy(&checkpointPoses, &poses, sizeof(poses));
    for (unsigned i = 0; i < PlayerCapacity; ++i) {
        const auto& object = checkpointPoses.players[i].object;
        if (object.duplicateParts) SL_AddRefToString(object.duplicateParts);
        checkpointPlayerPins = i + 1;
    }
    for (unsigned i = 0; i < poses.mapCount; ++i) {
        if (poses.maps[i].count > AnimationCapacity) return poseCheckpointReject(reason, capacity, "pose_checkpoint_cached_map_count_invalid");
        for (unsigned j = 0; j < poses.maps[i].count; ++j) {
            SL_AddRefToString(poses.maps[i].records[j].string); checkpointMapPins[i] = j + 1;
        }
    }
    checkpointCaptured = true; return true;
}
bool poseCheckpointValidate(const KHZ_ClientCheckpointFrame&, char* reason, unsigned capacity) {
    if (!checkpointCaptured || !poseCheckpointReady(reason, capacity)) return false;
    if (poses.game != checkpointPoses.game || poses.entities != checkpointPoses.entities || poses.mapCount != checkpointPoses.mapCount)
        return poseCheckpointReject(reason, capacity, "pose_checkpoint_storage_or_immutable_model_maps_changed");
    for (unsigned i = 0; i < poses.mapCount; ++i) {
        const auto& current = poses.maps[i]; const auto& saved = checkpointPoses.maps[i];
        if (current.count != saved.count || std::memcmp(current.names, saved.names, sizeof(current.names)))
            return poseCheckpointReject(reason, capacity, "pose_checkpoint_immutable_model_map_identity_changed");
        for (unsigned j = 0; j < current.count; ++j)
            if (current.records[j].parts != saved.records[j].parts || current.records[j].string != saved.records[j].string)
                return poseCheckpointReject(reason, capacity, "pose_checkpoint_animation_parts_or_map_strings_changed");
    }
    return true;
}
bool poseCheckpointRestore(const KHZ_ClientCheckpointFrame&, std::uint64_t, char* reason, unsigned capacity) {
    if (!KHZ_ClientOwnerCheckpointContext() || !checkpointCaptured)
        return poseCheckpointReject(reason, capacity, "pose_checkpoint_restore_not_prevalidated_on_worker");
    // Saved duplicate strings and animation maps have their own held refs.
    // Retire live player-object refs, then rebuild exactly the saved ownership.
    // Tree/node refs belong to the separate Animation provider.
    for (auto& player : poses.players) if (player.object.duplicateParts)
        SL_RemoveRefToStringOfSize(player.object.duplicateParts, player.object.duplicatePartsSize);
    std::memcpy(&poses, &checkpointPoses, sizeof(poses));
    for (auto& player : poses.players) if (player.object.duplicateParts) SL_AddRefToString(player.object.duplicateParts);
    return true;
}
}
void KHZ_ClientPoseRegisterCheckpoint() {
    const KHZ_ClientCheckpointProvider provider{KHZ_ClientCheckpointDomain::Pose,
        sizeof(checkpointPoses) + sizeof(checkpointMapPins), poseCheckpointCapture,
        poseCheckpointValidate, poseCheckpointRestore, poseCheckpointDiscard};
    KHZ_ClientCheckpointRegister(provider);
}
