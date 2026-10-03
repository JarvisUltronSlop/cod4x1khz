#include "khz_client_projectile.h"
#include "khz_client_owner.h"
#include "khz_canonical.h"
#include "khz_client_checkpoint.h"
#include <universal/q_shared.h>
#include <qcommon/threads.h>
#include <qcommon/qcommon.h>
#include <client_mp/client_mp.h>
#include <cgame_mp/cg_local_mp.h>
#include <bgame/bg_local.h>
#include <EffectsCore/fx_system.h>
#include <xanim/xmodel.h>
#include <xanim/dobj.h>
#include <script/scr_const.h>
#include <script/scr_stringlist.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace {
constexpr unsigned ModelCapacity = 128, EntityCapacity = 1024;
struct Prototype {
    XModel* model;
    DObj_s object;
    alignas(void*) unsigned char packedModel[sizeof(void*) + 1];
};
struct Projectile {
    bool valid, trailMade;
    std::uint64_t seen;
    int launchTime;
    unsigned weapon, temporalBits;
    const Prototype* prototype;
    DObj_s object;
};
struct Domain {
    bool prepared;
    cg_s* game;
    centity_s* entities;
    unsigned modelCount;
    Prototype models[ModelCapacity];
    Projectile projectiles[EntityCapacity];
    unsigned active[2][512], activeCount[2], activeBuffer;
    std::uint64_t tick;
    char failure[256];
};
Domain domain; // loader-zeroed POD; no dynamic CRT initializer or tick allocation
Domain checkpointDomain;
unsigned checkpointPrototypePins;
bool checkpointCaptured;

bool fail(const char* reason, const XModel* model = nullptr, unsigned entity = ENTITYNUM_NONE) {
    std::snprintf(domain.failure, sizeof(domain.failure), "%s entity=%u model=%s", reason, entity,
        model ? model->name : "none"); return false;
}
const Prototype* find(const XModel* model) {
    for (unsigned i = 0; i < domain.modelCount; ++i) if (domain.models[i].model == model) return &domain.models[i];
    return nullptr;
}
bool add(XModel* model) {
    if (!model || find(model)) return true;
    if (domain.modelCount == ModelCapacity) return fail("projectile_native_model_capacity_exhausted", model);
    if (!model->numBones || model->numBones > 128) return fail("projectile_native_bone_limit", model);
    // CG_PreProcess_GetDObj uses an unanimated, single-model object and disables
    // its collision. Gameplay collision remains the accepted snapshot/BSP.
    DObjModel_s descriptor{}; descriptor.model = model; descriptor.ignoreCollision = 1;
    DObj_s original{}; DObjCreate(&descriptor, 1, nullptr, &original, 0);
    auto& destination = domain.models[domain.modelCount++];
    destination.model = model; destination.object = original;
    std::memcpy(destination.packedModel, original.models, sizeof(destination.packedModel));
    destination.object.models = reinterpret_cast<XModel**>(destination.packedModel);
    destination.object.skel = {}; destination.object.locked = 0;
    if (destination.object.duplicateParts) SL_AddRefToString(destination.object.duplicateParts);
    DObjFree(&original);
    return true;
}
const WeaponDef* weaponFor(const entityState_s& state) {
    // Native MP CG_Missile maps an invalid weapon index to zero before lookup.
    return BG_GetWeaponDef(BG_ValidateWeaponNumber(state.weapon) ? state.weapon : 0);
}
bool needed(const WeaponDef* weapon) {
    return weapon && weapon->projTrailEffect && FX_EffectAffectsGameplay(weapon->projTrailEffect);
}
bool supports(const entityState_s& state) {
    const auto* weapon = weaponFor(state);
    if (!weapon || !weapon->projectileModel) return true; // Native draws no object/trail.
    const auto* prototype = find(weapon->projectileModel);
    if (!prototype) return false;
    if (needed(weapon)) {
        // Visual PhysX/ragdoll trajectories have no private gameplay physics
        // owner. Reject only a trail that actually affects native visibility.
        if (state.lerp.pos.trType == TR_PHYSICS || CG_IsRagdollTrajectory(&state.lerp.pos)) return false;
        // A missing tag_fx is valid native input: CG_PlayBoltedEffect simply
        // does not spawn the trail. Never invent a root/eye substitute.
    }
    return true;
}
void bind(Projectile& projectile, const Prototype* prototype, const entityState_s& state) {
    const bool respawn = !projectile.valid || projectile.launchTime != state.lerp.u.missile.launchTime ||
        projectile.weapon != state.weapon ||
        projectile.temporalBits != (state.lerp.eFlags & 2);
    if (projectile.prototype != prototype || respawn) {
        projectile.object = prototype->object; projectile.prototype = prototype;
        projectile.object.entnum = static_cast<short>(state.number + 1);
        projectile.object.skel = {}; projectile.object.locked = 0;
        projectile.trailMade = false;
        // The prototype holds the duplicate-string reference for this whole map
        // lifetime. Slots never free native allocations or string ownership.
    }
    projectile.launchTime = state.lerp.u.missile.launchTime;
    projectile.weapon = state.weapon; projectile.temporalBits = state.lerp.eFlags & 2;
    projectile.valid = true;
}
}

