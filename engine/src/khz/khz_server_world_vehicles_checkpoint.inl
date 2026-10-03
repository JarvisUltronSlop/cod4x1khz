// Include at END of game_mp/g_vehicles_mp.cpp under COD4X1KHZ && KISAK_MP.
#include "khz_server_world_checkpoint.h"
namespace {
using namespace khz::server_world_checkpoint;
Bytes<sizeof(s_vehicles)> khzWorldVehiclesImage;
Bytes<sizeof(vehEffects)> khzWorldVehicleEffectsImage;
KHZ_ServerCheckpointFrame khzWorldVehiclesFrame;
bool khzWorldVehiclesCaptured;
bool khzWorldVehiclesState(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!frame(f, text, capacity)) return false;
    if (level.vehicles != s_vehicles || s_numVehicleInfos < 0 || s_numVehicleInfos > 32)
        return reject(text, capacity, "server_world_vehicle_pool_or_definition_count_invalid");
    unsigned char referenced[8]{};
    for (const auto& ent : g_entities) if (ent.scr_vehicle) {
        if (!element(ent.scr_vehicle, s_vehicles, false))
            return reject(text, capacity, "server_world_vehicle_entity_reference_outside_native_pool");
        const auto index = ent.scr_vehicle - s_vehicles;
        if (referenced[index] || !ent.r.inuse || ent.scr_vehicle->entNum != ent.s.number)
            return reject(text, capacity, "server_world_vehicle_entity_reference_duplicate_or_nonreciprocal");
        referenced[index] = 1;
    }
    for (unsigned i = 0; i < 8; ++i) {
        const auto& vehicle = s_vehicles[i];
        if (vehicle.entNum == ENTITYNUM_NONE) {
            if (referenced[i]) return reject(text, capacity, "server_world_unused_vehicle_has_live_entity_reference");
            continue;
        }
        if (!referenced[i] || vehicle.entNum < 0 || vehicle.entNum >= MAX_GENTITIES ||
            vehicle.infoIdx < 0 || vehicle.infoIdx >= s_numVehicleInfos ||
            !finite3(vehicle.phys.origin) || !finite3(vehicle.phys.angles) ||
            !finite3(vehicle.phys.vel) || !finite3(vehicle.phys.rotVel))
            return reject(text, capacity, "server_world_vehicle_native_identity_or_physics_invalid");
        for (const auto& rider : vehicle.boneIndex.riderSlots)
            if (rider.entNum != ENTITYNUM_NONE && (rider.entNum < 0 || rider.entNum >= level.maxclients))
                return reject(text, capacity, "server_world_vehicle_native_rider_index_invalid");
    }
    // This is the dedicated fixture. Active client vehicle FX would require a
    // separate client renderer/sound lifetime and cannot be replayed here.
    for (const auto& effects : vehEffects[0]) if (effects.active || effects.soundPlaying)
        return reject(text, capacity, "server_world_dedicated_fixture_contains_active_client_vehicle_effects");
    return true;
}
bool khzWorldVehiclesCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldVehiclesState(f, text, capacity)) return false;
    khzWorldVehiclesImage.capture(s_vehicles); khzWorldVehicleEffectsImage.capture(vehEffects);
    khzWorldVehiclesFrame = f; khzWorldVehiclesCaptured = true; return true;
}
bool khzWorldVehiclesValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldVehiclesCaptured || !khz::server_checkpoint::sameLifetime(khzWorldVehiclesFrame, f))
        return reject(text, capacity, "server_world_vehicle_image_lifetime_missing_or_changed");
    return khzWorldVehiclesState(f, text, capacity);
}
void khzWorldVehiclesRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext());
    khzWorldVehiclesImage.restore(s_vehicles); khzWorldVehicleEffectsImage.restore(vehEffects);
}
void khzWorldVehiclesDiscard() { khzWorldVehiclesCaptured = false; }
}
void KHZ_ServerWorldVehiclesRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::World, 4,
        sizeof(khzWorldVehiclesImage) + sizeof(khzWorldVehicleEffectsImage),
        khzWorldVehiclesCapture, khzWorldVehiclesValidate, khzWorldVehiclesRestore, khzWorldVehiclesDiscard};
    registerPart(p, s_vehicles, sizeof(s_vehicles));
    if (!KHZ_ServerCheckpointRegisterRange(p.domain, vehEffects, sizeof(vehEffects)))
        Com_Error(ERR_FATAL, "Could not register native World vehicle effect counters");
}
