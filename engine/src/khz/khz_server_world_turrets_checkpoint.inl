// Include at END of game_mp/g_misc_mp.cpp under COD4X1KHZ && KISAK_MP.
#include "khz_server_world_checkpoint.h"
namespace {
using namespace khz::server_world_checkpoint;
Bytes<sizeof(turretInfo)> khzWorldTurretImage;
KHZ_ServerCheckpointFrame khzWorldTurretFrame;
bool khzWorldTurretCaptured;
bool khzWorldTurretState(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!frame(f, text, capacity)) return false;
    unsigned char referenced[32]{};
    for (const auto& ent : g_entities) if (ent.pTurretInfo) {
        if (!element(ent.pTurretInfo, turretInfo, false))
            return reject(text, capacity, "server_world_turret_entity_reference_outside_native_pool");
        const auto index = ent.pTurretInfo - turretInfo;
        if (referenced[index] || !ent.r.inuse || !ent.pTurretInfo->inuse)
            return reject(text, capacity, "server_world_turret_entity_reference_duplicate_or_inactive");
        referenced[index] = 1;
    }
    for (unsigned i = 0; i < 32; ++i)
        if ((turretInfo[i].inuse != 0 && turretInfo[i].inuse != 1) || bool(turretInfo[i].inuse) != bool(referenced[i]))
            return reject(text, capacity, "server_world_turret_native_pool_coverage_incomplete");
    return true;
}
bool khzWorldTurretCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldTurretState(f, text, capacity)) return false;
    khzWorldTurretImage.capture(turretInfo); khzWorldTurretFrame = f; khzWorldTurretCaptured = true; return true;
}
bool khzWorldTurretValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldTurretCaptured || !khz::server_checkpoint::sameLifetime(khzWorldTurretFrame, f))
        return reject(text, capacity, "server_world_turret_image_lifetime_missing_or_changed");
    return khzWorldTurretState(f, text, capacity);
}
void khzWorldTurretRestore(const KHZ_ServerCheckpointFrame&) { iassert(KHZ_ServerCheckpointContext()); khzWorldTurretImage.restore(turretInfo); }
void khzWorldTurretDiscard() { khzWorldTurretCaptured = false; }
}
void KHZ_ServerWorldTurretsRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::World, 3, sizeof(khzWorldTurretImage),
        khzWorldTurretCapture, khzWorldTurretValidate, khzWorldTurretRestore, khzWorldTurretDiscard};
    registerPart(p, turretInfo, sizeof(turretInfo));
}
