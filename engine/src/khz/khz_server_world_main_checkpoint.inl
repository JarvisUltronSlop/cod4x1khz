// Include at END of game_mp/g_main_mp.cpp under COD4X1KHZ && KISAK_MP.
// Raw fixed BSS images retain the original addresses; never serialize these
// pointer-bearing native records or reconstruct them with gameplay callbacks.
#include "khz_server_world_checkpoint.h"
extern turretInfo_s turretInfo[32];
extern scr_vehicle_s s_vehicles[8];
namespace {
using namespace khz::server_world_checkpoint;
Bytes<sizeof(level)> khzWorldLevelImage;
Bytes<sizeof(level_bgs)> khzWorldBgsImage;
Bytes<sizeof(g_entities)> khzWorldEntitiesImage;
Bytes<sizeof(g_clients)> khzWorldClientsImage;
KHZ_ServerCheckpointFrame khzWorldMainFrame;
int khzWorldUseHoldDelay;
bool khzWorldMainCaptured;
bool khzWorldScriptTable(const animScript_t& script, char* text, unsigned capacity) {
    if (script.numItems < 0 || script.numItems > int(sizeof(script.items) / sizeof(script.items[0])))
        return reject(text, capacity, "server_world_animation_script_item_count_invalid");
    for (const auto* item : script.items)
        if (!element(item, level_bgs.animScriptData.scriptItems))
            return reject(text, capacity, "server_world_animation_script_pointer_outside_original_items");
    for (int i = 0; i < script.numItems; ++i) if (!script.items[i])
        return reject(text, capacity, "server_world_animation_script_active_item_missing");
    return true;
}
bool khzWorldMainState(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!frame(f, text, capacity)) return false;
    if (level.clients != g_clients || level.vehicles != s_vehicles ||
        level.maxclients < 1 || level.maxclients > 16 || level.num_entities < 72 || level.num_entities > MAX_GENTITIES ||
        level.numConnectedClients < 0 || level.numConnectedClients > level.maxclients ||
        level.pendingTriggerListSize < 0 || level.pendingTriggerListSize > 256 ||
        level.currentTriggerListSize < 0 || level.currentTriggerListSize > 256 ||
        level.frametime < 0 || level.previousTime > level.time || level.startTime > level.time)
        return reject(text, capacity, "server_world_level_native_pools_or_counters_invalid");
    // Native log/script FILE handles and their parse buffers have external
    // cursors/side effects. The first pinned fixture explicitly excludes them.
    if (level.logFile) return reject(text, capacity, "server_world_open_game_log_has_no_cursor_restore_provider");
    for (unsigned i = 0; i < sizeof(level.openScriptIOFileHandles) / sizeof(level.openScriptIOFileHandles[0]); ++i)
        if (level.openScriptIOFileHandles[i] || level.openScriptIOFileBuffers[i] ||
            level.currentScriptIOLineMark[i].text || level.currentScriptIOLineMark[i].backup_text)
            return reject(text, capacity, "server_world_script_file_or_external_parse_cursor_unsupported");
    const auto& spawn = level.spawnVar;
    if (spawn.numSpawnVars < 0 || spawn.numSpawnVars > 64 || spawn.numSpawnVarChars < 0 || spawn.numSpawnVarChars > 2048)
        return reject(text, capacity, "server_world_spawn_var_native_counters_invalid");
    for (unsigned i = 0; i < 64; ++i) for (unsigned j = 0; j < 2; ++j) {
        const auto* value = spawn.spawnVars[i][j];
        if (value && !inside(value, spawn.spawnVarChars, sizeof(spawn.spawnVarChars)))
            return reject(text, capacity, "server_world_spawn_var_pointer_outside_original_level_chars");
        if (spawn.spawnVarsValid && i < unsigned(spawn.numSpawnVars) &&
            (!value || !std::memchr(value, 0, sizeof(spawn.spawnVarChars) -
                (reinterpret_cast<std::uintptr_t>(value) - reinterpret_cast<std::uintptr_t>(spawn.spawnVarChars)))))
            return reject(text, capacity, "server_world_active_spawn_var_not_bounded_string");
    }
    const auto triggerList = [&](const trigger_info_t* list, int count) {
        for (int i = 0; i < count; ++i) if (list[i].entnum >= MAX_GENTITIES || list[i].otherEntnum >= MAX_GENTITIES) return false;
        return true;
    };
    if (!triggerList(level.pendingTriggerList, level.pendingTriggerListSize) ||
        !triggerList(level.currentTriggerList, level.currentTriggerListSize))
        return reject(text, capacity, "server_world_pending_trigger_entity_index_invalid");
    if (!entity(level.firstFreeEnt) || !entity(level.lastFreeEnt) || bool(level.firstFreeEnt) != bool(level.lastFreeEnt))
        return reject(text, capacity, "server_world_free_entity_list_endpoint_invalid");
    unsigned char freeSeen[MAX_GENTITIES]{};
    const gentity_s* last = nullptr;
    for (auto* value = level.firstFreeEnt; value; value = value->nextFree) {
        if (!entity(value, false)) return reject(text, capacity, "server_world_free_entity_link_outside_original_arena");
        const auto index = value - g_entities;
        if (freeSeen[index] || value->r.inuse || index < 72 || index >= level.num_entities)
            return reject(text, capacity, "server_world_free_entity_list_cycle_or_active_entry");
        freeSeen[index] = 1; last = value;
    }
    if (last != level.lastFreeEnt) return reject(text, capacity, "server_world_free_entity_tail_disagrees");
    for (unsigned index = 0; index < MAX_GENTITIES; ++index) {
        const auto& ent = g_entities[index];
        if (!element(ent.client, g_clients) || (ent.client && (index >= 64 || ent.client != &g_clients[index])) ||
            !element(ent.pTurretInfo, turretInfo) || !element(ent.scr_vehicle, s_vehicles) ||
            !entity(ent.chain) || !entity(ent.tagChildren) || !entity(ent.nextFree) ||
            (ent.r.inuse && (ent.s.number != index || ent.handler >= ENT_HANDLER_COUNT)))
            return reject(text, capacity, "server_world_entity_pointer_or_native_identity_invalid");
        if (ent.tagInfo) {
            if (!tag(ent.tagInfo))
                return reject(text, capacity, "server_world_entity_tag_has_no_concrete_mt_restore_span");
            if (!entity(ent.tagInfo->parent, false) || !entity(ent.tagInfo->next))
                return reject(text, capacity, "server_world_tag_parent_or_sibling_outside_entity_arena");
        }
    }
    // Check the actual reciprocal tag-child topology, rather than copying a
    // collection of plausible pointers that could lead to a cyclic traversal.
    unsigned char childSeen[MAX_GENTITIES]{};
    for (auto& parent : g_entities) {
        unsigned count = 0;
        for (auto* child = parent.tagChildren; child; child = child->tagInfo->next) {
            if (++count > MAX_GENTITIES || !entity(child, false) || !child->tagInfo ||
                !tag(child->tagInfo) ||
                child->tagInfo->parent != &parent || childSeen[child - g_entities])
                return reject(text, capacity, "server_world_tag_child_graph_cycle_or_nonreciprocal_parent");
            childSeen[child - g_entities] = 1;
        }
    }
    for (unsigned i = 0; i < MAX_GENTITIES; ++i) if (bool(g_entities[i].tagInfo) != bool(childSeen[i]))
        return reject(text, capacity, "server_world_tag_child_graph_coverage_incomplete");
    for (const auto& client : g_clients) if (!entity(client.persistantPowerup))
        return reject(text, capacity, "server_world_client_powerup_pointer_outside_entity_arena");
    const auto& data = level_bgs.animScriptData;
    if (data.numAnimations > 512 || data.numScriptItems < 0 || data.numScriptItems > 2048 ||
        !data.animTree.anims || data.animTree.anims != level_bgs.generic_human.tree.anims ||
        level_bgs.anim_user != 1 || !level_bgs.GetXModel || !level_bgs.CreateDObj ||
        !level_bgs.GetDObj || !level_bgs.SafeDObjFree || !level_bgs.AllocXAnim || level_bgs.AttachWeapon)
        return reject(text, capacity, "server_world_native_bgs_loaded_bindings_invalid");
    for (const auto& row : data.scriptAnims) for (const auto& script : row) if (!khzWorldScriptTable(script, text, capacity)) return false;
    for (const auto& row : data.scriptCannedAnims) for (const auto& script : row) if (!khzWorldScriptTable(script, text, capacity)) return false;
    for (const auto& row : data.scriptStateChange) for (const auto& script : row) if (!khzWorldScriptTable(script, text, capacity)) return false;
    for (const auto& script : data.scriptEvents) if (!khzWorldScriptTable(script, text, capacity)) return false;
    for (const auto& info : level_bgs.clientinfo) if (!clientInfo(info, text, capacity)) return false;
    return true;
}
bool khzWorldMainCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldMainState(f, text, capacity)) return false;
    khzWorldLevelImage.capture(&level); khzWorldBgsImage.capture(&level_bgs);
    khzWorldEntitiesImage.capture(g_entities); khzWorldClientsImage.capture(g_clients);
    khzWorldUseHoldDelay = MY_DEFAULT_USEHOLDSPAWNDELAY;
    khzWorldMainFrame = f; khzWorldMainCaptured = true; return true;
}
bool khzWorldMainValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzWorldMainCaptured || !khz::server_checkpoint::sameLifetime(khzWorldMainFrame, f))
        return reject(text, capacity, "server_world_main_image_lifetime_missing_or_changed");
    if (!khzWorldMainState(f, text, capacity)) return false;
    const auto* saved = reinterpret_cast<const bgs_t*>(khzWorldBgsImage.value);
    // Definition tables, sound/model assets and callback code are pinned for
    // this map lifetime. Mutable animation trees have their own real provider.
    if (std::memcmp(&saved->animScriptData, &level_bgs.animScriptData, sizeof(animScriptData_t)) ||
        std::memcmp(&saved->generic_human, &level_bgs.generic_human, sizeof(bgs_t_human)) ||
        saved->GetXModel != level_bgs.GetXModel || saved->CreateDObj != level_bgs.CreateDObj ||
        saved->AttachWeapon != level_bgs.AttachWeapon || saved->GetDObj != level_bgs.GetDObj ||
        saved->SafeDObjFree != level_bgs.SafeDObjFree || saved->AllocXAnim != level_bgs.AllocXAnim)
        return reject(text, capacity, "server_world_pinned_animation_definition_asset_or_callback_identity_changed");
    return true;
}
void khzWorldMainRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext());
    khzWorldLevelImage.restore(&level); khzWorldBgsImage.restore(&level_bgs);
    khzWorldEntitiesImage.restore(g_entities); khzWorldClientsImage.restore(g_clients);
    MY_DEFAULT_USEHOLDSPAWNDELAY = khzWorldUseHoldDelay;
}
void khzWorldMainDiscard() { khzWorldMainCaptured = false; }
}
void KHZ_ServerWorldMainRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::World, 0,
        sizeof(khzWorldLevelImage) + sizeof(khzWorldBgsImage) + sizeof(khzWorldEntitiesImage) + sizeof(khzWorldClientsImage) + sizeof(khzWorldUseHoldDelay),
        khzWorldMainCapture, khzWorldMainValidate, khzWorldMainRestore, khzWorldMainDiscard};
    registerPart(p, &level, sizeof(level));
    if (!KHZ_ServerCheckpointRegisterRange(p.domain, &level_bgs, sizeof(level_bgs)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, g_entities, sizeof(g_entities)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, g_clients, sizeof(g_clients)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &MY_DEFAULT_USEHOLDSPAWNDELAY, sizeof(MY_DEFAULT_USEHOLDSPAWNDELAY)))
        Com_Error(ERR_FATAL, "Could not register native World level/BGS/entity/client spans");
}