bool KHZ_ClientProjectilePrepare(cg_s* game, centity_s* entities) {
    iassert(Sys_IsMainThread());
    if (domain.prepared || !game || !entities) return fail("invalid_projectile_prepare_arguments");
    domain.game = game; domain.entities = entities; domain.prepared = true;
    domain.activeBuffer = 0; domain.activeCount[0] = domain.activeCount[1] = 0; domain.tick = 0;
    if (!KHZ_ClientProjectileRefresh()) { KHZ_ClientProjectileRelease(); return false; }
    // Existing accepted missiles may own bolted effects when the private FX
    // domain migrates. Publish their descriptors now, before worker startup.
    for (unsigned i = 0; i < EntityCapacity; ++i) {
        const auto& entity = entities[i];
        if (!entity.nextValid || entity.nextState.eType != ET_MISSILE) continue;
        const auto* weapon = weaponFor(entity.nextState);
        if (weapon && weapon->projectileModel && entity.currentState.pos.trType != TR_PHYSICS &&
            entity.nextState.lerp.pos.trType != TR_PHYSICS && !CG_IsRagdollTrajectory(&entity.currentState.pos) &&
            !CG_IsRagdollTrajectory(&entity.nextState.lerp.pos)) {
            bind(domain.projectiles[i], find(weapon->projectileModel), entity.nextState);
            // Initial map migration captures native one-shot trail bookkeeping
            // once alongside the actual FX particle pools. No later render flag
            // feeds this owner; bounce trajectory changes do not replay a trail.
            domain.projectiles[i].trailMade = entity.bTrailMade != 0;
            if (domain.activeCount[0] == ARRAY_COUNT(domain.active[0])) {
                fail("projectile_accepted_entity_capacity_exhausted"); KHZ_ClientProjectileRelease(); return false;
            }
            domain.active[0][domain.activeCount[0]++] = i;
        }
    }
    return true;
}
bool KHZ_ClientProjectileRefresh() {
    iassert(Sys_IsMainThread());
    if (!domain.prepared) return fail("projectile_refresh_without_prepared_domain");
    for (unsigned i = 0; i < BG_GetNumWeapons(); ++i) {
        const auto* weapon = BG_GetWeaponDef(i);
        if (weapon && !add(weapon->projectileModel)) return false;
    }
    domain.failure[0] = 0; return true;
}
void KHZ_ClientProjectileRelease() {
    iassert(Sys_IsMainThread());
    for (auto& projectile : domain.projectiles) projectile = {};
    for (unsigned i = 0; i < domain.modelCount; ++i) {
        auto& model = domain.models[i];
        if (model.object.duplicateParts) SL_RemoveRefToStringOfSize(model.object.duplicateParts, model.object.duplicatePartsSize);
        model = {};
    }
    domain.modelCount = 0; domain.prepared = false; domain.game = nullptr; domain.entities = nullptr;
    domain.activeCount[0] = domain.activeCount[1] = 0; domain.activeBuffer = 0; domain.tick = 0;
}
bool KHZ_ClientProjectileAccepts(const snapshot_s* snapshot) {
    if (!domain.prepared || !snapshot) return false;
    for (int i = 0; i < snapshot->numEntities; ++i) {
        const auto& state = snapshot->entities[i];
        if (state.eType == ET_MISSILE && !(state.lerp.eFlags & 0x20) && !supports(state))
            return fail("accepted_projectile_requires_prepared_native_model_or_pose", weaponFor(state)->projectileModel, state.number);
    }
    return true;
}
bool KHZ_ClientProjectileTick() {
    iassert(KHZ_IsClientSimulationThread());
    if (!domain.prepared || !domain.game->nextSnap) return false;
    const unsigned previous = domain.activeBuffer, next = previous ^ 1;
    domain.activeCount[next] = 0; ++domain.tick;
    for (int s = 0; s < domain.game->nextSnap->numEntities; ++s) {
        const unsigned i = domain.game->nextSnap->entities[s].number;
        auto& projectile = domain.projectiles[i]; auto& entity = domain.entities[i];
        const auto& state = entity.nextState;
        if (!entity.nextValid || state.eType != ET_MISSILE || (state.lerp.eFlags & 0x20) ||
            state.lerp.u.missile.launchTime > domain.game->time) {
            projectile.valid = false; continue;
        }
        const auto* weapon = weaponFor(state);
        const auto* prototype = weapon && weapon->projectileModel ? find(weapon->projectileModel) : nullptr;
        if (!prototype) { projectile.valid = false; continue; }
        const bool visualPhysics = entity.currentState.pos.trType == TR_PHYSICS ||
            state.lerp.pos.trType == TR_PHYSICS || CG_IsRagdollTrajectory(&entity.currentState.pos) ||
            CG_IsRagdollTrajectory(&state.lerp.pos);
        if (!supports(state) || (needed(weapon) && visualPhysics)) {
            fail("projectile_gameplay_trail_requires_supported_native_pose", weapon->projectileModel, i);
            Com_Error(ERR_DROP, "%s", domain.failure); return false;
        }
        if (visualPhysics) { projectile.valid = false; continue; }
        bind(projectile, prototype, state);
        projectile.seen = domain.tick; domain.active[next][domain.activeCount[next]++] = i;
        entity.pose.localClientNum = 0; entity.pose.eType = ET_MISSILE;
        // The same native trajectory interpolation/evaluation used by MP's
        // presentation runs at the fixed owner time, with private CG storage.
        CG_CalcEntityLerpPositions(0, &entity);
    }
    for (unsigned a = 0; a < domain.activeCount[previous]; ++a) {
        auto& projectile = domain.projectiles[domain.active[previous][a]];
        if (projectile.seen != domain.tick) projectile.valid = false;
    }
    domain.activeBuffer = next;
    return true;
}
DObj_s* KHZ_ClientProjectileGetDObj(unsigned handle) {
    return domain.prepared && handle < EntityCapacity && domain.projectiles[handle].valid ? &domain.projectiles[handle].object : nullptr;
}
bool KHZ_ClientProjectileOwnsDObj(const DObj_s* object) {
    if (!KHZ_IsClientSimulationThread() || !domain.prepared || !object) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(object);
    const auto first = reinterpret_cast<std::uintptr_t>(&domain.projectiles[0].object);
    if (address < first || (address - first) % sizeof(Projectile)) return false;
    const auto index = (address - first) / sizeof(Projectile);
    return index < EntityCapacity && domain.projectiles[index].valid;
}
const char* KHZ_ClientProjectileFailureReason() { return domain.failure; }
unsigned KHZ_ClientProjectileModelCount() { return domain.prepared ? domain.modelCount : 0; }
bool KHZ_ClientProjectileTrailPending(unsigned handle) {
    iassert(KHZ_IsClientSimulationThread());
    return domain.prepared && handle < EntityCapacity && domain.projectiles[handle].valid && !domain.projectiles[handle].trailMade;
}
void KHZ_ClientProjectileMarkTrail(unsigned handle) {
    iassert(KHZ_IsClientSimulationThread() || Sys_IsMainThread());
    if (domain.prepared && handle < EntityCapacity && domain.projectiles[handle].valid) domain.projectiles[handle].trailMade = true;
}
bool KHZ_ClientProjectileCanonical(khz::canonical::Hash& hash) {
    iassert(KHZ_IsClientSimulationThread());
    if (!KHZ_IsClientSimulationThread() || !domain.prepared || domain.activeBuffer > 1) { hash.reject(); return false; }
    hash.scalar(domain.activeBuffer);
    // Both lists control the following tick's disappearance bookkeeping. Their
    // order is the accepted entity order, never a renderer enumeration.
    for (unsigned buffer = 0; buffer < 2; ++buffer) {
        if (domain.activeCount[buffer] > ARRAY_COUNT(domain.active[buffer])) { hash.reject(); return false; }
        hash.scalar(domain.activeCount[buffer]);
        for (unsigned i = 0; i < domain.activeCount[buffer]; ++i) {
            const unsigned entity = domain.active[buffer][i];
            if (entity >= EntityCapacity) { hash.reject(); return false; }
            hash.scalar(entity);
        }
    }
    for (unsigned i = 0; i < EntityCapacity; ++i) {
        const auto& projectile = domain.projectiles[i];
        hash.boolean(projectile.valid);
        if (!projectile.valid) continue;
        if (!projectile.prototype || !projectile.prototype->model || projectile.seen > domain.tick) { hash.reject(); return false; }
        hash.scalar(i); hash.boolean(projectile.trailMade); hash.scalar(projectile.launchTime);
        hash.scalar(projectile.weapon); hash.scalar(projectile.temporalBits); hash.text(projectile.prototype->model->name);
        hash.u64(domain.tick - projectile.seen);
    }
    return hash.finish().valid;
}

