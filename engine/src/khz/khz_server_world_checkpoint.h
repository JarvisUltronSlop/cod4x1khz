#pragma once
#include "khz_server_checkpoint_native.h"
#include <game_mp/g_main_mp.h>
#include <client_mp/client_mp.h>
#include <xanim/xanim.h>
#include <script/scr_stringlist.h>
#include <cmath>
extern scrMemTreePub_t scrMemTreePub;

// Every source-owned fragment requires ALL eight native storage parts. These
// declarations do not install hooks or register an uncaptured hunk range.
inline constexpr unsigned KHZ_ServerWorldRequiredParts = 0xff;
void KHZ_ServerWorldMainRegisterCheckpoint();       // game_mp/g_main_mp.cpp, part0
void KHZ_ServerWorldScriptRegisterCheckpoint();     // game_mp/g_scr_main_mp.cpp, part1
void KHZ_ServerWorldHudRegisterCheckpoint();        // game/g_hudelem.cpp, part2
void KHZ_ServerWorldTurretsRegisterCheckpoint();    // game_mp/g_misc_mp.cpp, part3
void KHZ_ServerWorldVehiclesRegisterCheckpoint();   // game_mp/g_vehicles_mp.cpp, part4
void KHZ_ServerWorldMissilesRegisterCheckpoint();   // game/g_missile.cpp, part5
void KHZ_ServerWorldMoversRegisterCheckpoint();     // game/g_mover.cpp, part6
void KHZ_ServerWorldVehiclePhysicsRegisterCheckpoint();// game/g_scr_vehicle.cpp, part7

namespace khz::server_world_checkpoint {
using namespace khz::server_native_checkpoint;
template<class T, std::size_t N> bool element(const T* value, const T (&array)[N], bool allowNull = true) {
    if (!value) return allowNull;
    return inside(value, array, sizeof(array), sizeof(T)) &&
        (reinterpret_cast<std::uintptr_t>(value) - reinterpret_cast<std::uintptr_t>(array)) % sizeof(T) == 0;
}
inline bool entity(const gentity_s* value, bool allowNull = true) { return element(value, g_entities, allowNull); }
inline bool tag(const tagInfo_s* value) {
    return value && reinterpret_cast<std::uintptr_t>(value) % alignof(tagInfo_s) == 0 &&
        scrMemTreePub.mt_buffer == reinterpret_cast<char*>(scrMemTreeGlob.nodes) &&
        inside(value, scrMemTreeGlob.nodes, sizeof(scrMemTreeGlob.nodes), sizeof(tagInfo_s)) &&
        (reinterpret_cast<std::uintptr_t>(value) - reinterpret_cast<std::uintptr_t>(scrMemTreeGlob.nodes)) % MT_NODE_SIZE == 0 &&
        KHZ_ServerCheckpointRestoresRange(KHZ_ServerCheckpointDomain::ScriptAllocators, value, sizeof(tagInfo_s));
}
inline bool finite3(const float* value) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}
inline bool frame(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity) || !KHZ_ServerScriptCheckpointIdle(text, capacity)) return false;
    return f.entitiesAddress == reinterpret_cast<std::uintptr_t>(g_entities) &&
        level.gentities == g_entities && level.gentitySize == sizeof(gentity_s) &&
        level.time == f.nativeTime && !level.initializing && !level.clientIsSpawning &&
        level.currentEntityThink == -1 && bgs == nullptr ? true :
        reject(text, capacity, "server_world_not_completed_tick_or_original_entity_arena");
}
inline bool clientInfo(const clientInfo_t& info, char* text, unsigned capacity) {
    if (!element(info.legs.animation, level_bgs.animScriptData.animations) ||
        !element(info.torso.animation, level_bgs.animScriptData.animations))
        return reject(text, capacity, "server_world_lerp_animation_outside_original_bgs_table");
    if (info.pXAnimTree) {
        if (reinterpret_cast<std::uintptr_t>(info.pXAnimTree) % alignof(XAnimTree_s) ||
            !KHZ_ServerCheckpointRestoresRange(KHZ_ServerCheckpointDomain::Animation, info.pXAnimTree, sizeof(XAnimTree_s)))
            return reject(text, capacity, "server_world_native_xanim_tree_has_no_concrete_animation_restore_span");
        if (info.pXAnimTree->calcRefCount || info.pXAnimTree->modifyRefCount ||
            info.pXAnimTree->anims != level_bgs.generic_human.tree.anims)
            return reject(text, capacity, "server_world_native_xanim_tree_busy_or_asset_identity_changed");
    }
    return true;
}
inline void registerPart(const KHZ_ServerCheckpointProvider& provider, const void* storage, std::size_t bytes) {
    if (!KHZ_ServerCheckpointRequireParts(provider.domain, KHZ_ServerWorldRequiredParts) ||
        !KHZ_ServerCheckpointRegister(provider) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, storage, bytes))
        Com_Error(ERR_FATAL, "Could not register complete native server World checkpoint part %u", provider.part);
}
}
