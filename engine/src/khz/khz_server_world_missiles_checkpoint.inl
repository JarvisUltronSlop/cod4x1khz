// Include at END of game/g_missile.cpp under COD4X1KHZ && KISAK_MP.
#include "khz_server_world_checkpoint.h"
namespace {
using namespace khz::server_world_checkpoint;
Bytes<sizeof(attrGlob)> khzWorldAttractorsImage;
KHZ_ServerCheckpointFrame khzWorldMissilesFrame;
float khzWorldJavelinOffset, khzWorldJavelinOffsetRight;
bool khzWorldMissilesCaptured;
bool khzWorldMissilesState(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!frame(f, text, capacity)) return false;
    for (const auto& attractor : attrGlob.attractors) if (attractor.inUse) {
        if (attractor.entnum < 0 || attractor.entnum >= MAX_GENTITIES ||
            !finite3(attractor.origin) || !std::isfinite(attractor.strength) ||
            !std::isfinite(attractor.maxDist) || attractor.maxDist <= 0.f)
            return reject(text, capacity, "server_world_missile_attractor_native_state_invalid");
    }
    return std::isfinite(MYJAVELINOFFSET) && std::isfinite(MYJAVELINOFFSET_RIGHT) ? true :
        reject(text, capacity, "server_world_missile_native_tuning_invalid");
}
bool khzWorldMissilesCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldMissilesState(f, text, capacity)) return false;
    khzWorldAttractorsImage.capture(&attrGlob);
    khzWorldJavelinOffset = MYJAVELINOFFSET; khzWorldJavelinOffsetRight = MYJAVELINOFFSET_RIGHT;
    khzWorldMissilesFrame = f; khzWorldMissilesCaptured = true; return true;
}
bool khzWorldMissilesValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldMissilesCaptured || !khz::server_checkpoint::sameLifetime(khzWorldMissilesFrame, f))
        return reject(text, capacity, "server_world_missile_image_lifetime_missing_or_changed");
    return khzWorldMissilesState(f, text, capacity);
}
void khzWorldMissilesRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext());
    khzWorldAttractorsImage.restore(&attrGlob);
    MYJAVELINOFFSET = khzWorldJavelinOffset; MYJAVELINOFFSET_RIGHT = khzWorldJavelinOffsetRight;
}
void khzWorldMissilesDiscard() { khzWorldMissilesCaptured = false; }
}
void KHZ_ServerWorldMissilesRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::World, 5,
        sizeof(khzWorldAttractorsImage) + sizeof(khzWorldJavelinOffset) + sizeof(khzWorldJavelinOffsetRight),
        khzWorldMissilesCapture, khzWorldMissilesValidate, khzWorldMissilesRestore, khzWorldMissilesDiscard};
    registerPart(p, &attrGlob, sizeof(attrGlob));
    if (!KHZ_ServerCheckpointRegisterRange(p.domain, &MYJAVELINOFFSET, sizeof(MYJAVELINOFFSET)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &MYJAVELINOFFSET_RIGHT, sizeof(MYJAVELINOFFSET_RIGHT)))
        Com_Error(ERR_FATAL, "Could not register native World missile tuning storage");
}
