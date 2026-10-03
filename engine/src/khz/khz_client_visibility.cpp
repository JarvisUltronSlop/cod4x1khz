#include "khz_client_visibility.h"
#include "khz_client_owner.h"
#include "khz_client_pose.h"
#include "khz_client_projectile.h"
#include "khz_canonical.h"
#include "khz_client_checkpoint.h"
#include <universal/q_shared.h>
#include <universal/surfaceflags.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <client_mp/client_mp.h>
#include <cgame_mp/cg_local_mp.h>
#include <bgame/bg_local.h>
#include <EffectsCore/fx_system.h>
#include <xanim/dobj.h>
#include <script/scr_stringlist.h>
#include <script/scr_const.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

void __cdecl CG_RandomEffectAxis(const float* forward, float* left, float* up);

namespace {
constexpr unsigned DefinitionCapacity = 256;
constexpr unsigned ElementCapacity = 128;
struct Definition {
    const FxEffectDef* original;
    FxEffectDef filtered;
    bool needed[ElementCapacity];
};
struct TagEffect { unsigned effect, tag; };
struct VisibilityDomain {
    bool prepared, preparing, unsupported;
    const char* failure;
    char failureDetail[512];
    unsigned migratedBolts;
    cg_s* game;
    cgs_t* gameStatic;
    FxSystem system;
    FxSystemBuffers buffers;
    Definition definitions[DefinitionCapacity];
    unsigned definitionCount;
    TagEffect tags[256];
    struct EntityEffect { FxEffect* effect; int trigger, type; bool seen; unsigned definition; } entities[1024];
    // Preparation scratch is fixed storage, and is never touched by rendering.
    struct Migration {
        bool effects[1024], elements[2048], trails[128], trailElements[2048];
        unsigned originalParticles[1024], retainedParticles[1024];
        unsigned originalChildren[1024], retainedChildren[1024], external[1024];
    } migration;
};
VisibilityDomain visibility; // loader-zeroed POD; no large CRT temporary
VisibilityDomain checkpointVisibility;
unsigned checkpointTagPins;
bool checkpointCaptured;

const FxEffectDef* prepareDefinition(const FxEffectDef* original);
const Definition* preparedDefinition(const FxEffectDef* original) {
    for (unsigned i = 0; i < visibility.definitionCount; ++i)
        if (visibility.definitions[i].original == original || &visibility.definitions[i].filtered == original)
            return &visibility.definitions[i];
    return nullptr;
}
bool gameplayChild(const FxEffectDef* definition) { return definition && FX_EffectAffectsGameplay(definition); }
bool failDefinition(const char* reason, const FxEffectDef* definition, unsigned entity = ENTITYNUM_NONE, unsigned tag = 0) {
    std::snprintf(visibility.failureDetail, sizeof(visibility.failureDetail), "%s effect=%s entity=%u tag=%s",
        reason, definition && definition->name ? definition->name : "none", entity,
        tag ? SL_ConvertToString(tag) : "none");
    visibility.failure = visibility.failureDetail; return false;
}
bool neededElement(const FxElemDef& element) {
    if (element.flags & 0x1000) return true;
    if (gameplayChild(element.effectOnDeath.handle) || gameplayChild(element.effectOnImpact.handle) ||
        gameplayChild(element.effectEmitted.handle)) return true;
    if (element.elemType != 10) return false;
    if (element.visualCount == 1) return gameplayChild(element.visuals.instance.effectDef.handle);
    for (unsigned i = 0; i < element.visualCount; ++i)
        if (gameplayChild(element.visuals.array[i].effectDef.handle)) return true;
    return false;
}
const FxEffectDef* prepareDefinition(const FxEffectDef* original) {
    if (!gameplayChild(original)) return nullptr;
    for (unsigned i = 0; i < visibility.definitionCount; ++i)
        if (visibility.definitions[i].original == original) return &visibility.definitions[i].filtered;
    const int count = original->elemDefCountLooping + original->elemDefCountOneShot + original->elemDefCountEmission;
    if (visibility.definitionCount == DefinitionCapacity || count < 0 || count > ElementCapacity) {
        failDefinition("gameplay_fx_definition_or_element_capacity_exceeded", original);
        visibility.unsupported = true; return nullptr;
    }
    auto& entry = visibility.definitions[visibility.definitionCount++];
    entry.original = original; entry.filtered = *original;
    // Packed lighting is a presentation payload, unrelated to opacity/physics.
    // It would query renderer light-grid state during FX_SpawnEffect.
    entry.filtered.flags &= ~1;
    // Keep immutable native element arrays and counts. Renumbering survivors
    // would alter native indices, emission bookkeeping and random selection.
    // Spawn/allocate hooks skip cosmetic slots; child definitions are routed
    // through this table only when they are actually spawned.
    for (int i = 0; i < count; ++i) {
        const auto& source = original->elemDefs[i];
        entry.needed[i] = neededElement(source);
        if (!entry.needed[i]) continue;
        if ((source.elemType > 4 && source.elemType != 10) ||
            (source.flags & 0x8000000)) {
            failDefinition("gameplay_fx_requires_unsupported_element_or_physics", original);
            visibility.unsupported = true; return nullptr;
        }
        prepareDefinition(source.effectOnImpact.handle);
        prepareDefinition(source.effectOnDeath.handle);
        prepareDefinition(source.effectEmitted.handle);
        if (source.elemType == 10) {
            if (source.visualCount == 1)
                prepareDefinition(source.visuals.instance.effectDef.handle);
            else for (unsigned visual = 0; visual < source.visualCount; ++visual)
                prepareDefinition(source.visuals.array[visual].effectDef.handle);
        }
    }
    return visibility.unsupported ? nullptr : &entry.filtered;
}
void play(const FxEffectDef* definition, int time, const float* origin, const float (*axis)[3]) {
    if (!definition || !FX_EffectAffectsGameplay(definition)) return;
    auto* effect = FX_SpawnEffect(&visibility.system, definition, time, origin, axis,
        FX_DOBJ_HANDLE_NONE, FX_BONE_INDEX_NONE, 255, FX_HANDLE_NONE, ENTITYNUM_NONE);
    if (effect) FX_DelRefToEffect(&visibility.system, effect);
}
void impact(unsigned weapon, unsigned surface, int time, const float* origin, const float (*axis)[3]) {
    if (surface >= SURF_TYPECOUNT) { KHZ_ClientOwnerRequestAssetRebuild("Visibility impact surface is invalid"); return; }
    const FxEffectDef* effect = nullptr; snd_alias_list_t* unused = nullptr;
    CG_ImpactEffectForWeapon(weapon, surface, 0, &effect, &unused);
    play(effect, time, origin, axis);
}
bool evaluateElement(FxEffect* effect, const FxElemDef* definition, int begin, int sequence, const float* origin) {
    if (!(definition->flags & 0x1000) || begin > visibility.system.msecDraw) return true;
    FxDrawState state{};
    state.system = &visibility.system; state.effect = effect; state.elemDef = definition;
    state.msecDraw = visibility.system.msecDraw;
    // This native evaluator stops before rendering: it evaluates orientation,
    // lifetime/size/alpha/distance fade and appends the stock packed blocker.
    FX_DrawElement_Setup_1_(&visibility.system, &state, begin, sequence, origin, nullptr);
    return true;
}
template<typename Item, unsigned Capacity>
void rebuildFreeList(FxPool<Item>* pool, const bool (&used)[Capacity], volatile long& first, volatile long& active) {
    first = -1; active = 0;
    for (unsigned reverse = Capacity; reverse; ) {
        const auto index = --reverse;
        if (used[index]) ++active;
        else { pool[index].nextFree = first; first = index; }
    }
}
unsigned poolIndex(const void* item, const void* base, unsigned stride) {
    const auto address = reinterpret_cast<std::uintptr_t>(item);
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    if (address < start || (address - start) % stride) return ~0u;
    return static_cast<unsigned>((address - start) / stride);
}
// Migrate actual native particle records, rather than respawning old effects
// with a guessed historical trajectory. Handles, seeds, sequence numbers,
// origins, velocity, rest state and emission residual are retained exactly.
// Only gameplay particles/children survive; reference counts are reconstructed
// and checked against every recognized native owner before publication.
bool migrateEffects(const FxSystem& original) {
    if (!original.isInitialized) return true;
    if (original.isArchiving || original.iteratorCount || original.deferredElemCount ||
        original.firstFreeEffect != original.firstNewEffect ||
        original.firstNewEffect - original.firstActiveEffect > 1024) {
        visibility.failure = "native_fx_migration_has_pending_archive_iterator_or_effect_work"; return false;
    }
    auto& scratch = visibility.migration;
    scratch = {};
    auto& system = visibility.system;
    system.camera = original.camera; system.cameraPrev = original.cameraPrev;
    system.msecNow = original.msecNow; system.msecDraw = original.msecDraw;
    system.frameCount = original.frameCount;
    for (int active = original.firstActiveEffect; active != original.firstNewEffect; ++active) {
        const auto handle = original.allEffectHandles[active & 1023];
        const auto* effect = FX_EffectFromHandle(const_cast<FxSystem*>(&original), handle);
        if (!(effect->status & 0xffff)) continue;
        const auto index = static_cast<unsigned>(effect - original.effects);
        if (index >= 1024) return false;
        if (!(effect->status & 0x10000000)) {
            const auto* parent = FX_EffectFromHandle(const_cast<FxSystem*>(&original), effect->owner);
            const auto parentIndex = static_cast<unsigned>(parent - original.effects);
            if (parentIndex >= 1024) return false;
            ++scratch.originalChildren[parentIndex];
        }
        if (!gameplayChild(effect->def)) continue;
        const auto* definition = prepareDefinition(effect->def);
        if (!definition) return false;
        scratch.effects[index] = true;
        auto& copy = system.effects[index]; copy = *effect; copy.def = definition;
        if (effect->boltAndSortOrder.boneIndex != FX_BONE_INDEX_NONE) {
            const auto entity = effect->boltAndSortOrder.dobjHandle;
            const auto* native = Com_GetClientDObj(entity, 0);
            const auto* privateObject = KHZ_ClientPoseGetDObj(entity);
            if (!native || !privateObject || effect->boltAndSortOrder.boneIndex >= native->numBones)
                return failDefinition("native_gameplay_fx_has_an_unprepared_bolt_pose", effect->def, entity);
            unsigned local = effect->boltAndSortOrder.boneIndex, tag = 0;
            for (unsigned model = 0; model < native->numModels; ++model) {
                if (local < native->models[model]->numBones) { tag = native->models[model]->boneNames[local]; break; }
                local -= native->models[model]->numBones;
            }
            unsigned char privateBone = 254;
            if (!tag || !DObjGetBoneIndex(privateObject, tag, &privateBone))
                return failDefinition("native_gameplay_fx_bolt_tag_is_not_in_private_composition", effect->def, entity, tag);
            // Weapon omission changes head/attachment offsets. Preserve the
            // real native attachment tag, never the source numeric bone index.
            copy.boltAndSortOrder.boneIndex = privateBone; ++visibility.migratedBolts;
            if (entity < 1024 && visibility.game->nextSnap) {
                const auto* missile = CG_GetEntity(0, entity);
                if (missile->nextState.eType == ET_MISSILE && BG_ValidateWeaponNumber(missile->nextState.weapon) &&
                    BG_GetWeaponDef(missile->nextState.weapon)->projTrailEffect == effect->def)
                    KHZ_ClientProjectileMarkTrail(entity);
            }
        }
        copy.firstSortedElemHandle = FX_HANDLE_NONE;
        for (unsigned list = 0; list < 3; ++list) {
            copy.firstElemHandle[list] = FX_HANDLE_NONE;
            unsigned short previous = FX_HANDLE_NONE;
            for (auto elementHandle = effect->firstElemHandle[list]; elementHandle != FX_HANDLE_NONE; ) {
                const auto* element = &FX_PoolFromHandle_Generic<FxElem, 2048>(original.elems, elementHandle)->item;
                ++scratch.originalParticles[index];
                const auto elementIndex = poolIndex(element, original.elems, sizeof(FxPool<FxElem>));
                // Pool<T> and T have identical size/alignment by definition.
                if (elementIndex >= 2048 || element->defIndex >= ElementCapacity) return false;
                const auto* entry = preparedDefinition(effect->def);
                if (entry && entry->needed[element->defIndex]) {
                    if (scratch.elements[elementIndex]) return false;
                    scratch.elements[elementIndex] = true;
                    auto& elementCopy = system.elems[elementIndex].item; elementCopy = *element;
                    elementCopy.prevElemHandleInEffect = previous;
                    elementCopy.nextElemHandleInEffect = FX_HANDLE_NONE;
                    if (previous == FX_HANDLE_NONE) copy.firstElemHandle[list] = elementHandle;
                    else FX_PoolFromHandle_Generic<FxElem, 2048>(system.elems, previous)->item.nextElemHandleInEffect = elementHandle;
                    previous = elementHandle; ++scratch.retainedParticles[index];
                }
                elementHandle = element->nextElemHandleInEffect;
            }
        }
        copy.firstTrailHandle = FX_HANDLE_NONE;
        unsigned short previousTrail = FX_HANDLE_NONE;
        for (auto trailHandle = effect->firstTrailHandle; trailHandle != FX_HANDLE_NONE; ) {
            const auto* trail = &FX_PoolFromHandle_Generic<FxTrail, 128>(original.trails, trailHandle)->item;
            const auto trailIndex = poolIndex(trail, original.trails, sizeof(FxPool<FxTrail>));
            const auto definitionIndex = static_cast<unsigned char>(trail->defIndex);
            if (trailIndex >= 128 || definitionIndex >= ElementCapacity) return false;
            const auto* entry = preparedDefinition(effect->def);
            const bool retained = entry && entry->needed[definitionIndex];
            if (retained) {
                if (scratch.trails[trailIndex]) return false;
                scratch.trails[trailIndex] = true;
                system.trails[trailIndex].item = *trail;
                system.trails[trailIndex].item.nextTrailHandle = FX_HANDLE_NONE;
                if (previousTrail == FX_HANDLE_NONE) copy.firstTrailHandle = trailHandle;
                else FX_PoolFromHandle_Generic<FxTrail, 128>(system.trails, previousTrail)->item.nextTrailHandle = trailHandle;
                previousTrail = trailHandle;
            }
            for (auto elementHandle = trail->firstElemHandle; elementHandle != FX_HANDLE_NONE; ) {
                const auto* element = &FX_PoolFromHandle_Generic<FxTrailElem, 2048>(original.trailElems, elementHandle)->item;
                ++scratch.originalParticles[index];
                const auto elementIndex = poolIndex(element, original.trailElems, sizeof(FxPool<FxTrailElem>));
                if (elementIndex >= 2048) return false;
                if (retained) {
                    if (scratch.trailElements[elementIndex]) return false;
                    scratch.trailElements[elementIndex] = true;
                    system.trailElems[elementIndex].item = *element;
                    ++scratch.retainedParticles[index];
                }
                elementHandle = element->nextTrailElemHandle;
            }
            trailHandle = trail->nextTrailHandle;
        }
        if (original.activeSpotLightElemCount && original.activeSpotLightEffectHandle == handle)
            ++scratch.originalParticles[index]; // cosmetic spotlight is not retained
    }
    // Native ET_FX/ET_LOOP_FX store one explicit reference in their pose union.
    // Other unidentified external references are rejected below, never guessed.
    for (int i = 0; i < visibility.game->nextSnap->numEntities; ++i) {
        const auto& state = visibility.game->nextSnap->entities[i];
        if (state.eType != ET_FX && state.eType != ET_LOOP_FX) continue;
        const auto* entity = CG_GetEntity(0, state.number);
        const auto* effect = entity->pose.fx.effect;
        if (!effect) continue;
        if (effect < original.effects || effect >= original.effects + 1024) return false;
        const auto index = static_cast<unsigned>(effect - original.effects);
        if (!scratch.effects[index]) continue;
        ++scratch.external[index];
        visibility.entities[state.number] = {&system.effects[index], entity->pose.fx.triggerTime, state.eType, false, static_cast<unsigned>(state.un1.scale)};
    }
    for (unsigned index = 0; index < 1024; ++index) if (scratch.effects[index]) {
        const auto& effect = original.effects[index];
        if (!(effect.status & 0x10000000)) {
            const auto* parent = FX_EffectFromHandle(const_cast<FxSystem*>(&original), effect.owner);
            const auto parentIndex = static_cast<unsigned>(parent - original.effects);
            if (parentIndex >= 1024 || !scratch.effects[parentIndex]) return false;
            ++scratch.retainedChildren[parentIndex];
        }
        const unsigned known = scratch.originalParticles[index] + scratch.originalChildren[index] +
            scratch.external[index] + ((effect.status & 0x10000) ? 1 : 0);
        if ((effect.status & 0xffff) != known || ((effect.status >> 17) & 1023) != scratch.originalChildren[index]) return false;
    }
    unsigned activeCount = 0;
    for (int active = original.firstActiveEffect; active != original.firstNewEffect; ++active) {
        const auto handle = original.allEffectHandles[active & 1023];
        const auto* effect = FX_EffectFromHandle(const_cast<FxSystem*>(&original), handle);
        const auto index = static_cast<unsigned>(effect - original.effects);
        if (!scratch.effects[index]) continue;
        const unsigned refs = scratch.retainedParticles[index] + scratch.retainedChildren[index] +
            scratch.external[index] + ((effect->status & 0x10000) ? 1 : 0);
        system.effects[index].status = (effect->status & 0x10010000) | refs | (scratch.retainedChildren[index] << 17);
        system.allEffectHandles[activeCount++] = handle;
        if (!refs) system.needsGarbageCollection = true;
    }
    system.firstActiveEffect = 0; system.firstNewEffect = system.firstFreeEffect = activeCount;
    for (unsigned index = 0; index < 1024; ++index) if (!scratch.effects[index])
        system.allEffectHandles[activeCount++] = static_cast<unsigned short>(FX_EffectToHandle(&system, &system.effects[index]));
    rebuildFreeList(system.elems, scratch.elements, system.firstFreeElem, system.activeElemCount);
    rebuildFreeList(system.trails, scratch.trails, system.firstFreeTrail, system.activeTrailCount);
    rebuildFreeList(system.trailElems, scratch.trailElements, system.firstFreeTrailElem, system.activeTrailElemCount);
    return activeCount == 1024;
}
}

