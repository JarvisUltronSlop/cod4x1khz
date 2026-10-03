// Include at END of game_mp/g_scr_main_mp.cpp under COD4X1KHZ && KISAK_MP.
#include "khz_server_world_checkpoint.h"
#include <script/scr_main.h>
namespace {
using namespace khz::server_world_checkpoint;
Bytes<sizeof(g_scr_data)> khzWorldScriptDataImage;
KHZ_ServerCheckpointFrame khzWorldScriptFrame;
std::uint32_t khzWorldScriptProbe, khzWorldBotBootstrap, khzWorldPluginFixture;
bool khzWorldScriptCaptured;
bool khzWorldScriptHandle(std::uint32_t handle) {
    const auto begin = reinterpret_cast<std::uintptr_t>(scrVarPub.programBuffer);
    const auto end = reinterpret_cast<std::uintptr_t>(scrVarPub.endScriptBuffer);
    return !handle || (begin && end > begin && handle < end - begin);
}
bool khzWorldScriptState(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!frame(f, text, capacity)) return false;
    if (f.scriptProgramAddress != reinterpret_cast<std::uintptr_t>(scrVarPub.programBuffer) ||
        g_scr_data.gametype.iNumGameTypes < 0 || g_scr_data.gametype.iNumGameTypes > 32)
        return reject(text, capacity, "server_world_script_program_or_gametype_count_invalid");
    const std::uint32_t handles[] = {std::uint32_t(g_scr_data.levelscript), std::uint32_t(g_scr_data.gametypescript),
        std::uint32_t(g_scr_data.gametype.main), std::uint32_t(g_scr_data.gametype.startupgametype),
        std::uint32_t(g_scr_data.gametype.playerconnect), std::uint32_t(g_scr_data.gametype.playerdisconnect),
        std::uint32_t(g_scr_data.gametype.playerdamage), std::uint32_t(g_scr_data.gametype.playerkilled),
        std::uint32_t(g_scr_data.gametype.votecalled), std::uint32_t(g_scr_data.gametype.playervote),
        std::uint32_t(g_scr_data.gametype.playerlaststand), std::uint32_t(g_scr_data.delete_),
        std::uint32_t(g_scr_data.initstructs), std::uint32_t(g_scr_data.createstruct),
        khzScriptProbe, khzBotBootstrap, khzPluginFixture};
    for (const auto handle : handles) if (!khzWorldScriptHandle(handle))
        return reject(text, capacity, "server_world_native_script_callback_outside_original_program");
    for (const auto& corpse : g_scr_data.playerCorpseInfo) {
        if (corpse.entnum != -1 && (corpse.entnum < 64 || corpse.entnum >= 72))
            return reject(text, capacity, "server_world_corpse_entity_index_invalid");
        if (corpse.tree) {
            if (!KHZ_ServerCheckpointRestoresRange(KHZ_ServerCheckpointDomain::Animation, corpse.tree, sizeof(XAnimTree_s)) ||
                reinterpret_cast<std::uintptr_t>(corpse.tree) % alignof(XAnimTree_s))
                return reject(text, capacity, "server_world_corpse_tree_has_no_concrete_animation_restore_span");
            if (corpse.tree->calcRefCount || corpse.tree->modifyRefCount || corpse.tree->anims != level_bgs.generic_human.tree.anims)
                return reject(text, capacity, "server_world_corpse_tree_busy_or_asset_identity_changed");
        }
        if (!clientInfo(corpse.ci, text, capacity)) return false;
    }
    return true;
}
bool khzWorldScriptCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldScriptState(f, text, capacity)) return false;
    khzWorldScriptDataImage.capture(&g_scr_data);
    khzWorldScriptProbe = khzScriptProbe; khzWorldBotBootstrap = khzBotBootstrap; khzWorldPluginFixture = khzPluginFixture;
    khzWorldScriptFrame = f; khzWorldScriptCaptured = true; return true;
}
bool khzWorldScriptValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldScriptCaptured || !khz::server_checkpoint::sameLifetime(khzWorldScriptFrame, f))
        return reject(text, capacity, "server_world_script_image_lifetime_missing_or_changed");
    return khzWorldScriptState(f, text, capacity);
}
void khzWorldScriptRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext());
    khzWorldScriptDataImage.restore(&g_scr_data);
    khzScriptProbe = khzWorldScriptProbe; khzBotBootstrap = khzWorldBotBootstrap; khzPluginFixture = khzWorldPluginFixture;
}
void khzWorldScriptDiscard() { khzWorldScriptCaptured = false; }
}
void KHZ_ServerWorldScriptRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::World, 1,
        sizeof(khzWorldScriptDataImage) + sizeof(khzWorldScriptProbe) + sizeof(khzWorldBotBootstrap) + sizeof(khzWorldPluginFixture),
        khzWorldScriptCapture, khzWorldScriptValidate, khzWorldScriptRestore, khzWorldScriptDiscard};
    registerPart(p, &g_scr_data, sizeof(g_scr_data));
    if (!KHZ_ServerCheckpointRegisterRange(p.domain, &khzScriptProbe, sizeof(khzScriptProbe)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &khzBotBootstrap, sizeof(khzBotBootstrap)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &khzPluginFixture, sizeof(khzPluginFixture)))
        Com_Error(ERR_FATAL, "Could not register native World scenario/bootstrap callback handles");
}