namespace {
bool projectileCheckpointReject(char* reason, unsigned capacity, const char* text) {
    if (reason && capacity) std::snprintf(reason, capacity, "%s", text); return false;
}
void projectileCheckpointDiscard() {
    for (unsigned i = 0; i < checkpointPrototypePins; ++i) {
        const auto& object = checkpointDomain.models[i].object;
        if (object.duplicateParts) SL_RemoveRefToStringOfSize(object.duplicateParts, object.duplicatePartsSize);
    }
    checkpointPrototypePins = 0; checkpointCaptured = false;
}
bool projectileCheckpointCapture(const KHZ_ClientCheckpointFrame&, char* reason, unsigned capacity) {
    khz::canonical::Hash hash;
    if (!KHZ_ClientOwnerCheckpointContext() || checkpointCaptured || domain.modelCount > ModelCapacity ||
        !KHZ_ClientProjectileCanonical(hash))
        return projectileCheckpointReject(reason, capacity, "projectile_checkpoint_native_state_or_boundary_invalid");
    std::memcpy(&checkpointDomain, &domain, sizeof(domain));
    for (unsigned i = 0; i < domain.modelCount; ++i) {
        if (domain.models[i].object.duplicateParts) SL_AddRefToString(domain.models[i].object.duplicateParts);
        checkpointPrototypePins = i + 1;
    }
    checkpointCaptured = true; return true;
}
bool projectileCheckpointValidate(const KHZ_ClientCheckpointFrame&, char* reason, unsigned capacity) {
    khz::canonical::Hash hash;
    if (!KHZ_ClientOwnerCheckpointContext() || !checkpointCaptured || !KHZ_ClientProjectileCanonical(hash) ||
        domain.modelCount != checkpointDomain.modelCount || domain.game != checkpointDomain.game || domain.entities != checkpointDomain.entities)
        return projectileCheckpointReject(reason, capacity, "projectile_checkpoint_storage_or_asset_lifetime_changed");
    for (unsigned i = 0; i < domain.modelCount; ++i)
        if (domain.models[i].model != checkpointDomain.models[i].model ||
            domain.models[i].object.duplicateParts != checkpointDomain.models[i].object.duplicateParts)
            return projectileCheckpointReject(reason, capacity, "projectile_checkpoint_native_model_or_duplicate_binding_changed");
    return true;
}
bool projectileCheckpointRestore(const KHZ_ClientCheckpointFrame&, std::uint64_t, char* reason, unsigned capacity) {
    if (!KHZ_ClientOwnerCheckpointContext() || !checkpointCaptured)
        return projectileCheckpointReject(reason, capacity, "projectile_checkpoint_restore_not_prevalidated_on_worker");
    for (unsigned i = 0; i < domain.modelCount; ++i) {
        const auto& object = domain.models[i].object;
        if (object.duplicateParts) SL_RemoveRefToStringOfSize(object.duplicateParts, object.duplicatePartsSize);
    }
    std::memcpy(&domain, &checkpointDomain, sizeof(domain));
    for (unsigned i = 0; i < domain.modelCount; ++i)
        if (domain.models[i].object.duplicateParts) SL_AddRefToString(domain.models[i].object.duplicateParts);
    return true;
}
}
void KHZ_ClientProjectileRegisterCheckpoint() {
    const KHZ_ClientCheckpointProvider provider{KHZ_ClientCheckpointDomain::Projectiles, sizeof(checkpointDomain),
        projectileCheckpointCapture, projectileCheckpointValidate, projectileCheckpointRestore, projectileCheckpointDiscard};
    KHZ_ClientCheckpointRegister(provider);
}