bool KHZ_ClientVisibilityPrepare(cg_s* game, cgs_t* gameStatic) {
    iassert(Sys_IsMainThread());
    if (visibility.prepared || !game || !gameStatic) return false;
    visibility.game = game; visibility.gameStatic = gameStatic; visibility.preparing = true;
    visibility.unsupported = false; visibility.definitionCount = 0; visibility.failure = ""; visibility.migratedBolts = 0;
    for (auto& effect : visibility.entities) effect = {};
    visibility.system = {}; visibility.buffers = {};
    FX_LinkSystemBuffers(&visibility.system, &visibility.buffers);
    FX_ResetSystem(&visibility.system);
    visibility.system.localClientNum = 0; visibility.system.isInitialized = true;
    visibility.system.msecNow = visibility.system.msecDraw = game->time;
    visibility.system.frameCount = 1;
    auto* originalSystem = FX_GetSystem(0);
    visibility.system.camera = originalSystem->camera;
    visibility.system.cameraPrev = originalSystem->cameraPrev;
    KHZ_ClientVisibilityRefresh(gameStatic);
    if (!visibility.unsupported && !migrateEffects(*originalSystem)) {
        visibility.unsupported = true;
        if (!visibility.failure[0]) visibility.failure = "native_fx_migration_reference_or_pool_validation_failed";
    }
    visibility.preparing = false;
    if (visibility.unsupported) { KHZ_ClientVisibilityRelease(); return false; }
    visibility.prepared = true;
    return true;
}
bool KHZ_ClientVisibilityRefresh(cgs_t* gameStatic) {
    iassert(Sys_IsMainThread());
    if ((!visibility.prepared && !visibility.preparing) || !gameStatic) return false;
    const bool previousPreparation = visibility.preparing;
    visibility.preparing = true; visibility.gameStatic = gameStatic;
    for (const auto* definition : gameStatic->fxs) prepareDefinition(definition);
    prepareDefinition(gameStatic->smokeGrenadeFx);
    for (unsigned i = 0; i < BG_GetNumWeapons(); ++i) {
        const auto* weapon = BG_GetWeaponDef(i);
        prepareDefinition(weapon->projExplosionEffect); prepareDefinition(weapon->projDudEffect);
        // Native MP CG_Missile spawns trail only. Ignition is an SP path.
        prepareDefinition(weapon->projTrailEffect);
        prepareDefinition(weapon->viewFlashEffect); prepareDefinition(weapon->worldFlashEffect);
    }
    if (cgMedia.fx) for (unsigned impact = 0; impact < 12; ++impact) {
        for (const auto* definition : cgMedia.fx->table[impact].nonflesh) prepareDefinition(definition);
        for (const auto* definition : cgMedia.fx->table[impact].flesh) prepareDefinition(definition);
    }
    prepareDefinition(cgMedia.fxNoBloodFleshHit);
    prepareDefinition(cgMedia.helicopterLightSmoke); prepareDefinition(cgMedia.helicopterHeavySmoke);
    prepareDefinition(cgMedia.helicopterOnFire); prepareDefinition(cgMedia.heliDustEffect);
    prepareDefinition(cgMedia.heliWaterEffect); prepareDefinition(cgMedia.fxVehicleTireDust);
    for (unsigned i = 0; i < 256; ++i) {
        const auto* text = CL_GetConfigString(0, CS_EFFECT_TAGS + i);
        if (!text[0]) {
            if (visibility.tags[i].tag) SL_RemoveRefToString(visibility.tags[i].tag);
            visibility.tags[i] = {}; continue;
        }
        if (!text[1] || text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9') {
            visibility.failure = "native_effect_tag_configstring_is_invalid"; visibility.unsupported = true; break;
        }
        const auto effect = 10 * (text[0] - '0') + text[1] - '0';
        if (visibility.tags[i].tag && visibility.tags[i].effect == static_cast<unsigned>(effect) &&
            !std::strcmp(SL_ConvertToString(visibility.tags[i].tag), text + 2)) continue;
        const auto tag = SL_GetString(text + 2, 0);
        if (visibility.tags[i].tag) SL_RemoveRefToString(visibility.tags[i].tag);
        visibility.tags[i] = {static_cast<unsigned>(effect), tag};
    }
    visibility.preparing = previousPreparation;
    return !visibility.unsupported;
}
const char* KHZ_ClientVisibilityFailureReason() { return visibility.failure ? visibility.failure : ""; }
unsigned KHZ_ClientVisibilityMigratedBolts() { return visibility.migratedBolts; }
void KHZ_ClientVisibilityRelease() {
    iassert(Sys_IsMainThread());
    for (auto& tag : visibility.tags) { if (tag.tag) SL_RemoveRefToString(tag.tag); tag = {}; }
    visibility.prepared = visibility.preparing = false;
    visibility.game = nullptr; visibility.gameStatic = nullptr; visibility.definitionCount = 0;
}
FxSystem* KHZ_ClientVisibilitySystem() { return &visibility.system; }
FxSystemBuffers* KHZ_ClientVisibilityBuffers() { return &visibility.buffers; }
bool KHZ_ClientVisibilityIsSystem(const FxSystem* system) {
    return system == &visibility.system && (visibility.prepared || visibility.preparing);
}
const FxEffectDef* KHZ_ClientVisibilityEffectDef(const FxEffectDef* original) {
    if (!original) return nullptr;
    for (unsigned i = 0; i < visibility.definitionCount; ++i) {
        const auto& definition = visibility.definitions[i];
        if (definition.original == original || &definition.filtered == original) return &definition.filtered;
    }
    if (!FX_EffectAffectsGameplay(original)) return nullptr;
    if (visibility.preparing) return prepareDefinition(original);
    KHZ_ClientOwnerRequestAssetRebuild("A gameplay visibility FX definition requires parked preparation");
    return nullptr;
}
bool KHZ_ClientVisibilityWantsElement(const FxEffectDef* definition, unsigned element) {
    for (unsigned i = 0; i < visibility.definitionCount; ++i) {
        const auto& entry = visibility.definitions[i];
        if (definition == &entry.filtered || definition == entry.original)
            return element < ElementCapacity && entry.needed[element];
    }
    return false;
}
void KHZ_ClientVisibilityEntityEvent(const centity_s* entity, int event) {
    iassert(KHZ_IsClientSimulationThread());
    if (!visibility.prepared || !entity) return;
    const auto& state = entity->nextState; const auto& game = *visibility.game;
    const bool playerView = (game.nextSnap->ps.otherFlags & 6) && state.number == game.nextSnap->ps.clientNum;
    const unsigned weaponIndex = playerView ? game.predictedPlayerState.weapon : state.weapon;
    if (!BG_ValidateWeaponNumber(weaponIndex)) { KHZ_ClientOwnerRequestAssetRebuild("Event visibility weapon is invalid"); return; }
    const auto* weapon = BG_GetWeaponDef(weaponIndex);
    float axis[3][3];
    const auto normalAxis = [&] {
        ByteToDir(state.eventParm, axis[0]); Vec3Basis_RightHanded(axis[0], axis[1], axis[2]);
    };
    switch (event) {
    case EV_FIRE_WEAPON: case EV_FIRE_WEAPON_LASTSHOT: case EV_FIRE_WEAPON_MG42:
        // Native stock flashes are cosmetic. Custom blocking flashes require
        // their real muzzle/weapon attachment descriptor, which must not be
        // approximated with an eye or root-body position.
        if (gameplayChild(playerView ? weapon->viewFlashEffect : weapon->worldFlashEffect))
            KHZ_ClientOwnerRequestPark("Blocking muzzle FX require a prepared private weapon attachment");
        break;
    case EV_CUSTOM_EXPLODE: case EV_CUSTOM_EXPLODE_NOMARKS:
        normalAxis();
        impact(weaponIndex, state.surfType, game.time, entity->pose.origin, axis);
        play(weapon->projExplosionEffect, state.lerp.u.customExplode.startTime, state.lerp.pos.trBase, axis); break;
    case EV_GRENADE_EXPLODE: case EV_ROCKET_EXPLODE: case EV_ROCKET_EXPLODE_NOMARKS:
        normalAxis();
        impact(weaponIndex, state.surfType, game.time, entity->pose.origin, axis);
        play(weapon->projExplosionEffect, game.time, entity->pose.origin, axis); break;
    case EV_FLASHBANG_EXPLODE:
        normalAxis();
        play(weapon->projExplosionEffect, game.time, entity->pose.origin, axis); break;
    case EV_DUD_EXPLODE: case EV_DUD_IMPACT:
        normalAxis();
        if (cgMedia.fx && state.surfType < SURF_TYPECOUNT) play(cgMedia.fx->table[11].nonflesh[state.surfType], game.time, entity->pose.origin, axis);
        if (event == EV_DUD_EXPLODE) play(weapon->projExplosionEffect, game.time, entity->pose.origin, axis);
        play(weapon->projDudEffect, game.time, entity->pose.origin, axis); break;
    case EV_CHANGE_TO_DUD:
        normalAxis();
        if (cgMedia.fx) play(cgMedia.fx->table[11].nonflesh[0], game.time, entity->pose.origin, axis); break;
    case EV_GRENADE_BOUNCE:
        normalAxis();
        if (cgMedia.fx && state.surfType < SURF_TYPECOUNT)
            play(cgMedia.fx->table[8].nonflesh[state.surfType], game.time, entity->pose.origin, axis);
        break;
    case EV_BULLET_HIT: {
        const FxEffectDef* definition = nullptr; snd_alias_list_t* unused = nullptr;
        if (state.surfType >= SURF_TYPECOUNT) break;
        CG_ImpactEffectForWeapon(weaponIndex, state.surfType, state.un1.scale, &definition, &unused);
        if (definition && state.surfType == 7 && !cg_blood->current.enabled) definition = cgMedia.fxNoBloodFleshHit;
        if (!gameplayChild(definition)) break;
        normalAxis();
        CG_RandomEffectAxis(axis[0], axis[1], axis[2]);
        play(definition, game.time, entity->pose.origin, axis);
        break;
    }
    case EV_PLAY_FX:
        if (state.eventParm > 0 && state.eventParm < 100) {
            AnglesToAxis(state.lerp.apos.trBase, axis);
            play(visibility.gameStatic->fxs[state.eventParm], game.time, entity->pose.origin, axis);
        } break;
    case EV_PLAY_FX_ON_TAG: {
        const auto& tag = visibility.tags[static_cast<unsigned char>(state.eventParm)];
        if (!tag.tag || tag.effect >= 100) { KHZ_ClientOwnerRequestAssetRebuild("Visibility tag effect configuration changed"); break; }
        const auto* definition = visibility.gameStatic->fxs[tag.effect];
        if (!gameplayChild(definition)) break;
        auto* object = KHZ_ClientPoseGetDObj(state.number); unsigned char bone = 0;
        if (!object || !DObjGetBoneIndex(object, tag.tag, &bone)) { KHZ_ClientOwnerRequestAssetRebuild("Visibility effect bolt lacks a private gameplay pose"); break; }
        FX_PlayBoltedEffect(0, definition, game.time, state.number, bone); break;
    }
    default: break;
    }
}
bool KHZ_ClientVisibilityCanonical(khz::canonical::Hash& hash) {
    iassert(KHZ_IsClientSimulationThread());
    if (!KHZ_IsClientSimulationThread() || !visibility.prepared || visibility.preparing || visibility.unsupported ||
        visibility.definitionCount > DefinitionCapacity) { hash.reject(); return false; }
    hash.scalar(visibility.definitionCount);
    for (unsigned i = 0; i < visibility.definitionCount; ++i) {
        const auto& definition = visibility.definitions[i];
        if (!definition.original) { hash.reject(); return false; }
        hash.text(definition.original->name); hash.scalar(definition.filtered.flags);
        const int elements = definition.filtered.elemDefCountLooping + definition.filtered.elemDefCountOneShot + definition.filtered.elemDefCountEmission;
        if (elements < 0 || elements > ElementCapacity) { hash.reject(); return false; }
        hash.scalar(elements); for (int e = 0; e < elements; ++e) hash.boolean(definition.needed[e]);
    }
    for (const auto& tag : visibility.tags) {
        hash.scalar(tag.effect); hash.text(tag.tag ? SL_ConvertToString(tag.tag) : nullptr);
    }
    const auto first = reinterpret_cast<std::uintptr_t>(visibility.system.effects);
    for (const auto& entity : visibility.entities) {
        hash.boolean(entity.effect != nullptr);
        if (!entity.effect) continue;
        const auto address = reinterpret_cast<std::uintptr_t>(entity.effect);
        if (address < first || (address - first) % sizeof(FxEffect) || (address - first) / sizeof(FxEffect) >= 1024) {
            hash.reject(); return false;
        }
        hash.scalar(static_cast<unsigned>((address - first) / sizeof(FxEffect)));
        hash.scalar(entity.trigger); hash.scalar(entity.type); hash.scalar(entity.definition); hash.boolean(entity.seen);
    }
    return KHZ_CanonicalGameplayFX(hash, visibility.system);
}

