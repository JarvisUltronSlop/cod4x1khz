// Include at END of game/g_scr_vehicle.cpp under COD4X1KHZ && KISAK_MP.
// These are the real MP vehicle definitions/counters and collision/rollback
// scratch, separately owned from game_mp/g_vehicles_mp.cpp's eight vehicles.
#include "khz_server_world_checkpoint.h"
namespace {
using namespace khz::server_world_checkpoint;
Bytes<sizeof(s_vehicleInfos)> khzWorldVehicleDefinitionsImage;
Bytes<sizeof(s_phys)> khzWorldVehiclePhysicsImage;
Bytes<sizeof(s_backup)> khzWorldVehicleBackupImage;
Bytes<sizeof(s_wheelTags)> khzWorldVehicleWheelTagsImage;
Bytes<sizeof(s_flashTags)> khzWorldVehicleFlashTagsImage;
KHZ_ServerCheckpointFrame khzWorldVehiclePhysicsFrame;
short khzWorldVehicleDefinitionCount;
const dmaterial_t* khzWorldVehicleMaterials;
unsigned khzWorldVehicleMaterialCount;
bool khzWorldVehiclePhysicsCaptured;
bool khzWorldVehiclePhysicsState(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!frame(f, text, capacity)) return false;
    if (s_numVehicleInfos < 0 || s_numVehicleInfos > 32 || !cm.isInUse ||
        (cm.numMaterials && !cm.materials) || cm.numMaterials > 1048576)
        return reject(text, capacity, "server_world_vehicle_definition_or_pinned_bsp_material_pool_invalid");
    for (int i = 0; i < s_numVehicleInfos; ++i) {
        const auto& info = s_vehicleInfos[i];
        if (info.type < 0 || info.type >= NUM_VEHICLE_TYPES || !std::memchr(info.name, 0, sizeof(info.name)) ||
            !std::memchr(info.turretWeapon, 0, sizeof(info.turretWeapon)))
            return reject(text, capacity, "server_world_vehicle_definition_native_type_or_name_invalid");
        for (const auto& name : info.sndNames) if (!std::memchr(name, 0, sizeof(name)))
            return reject(text, capacity, "server_world_vehicle_sound_name_not_bounded");
    }
    if (s_phys.groundTrace.material) {
        bool found = false;
        // The pointer must be the actual start of a name in the pinned BSP's
        // concrete immutable material array, never just somewhere in hunk.
        for (unsigned i = 0; i < cm.numMaterials; ++i)
            if (s_phys.groundTrace.material == cm.materials[i].material) {
                found = std::memchr(cm.materials[i].material, 0, sizeof(cm.materials[i].material)) != nullptr; break;
            }
        if (!found) return reject(text, capacity, "server_world_vehicle_trace_material_outside_pinned_bsp_names_unsupported");
    }
    const std::uint16_t* wheelTags[4] = {&scr_const.tag_wheel_front_left, &scr_const.tag_wheel_front_right,
        &scr_const.tag_wheel_back_left, &scr_const.tag_wheel_back_right};
    const std::uint16_t* flashTags[5] = {&scr_const.tag_flash, &scr_const.tag_flash_11, &scr_const.tag_flash_2,
        &scr_const.tag_flash_22, &scr_const.tag_flash_3};
    for (unsigned i = 0; i < 4; ++i) if (s_wheelTags[i] != wheelTags[i])
        return reject(text, capacity, "server_world_vehicle_wheel_tag_constant_identity_changed");
    for (unsigned i = 0; i < 5; ++i) if (s_flashTags[i] != flashTags[i])
        return reject(text, capacity, "server_world_vehicle_flash_tag_constant_identity_changed");
    return true;
}
bool khzWorldVehiclePhysicsCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldVehiclePhysicsState(f, text, capacity)) return false;
    khzWorldVehicleDefinitionsImage.capture(s_vehicleInfos); khzWorldVehiclePhysicsImage.capture(&s_phys);
    khzWorldVehicleBackupImage.capture(&s_backup); khzWorldVehicleWheelTagsImage.capture(s_wheelTags);
    khzWorldVehicleFlashTagsImage.capture(s_flashTags); khzWorldVehicleDefinitionCount = s_numVehicleInfos;
    khzWorldVehicleMaterials = cm.materials; khzWorldVehicleMaterialCount = cm.numMaterials;
    khzWorldVehiclePhysicsFrame = f; khzWorldVehiclePhysicsCaptured = true; return true;
}
bool khzWorldVehiclePhysicsValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldVehiclePhysicsCaptured || !khz::server_checkpoint::sameLifetime(khzWorldVehiclePhysicsFrame, f) ||
        khzWorldVehicleMaterials != cm.materials || khzWorldVehicleMaterialCount != cm.numMaterials)
        return reject(text, capacity, "server_world_vehicle_physics_image_or_material_lifetime_changed");
    return khzWorldVehiclePhysicsState(f, text, capacity);
}
void khzWorldVehiclePhysicsRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext());
    khzWorldVehicleDefinitionsImage.restore(s_vehicleInfos); khzWorldVehiclePhysicsImage.restore(&s_phys);
    khzWorldVehicleBackupImage.restore(&s_backup); khzWorldVehicleWheelTagsImage.restore(s_wheelTags);
    khzWorldVehicleFlashTagsImage.restore(s_flashTags); s_numVehicleInfos = khzWorldVehicleDefinitionCount;
}
void khzWorldVehiclePhysicsDiscard() { khzWorldVehiclePhysicsCaptured = false; }
}
void KHZ_ServerWorldVehiclePhysicsRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::World, 7,
        sizeof(khzWorldVehicleDefinitionsImage) + sizeof(khzWorldVehiclePhysicsImage) + sizeof(khzWorldVehicleBackupImage) +
        sizeof(khzWorldVehicleWheelTagsImage) + sizeof(khzWorldVehicleFlashTagsImage) + sizeof(khzWorldVehicleDefinitionCount),
        khzWorldVehiclePhysicsCapture, khzWorldVehiclePhysicsValidate, khzWorldVehiclePhysicsRestore, khzWorldVehiclePhysicsDiscard};
    registerPart(p, s_vehicleInfos, sizeof(s_vehicleInfos));
    if (!KHZ_ServerCheckpointRegisterRange(p.domain, &s_phys, sizeof(s_phys)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &s_backup, sizeof(s_backup)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &s_numVehicleInfos, sizeof(s_numVehicleInfos)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, s_wheelTags, sizeof(s_wheelTags)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, s_flashTags, sizeof(s_flashTags)))
        Com_Error(ERR_FATAL, "Could not register native World vehicle physics/backup/definition spans");
}
