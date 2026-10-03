// Include at END of game/g_hudelem.cpp under COD4X1KHZ && KISAK_MP.
#include "khz_server_world_checkpoint.h"
namespace {
using namespace khz::server_world_checkpoint;
Bytes<sizeof(g_hudelems)> khzWorldHudImage;
Bytes<sizeof(g_dummyHudCurrent_0)> khzWorldHudDummyImage;
KHZ_ServerCheckpointFrame khzWorldHudFrame;
bool khzWorldHudCaptured;
bool khzWorldHudState(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!frame(f, text, capacity)) return false;
    static_assert(MAX_HUDELEMS_TOTAL == 1024);
    for (const auto& hud : g_hudelems) {
        if (hud.elem.type < HE_TYPE_FREE || hud.elem.type >= HE_TYPE_COUNT)
            return reject(text, capacity, "server_world_hud_native_type_invalid");
        if (hud.elem.type != HE_TYPE_FREE &&
            ((hud.clientNum != ENTITYNUM_NONE && (hud.clientNum < 0 || hud.clientNum >= 64)) ||
             hud.team < TEAM_FREE || hud.team >= TEAM_NUM_TEAMS || (hud.archived != 0 && hud.archived != 1)))
            return reject(text, capacity, "server_world_hud_native_target_or_archival_flag_invalid");
    }
    return true;
}
bool khzWorldHudCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldHudState(f, text, capacity)) return false;
    khzWorldHudImage.capture(g_hudelems); khzWorldHudDummyImage.capture(&g_dummyHudCurrent_0);
    khzWorldHudFrame = f; khzWorldHudCaptured = true; return true;
}
bool khzWorldHudValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldHudCaptured || !khz::server_checkpoint::sameLifetime(khzWorldHudFrame, f))
        return reject(text, capacity, "server_world_hud_image_lifetime_missing_or_changed");
    return khzWorldHudState(f, text, capacity);
}
void khzWorldHudRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext());
    khzWorldHudImage.restore(g_hudelems); khzWorldHudDummyImage.restore(&g_dummyHudCurrent_0);
}
void khzWorldHudDiscard() { khzWorldHudCaptured = false; }
}
void KHZ_ServerWorldHudRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::World, 2,
        sizeof(khzWorldHudImage) + sizeof(khzWorldHudDummyImage),
        khzWorldHudCapture, khzWorldHudValidate, khzWorldHudRestore, khzWorldHudDiscard};
    registerPart(p, g_hudelems, sizeof(g_hudelems));
    if (!KHZ_ServerCheckpointRegisterRange(p.domain, &g_dummyHudCurrent_0, sizeof(g_dummyHudCurrent_0)))
        Com_Error(ERR_FATAL, "Could not register native World HUD comparison storage");
}