bool KHZ_ClientVisibilityTick() {
    iassert(KHZ_IsClientSimulationThread());
    if (!visibility.prepared) return false;
    auto& game = *visibility.game; auto& system = visibility.system;
    if (game.time < system.msecNow) return false;
    system.cameraPrev = system.camera;
    FX_SetNextUpdateTime(0, game.time);
    FX_SetNextUpdateCamera(0, &game.refdef, 0.f);
    for (auto& entity : visibility.entities) entity.seen = false;
    for (int i = 0; i < game.nextSnap->numEntities; ++i) {
        const auto& state = game.nextSnap->entities[i];
        if (state.eType == ET_MISSILE && BG_ValidateWeaponNumber(state.weapon)) {
            const auto* weapon = BG_GetWeaponDef(state.weapon);
            if (gameplayChild(weapon->projTrailEffect) && !(state.lerp.eFlags & 0x20) &&
                state.lerp.u.missile.launchTime <= game.time && KHZ_ClientProjectileTrailPending(state.number)) {
                auto* object = KHZ_ClientProjectileGetDObj(state.number);
                unsigned char bone = 254;
                // Native marks the trail made even when tag_fx is absent. That
                // case spawns nothing; it never falls back to the model root.
                KHZ_ClientProjectileMarkTrail(state.number);
                if (object && DObjGetBoneIndex(object, scr_const.tag_fx, &bone))
                    FX_PlayBoltedEffect(0, weapon->projTrailEffect, game.time, state.number, bone);
            }
        }
        if (state.eType != ET_FX && state.eType != ET_LOOP_FX) continue;
        auto& local = visibility.entities[state.number]; local.seen = true;
        const unsigned id = state.un1.scale;
        if (!id || id >= 100) return false;
        const auto* definition = visibility.gameStatic->fxs[id];
        if (!gameplayChild(definition)) {
            if (local.effect) { FX_ThroughWithEffect(0, local.effect); local = {}; }
            continue;
        }
        auto* entity = CG_GetEntity(0, state.number);
        CG_CalcEntityLerpPositions(0, entity);
        float axis[3][3]; AnglesToAxis(state.lerp.apos.trBase, axis);
        if (state.eType == ET_FX) {
            if (!local.effect || local.type != state.eType || local.definition != id || local.trigger != state.time2) {
                if (local.effect) FX_ThroughWithEffect(0, local.effect);
                local.effect = FX_SpawnOrientedEffect(0, definition, state.time2, entity->pose.origin, axis, ENTITYNUM_NONE);
                local.trigger = state.time2; local.type = state.eType; local.definition = id;
            }
        } else {
            const int period = state.lerp.u.loopFx.period;
            if (period <= 0) return false;
            float difference[3]; Vec3Sub(entity->pose.origin, game.predictedPlayerState.origin, difference);
            const float distance = state.lerp.u.loopFx.cullDist;
            if (distance && Vec3LengthSq(difference) >= distance * distance) continue;
            if (!local.effect || local.type != state.eType || local.definition != id) {
                if (local.effect) FX_ThroughWithEffect(0, local.effect);
                local.effect = FX_SpawnOrientedEffect(0, definition, game.time, entity->pose.origin, axis, ENTITYNUM_NONE);
                local.trigger = game.time + period; local.type = state.eType; local.definition = id;
            }
            while (local.effect && game.time >= local.trigger) {
                FX_RetriggerEffect(0, local.effect, local.trigger); local.trigger += period;
            }
        }
    }
    for (auto& entity : visibility.entities) if (entity.effect && !entity.seen) {
        FX_ThroughWithEffect(0, entity.effect); entity = {};
    }
    FX_Update(&system, 0, false);
    system.visStateBufferWrite->blockerCount = 0;
    for (int active = system.firstActiveEffect; active != system.firstNewEffect; ++active) {
        auto* effect = FX_EffectFromHandle(&system, system.allEffectHandles[active & 1023]);
        for (unsigned list = 0; list < 3; ++list) {
            for (unsigned short handle = effect->firstElemHandle[list]; handle != FX_HANDLE_NONE; ) {
                const auto* element = &FX_PoolFromHandle_Generic<FxElem, 2048>(system.elems, handle)->item;
                if (!evaluateElement(effect, &effect->def->elemDefs[element->defIndex], element->msecBegin,
                    element->sequence, element->origin)) return false;
                handle = element->nextElemHandleInEffect;
            }
        }
        for (unsigned short trailHandle = effect->firstTrailHandle; trailHandle != FX_HANDLE_NONE; ) {
            const auto* trail = &FX_PoolFromHandle_Generic<FxTrail, 128>(system.trails, trailHandle)->item;
            for (unsigned short handle = trail->firstElemHandle; handle != FX_HANDLE_NONE; ) {
                const auto* element = &FX_PoolFromHandle_Generic<FxTrailElem, 2048>(system.trailElems, handle)->item;
                if (!evaluateElement(effect, &effect->def->elemDefs[trail->defIndex], element->msecBegin,
                    element->sequence, element->origin)) return false;
                handle = element->nextTrailElemHandle;
            }
            trailHandle = trail->nextTrailHandle;
        }
    }
    // Swap only this private domain; FX_ToggleVisBlockerFrame also writes a
    // process-global server client index and is deliberately not called here.
    const auto* previous = system.visStateBufferRead;
    system.visStateBufferRead = system.visStateBufferWrite;
    system.visStateBufferWrite = const_cast<FxVisState*>(previous);
    system.visStateBufferWrite->blockerCount = 0;
    return !KHZ_ClientOwnerNeedsMainThread();
}

