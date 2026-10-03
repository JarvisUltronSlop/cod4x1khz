// Source-owned fragment: END of server/sv_world.cpp, opt-in custom MP/dedicated.
// Native actor bounds are real globals; all other SV_Trace leaf/clip locals
// are stack-owned. Entity links/caches belong to Collision part1, not here.
#include "khz_server_collision_checkpoint_util.h"
#include <qcommon/threads.h>
#include <server_mp/server_mp.h>
#include <game_mp/g_public_mp.h>

namespace khz_server_collision_query_detail {
using namespace khz::server_collision_checkpoint;
struct BrushPin {
    float origin[3], angles[3], mins[3], maxs[3];
    int model, useCount;
    bool present;
};
struct Image {
    Bytes<sizeof(actorLocationalMins)> mins;
    Bytes<sizeof(actorLocationalMaxs)> maxs;
    BrushPin brushes[MAX_GENTITIES];
    KHZ_ServerCheckpointFrame frame;
    bool captured;
};
Image image;
bool nativeEntity(const gentity_s* ent) { return element(ent, g_entities, MAX_GENTITIES); }
bool sameBrush(const gentity_s& ent, const BrushPin& pin) {
    return pin.present && ent.r.inuse && ent.r.bmodel && ent.s.index.brushmodel == pin.model &&
        ent.useCount == pin.useCount && ent.s.lerp.pos.trType == TR_STATIONARY && ent.s.lerp.apos.trType == TR_STATIONARY &&
        !std::memcmp(ent.r.currentOrigin, pin.origin, sizeof(pin.origin)) &&
        !std::memcmp(ent.r.currentAngles, pin.angles, sizeof(pin.angles)) &&
        !std::memcmp(ent.r.mins, pin.mins, sizeof(pin.mins)) && !std::memcmp(ent.r.maxs, pin.maxs, sizeof(pin.maxs));
}
bool state(const KHZ_ServerCheckpointFrame& f, char* reason, unsigned capacity) {
    if (!context(reason, capacity) || !KHZ_ServerCollisionIdle(reason, capacity)) return false;
    if (f.entitiesAddress != reinterpret_cast<std::uintptr_t>(g_entities) || sv.gentities != g_entities ||
        sv.state != SS_GAME || sv.inFrame || sv.restarting || level.currentEntityThink != -1 || bgs ||
        !bounds(actorLocationalMins, actorLocationalMaxs))
        return reject(reason, capacity, "server_collision_sv_query_not_completed_native_owner_or_bounds_invalid");
    for (unsigned i = 0; i < MAX_GENTITIES; ++i) {
        const auto& ent = g_entities[i]; if (!ent.r.inuse) continue;
        if (ent.s.number != i || !finite3(ent.r.currentOrigin) || !finite3(ent.r.currentAngles) ||
            !bounds(ent.r.mins, ent.r.maxs) || (ent.r.linked && !bounds(ent.r.absmin, ent.r.absmax)))
            return reject(reason, capacity, "server_collision_entity_identity_transform_or_bounds_invalid");
        if (ent.physicsObject || ent.scr_vehicle || ent.s.eType == ET_VEHICLE || ent.s.eType == ET_VEHICLE_COLLMAP ||
            ent.s.eType == ET_VEHICLE_CORPSE || ent.s.eType == ET_HELICOPTER || ent.s.eType == ET_PLANE)
            return reject(reason, capacity, "server_collision_entity_external_physics_or_vehicle_query_requires_provider");
        if (ent.r.bmodel && (ent.tagInfo || ent.s.index.brushmodel < 0 || unsigned(ent.s.index.brushmodel) >= cm.numSubModels ||
            ent.s.lerp.pos.trType != TR_STATIONARY || ent.s.lerp.apos.trType != TR_STATIONARY))
            return reject(reason, capacity, "server_collision_moving_or_foreign_brush_geometry_is_unsupported");
    }
    return true;
}
bool capture(const KHZ_ServerCheckpointFrame& f, char* reason, unsigned capacity) {
    if (!state(f, reason, capacity)) return false;
    std::memset(image.brushes, 0, sizeof(image.brushes));
    for (unsigned i = 0; i < MAX_GENTITIES; ++i) if (g_entities[i].r.inuse && g_entities[i].r.bmodel) {
        const auto& ent = g_entities[i]; auto& pin = image.brushes[i];
        std::memcpy(pin.origin, ent.r.currentOrigin, sizeof(pin.origin));
        std::memcpy(pin.angles, ent.r.currentAngles, sizeof(pin.angles));
        std::memcpy(pin.mins, ent.r.mins, sizeof(pin.mins)); std::memcpy(pin.maxs, ent.r.maxs, sizeof(pin.maxs));
        pin.model = ent.s.index.brushmodel; pin.useCount = ent.useCount; pin.present = true;
    }
    image.mins.capture(actorLocationalMins); image.maxs.capture(actorLocationalMaxs);
    image.frame = f; image.captured = true; return true;
}
bool validate(const KHZ_ServerCheckpointFrame& f, char* reason, unsigned capacity) {
    if (!image.captured || !khz::server_checkpoint::sameLifetime(image.frame, f))
        return reject(reason, capacity, "server_collision_sv_query_image_missing_or_map_lifetime_changed");
    if (!state(f, reason, capacity)) return false;
    for (unsigned i = 0; i < MAX_GENTITIES; ++i) {
        const auto& ent = g_entities[i]; const auto& pin = image.brushes[i];
        if (pin.present != bool(ent.r.inuse && ent.r.bmodel) || (pin.present && !sameBrush(ent, pin)))
            return reject(reason, capacity, "server_collision_stationary_brush_identity_geometry_or_transform_changed");
    }
    return true;
}
void restore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext() && image.captured);
    image.mins.restore(actorLocationalMins); image.maxs.restore(actorLocationalMaxs);
    // BrushPin is a guard/pin, not another restored entity arena. World part0
    // restores the exact original entities; Collision part1 restores links.
}
void discard() { image.captured = false; }
}
void KHZ_ServerCollisionQueryRegisterCheckpoint() {
    using namespace khz_server_collision_query_detail;
    registerPart({KHZ_ServerCheckpointDomain::Collision, 2, sizeof(image), capture, validate, restore, discard});
    if (!KHZ_ServerCheckpointRegisterRange(KHZ_ServerCheckpointDomain::Collision, actorLocationalMins, sizeof(actorLocationalMins)) ||
        !KHZ_ServerCheckpointRegisterRange(KHZ_ServerCheckpointDomain::Collision, actorLocationalMaxs, sizeof(actorLocationalMaxs)))
        Com_Error(ERR_FATAL, "Could not register native server locational collision bounds");
}
bool KHZ_ServerCollisionAllowBrushTransform(const gentity_s* ent, const float* proposedOrigin,
    const float* proposedAngles, const char* operation) {
    using namespace khz_server_collision_query_detail;
    if (!Sys_IsMainThread()) return KHZ_ServerCollisionAllowUnsupportedEffect(operation);
    if (!KHZ_ServerCheckpointPinned()) return true;
    if (!nativeEntity(ent)) return KHZ_ServerCollisionAllowUnsupportedEffect(operation);
    if (!ent->r.bmodel) return true;
    const auto& pin = image.brushes[ent - g_entities];
    if (image.captured && sameBrush(*ent, pin) &&
        (!proposedOrigin || !std::memcmp(proposedOrigin, pin.origin, sizeof(pin.origin))) &&
        (!proposedAngles || !std::memcmp(proposedAngles, pin.angles, sizeof(pin.angles)))) return true;
    return KHZ_ServerCollisionAllowUnsupportedEffect(operation ? operation : "server_collision_moving_brush_effect_refused_before_mutation");
}
bool KHZ_ServerCollisionAllowEntityLink(const gentity_s* ent, const char* operation) {
    using namespace khz_server_collision_query_detail;
    if (!Sys_IsMainThread()) return KHZ_ServerCollisionAllowUnsupportedEffect(operation);
    if (!KHZ_ServerCheckpointPinned()) return true;
    if (!nativeEntity(ent) || !ent->r.inuse || ent->physicsObject || ent->scr_vehicle ||
        ent->s.eType == ET_VEHICLE || ent->s.eType == ET_VEHICLE_COLLMAP || ent->s.eType == ET_VEHICLE_CORPSE ||
        ent->s.eType == ET_HELICOPTER || ent->s.eType == ET_PLANE)
        return KHZ_ServerCollisionAllowUnsupportedEffect(operation ? operation : "server_collision_foreign_or_external_physics_entity_link_refused");
    return KHZ_ServerCollisionAllowBrushTransform(ent, ent->r.currentOrigin, ent->r.currentAngles, operation);
}
bool KHZ_ServerCollisionAllowBrushMutation(const gentity_s* ent, const char* operation) {
    using namespace khz_server_collision_query_detail;
    if (!Sys_IsMainThread()) return KHZ_ServerCollisionAllowUnsupportedEffect(operation);
    if (!KHZ_ServerCheckpointPinned()) return true;
    if (nativeEntity(ent) && !ent->r.bmodel) return true;
    return KHZ_ServerCollisionAllowUnsupportedEffect(operation ? operation : "server_collision_brush_geometry_mutation");
}
bool KHZ_ServerCollisionAllowTrajectoryMutation(const trajectory_t* trajectory, const char* operation) {
    using namespace khz_server_collision_query_detail;
    if (!Sys_IsMainThread()) return KHZ_ServerCollisionAllowUnsupportedEffect(operation);
    if (!KHZ_ServerCheckpointPinned()) return true;
    for (unsigned i = 0; i < MAX_GENTITIES; ++i) {
        const auto* ent = &g_entities[i];
        if (trajectory == &ent->s.lerp.pos || trajectory == &ent->s.lerp.apos)
            return KHZ_ServerCollisionAllowBrushMutation(ent, operation);
    }
    // A caller-owned or heap trajectory is not an advertised restored arena.
    return KHZ_ServerCollisionAllowUnsupportedEffect(operation ? operation : "server_collision_foreign_trajectory_mutation");
}

