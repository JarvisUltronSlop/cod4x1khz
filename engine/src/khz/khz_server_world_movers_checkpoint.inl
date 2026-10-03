// Include at END of game/g_mover.cpp under COD4X1KHZ && KISAK_MP.
#include "khz_server_world_checkpoint.h"
namespace {
using namespace khz::server_world_checkpoint;
Bytes<sizeof(pushed)> khzWorldPushedImage;
KHZ_ServerCheckpointFrame khzWorldMoversFrame;
pushed_t* khzWorldPushedCursor;
bool khzWorldMoversCaptured;
bool khzWorldMoversState(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!frame(f, text, capacity)) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(pushed), cursor = reinterpret_cast<std::uintptr_t>(pushed_p);
    if (pushed_p && (cursor < base || cursor - base > sizeof(pushed) || (cursor - base) % sizeof(pushed_t)))
        return reject(text, capacity, "server_world_mover_push_cursor_outside_original_pool");
    const unsigned used = pushed_p ? unsigned((cursor - base) / sizeof(pushed_t)) : 0;
    for (unsigned i = 0; i < 1024; ++i) {
        if (!entity(pushed[i].ent)) return reject(text, capacity, "server_world_mover_pushed_entity_outside_original_arena");
        if (i < used && (!pushed[i].ent || !finite3(pushed[i].origin) || !finite3(pushed[i].angles) ||
            !finite3(pushed[i].surfaceNormal) || !std::isfinite(pushed[i].deltayaw)))
            return reject(text, capacity, "server_world_mover_used_push_record_incomplete");
    }
    return true;
}
bool khzWorldMoversCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldMoversState(f, text, capacity)) return false;
    khzWorldPushedImage.capture(pushed); khzWorldPushedCursor = pushed_p;
    khzWorldMoversFrame = f; khzWorldMoversCaptured = true; return true;
}
bool khzWorldMoversValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldMoversCaptured || !khz::server_checkpoint::sameLifetime(khzWorldMoversFrame, f))
        return reject(text, capacity, "server_world_mover_image_lifetime_missing_or_changed");
    return khzWorldMoversState(f, text, capacity);
}
void khzWorldMoversRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext()); khzWorldPushedImage.restore(pushed); pushed_p = khzWorldPushedCursor;
}
void khzWorldMoversDiscard() { khzWorldMoversCaptured = false; }
}
void KHZ_ServerWorldMoversRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::World, 6,
        sizeof(khzWorldPushedImage) + sizeof(khzWorldPushedCursor),
        khzWorldMoversCapture, khzWorldMoversValidate, khzWorldMoversRestore, khzWorldMoversDiscard};
    registerPart(p, pushed, sizeof(pushed));
    if (!KHZ_ServerCheckpointRegisterRange(p.domain, &pushed_p, sizeof(pushed_p)))
        Com_Error(ERR_FATAL, "Could not register native World mover push cursor");
}