namespace {
bool visibilityCheckpointReject(char* reason, unsigned capacity, const char* text) {
    if (reason && capacity) std::snprintf(reason, capacity, "%s", text); return false;
}
void visibilityCheckpointDiscard() {
    for (unsigned i = 0; i < checkpointTagPins; ++i)
        if (checkpointVisibility.tags[i].tag) SL_RemoveRefToString(checkpointVisibility.tags[i].tag);
    checkpointTagPins = 0; checkpointCaptured = false;
}
bool visibilityCheckpointCapture(const KHZ_ClientCheckpointFrame&, char* reason, unsigned capacity) {
    khz::canonical::Hash hash;
    if (!KHZ_ClientOwnerCheckpointContext() || checkpointCaptured || visibility.preparing || visibility.unsupported ||
        visibility.definitionCount > DefinitionCapacity || !KHZ_ClientVisibilityCanonical(hash))
        return visibilityCheckpointReject(reason, capacity, "visibility_checkpoint_native_pools_or_boundary_invalid");
    std::memcpy(&checkpointVisibility, &visibility, sizeof(visibility));
    for (unsigned i = 0; i < ARRAY_COUNT(visibility.tags); ++i) {
        if (visibility.tags[i].tag) SL_AddRefToString(visibility.tags[i].tag);
        checkpointTagPins = i + 1;
    }
    checkpointCaptured = true; return true;
}
bool visibilityCheckpointValidate(const KHZ_ClientCheckpointFrame&, char* reason, unsigned capacity) {
    khz::canonical::Hash hash;
    if (!KHZ_ClientOwnerCheckpointContext() || !checkpointCaptured || visibility.preparing || visibility.unsupported ||
        visibility.game != checkpointVisibility.game || visibility.gameStatic != checkpointVisibility.gameStatic ||
        visibility.definitionCount != checkpointVisibility.definitionCount || !KHZ_ClientVisibilityCanonical(hash))
        return visibilityCheckpointReject(reason, capacity, "visibility_checkpoint_storage_or_immutable_asset_registry_changed");
    for (unsigned i = 0; i < visibility.definitionCount; ++i) {
        const auto& current = visibility.definitions[i]; const auto& saved = checkpointVisibility.definitions[i];
        if (current.original != saved.original || current.filtered.elemDefs != saved.filtered.elemDefs ||
            current.filtered.name != saved.filtered.name || std::memcmp(current.needed, saved.needed, sizeof(current.needed)))
            return visibilityCheckpointReject(reason, capacity, "visibility_checkpoint_filtered_native_definition_changed");
    }
    return true;
}
bool visibilityCheckpointRestore(const KHZ_ClientCheckpointFrame&, std::uint64_t, char* reason, unsigned capacity) {
    if (!KHZ_ClientOwnerCheckpointContext() || !checkpointCaptured)
        return visibilityCheckpointReject(reason, capacity, "visibility_checkpoint_restore_not_prevalidated_on_worker");
    for (auto& tag : visibility.tags) if (tag.tag) SL_RemoveRefToString(tag.tag);
    std::memcpy(&visibility, &checkpointVisibility, sizeof(visibility));
    for (auto& tag : visibility.tags) if (tag.tag) SL_AddRefToString(tag.tag);
    return true;
}
}
void KHZ_ClientVisibilityRegisterCheckpoint() {
    const KHZ_ClientCheckpointProvider provider{KHZ_ClientCheckpointDomain::GameplayEffects, sizeof(checkpointVisibility),
        visibilityCheckpointCapture, visibilityCheckpointValidate, visibilityCheckpointRestore, visibilityCheckpointDiscard};
    KHZ_ClientCheckpointRegister(provider);
}
