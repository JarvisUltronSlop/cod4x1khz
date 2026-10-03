// Source-owned diagnostic included at END khz_bots.cpp, after its native
// control storage/getters. The
// native client `pers` field is sess.scriptPersId, NOT the entity script object:
// game/g_client_fields.cpp binds `pers` as F_OBJECT at that session offset.
#include "khz_bot_inspection.h"
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
#include "khz_runtime.h"
#include "khz_admin_server.h"
#include <client_mp/client_mp.h>
#include <game_mp/g_main_mp.h>
#include <game/teams.h>
#include <script/scr_main.h>
#include <script/scr_variable.h>
#include <script/scr_vm.h>
#include <script/scr_evaluate.h>
#include <qcommon/threads.h>
#include <server_mp/server_mp.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Native definition is in scr_vm.cpp; its type is declared by scr_vm.h.
extern scrVmGlob_t scrVmGlob;

namespace khz_bot_inspection {
constexpr unsigned Capacity = 16;
static_assert(KHZ_MaxMatchClients == Capacity, "inspect every admitted native match slot");
char output[12288];
struct Keys {
    std::uint32_t bots, skill, base, aimTime, initReaction, reaction, remember, fov;
};
struct Slot {
    unsigned slot, entityObject, persObject, botsObject, skillObject;
    std::uint64_t generation;
    bool bot;
    int team, sessionState, health, weapon, weaponState, spawnTime, connectTime;
    int base, initReaction, reaction, remember;
    float aimTime, fov;
    std::uint64_t commandTicks, playingTicks, appliedTick;
};

bool objectIs(std::uint32_t id, Vartype_t type) {
    if (!id || id >= VARIABLELIST_PARENT_SIZE) return false;
    const auto& entry = scrVarGlob.variableList[VARIABLELIST_PARENT_BEGIN + id];
    // Native FindVariable requires original external object allocation.
    return (entry.w.status & VAR_STAT_MASK) == VAR_STAT_EXTERNAL &&
        (entry.w.type & VAR_MASK) == type && GetObjectType(id) == static_cast<unsigned>(type);
}
bool childIs(std::uint32_t id, Vartype_t type) {
    if (!id || id >= VARIABLELIST_CHILD_SIZE) return false;
    const auto& entry = scrVarGlob.variableList[VARIABLELIST_CHILD_BEGIN + id];
    return (entry.w.status & VAR_STAT_MASK) != VAR_STAT_FREE &&
        (entry.w.type & VAR_MASK) == type && GetValueType(id) == type;
}
bool arrayChild(std::uint32_t parent, std::uint32_t name, unsigned& value) {
    if (!objectIs(parent, VAR_ARRAY) || !name || name >= 0x10000u) return false;
    const auto child = FindVariable(parent, name);
    if (!childIs(child, VAR_POINTER)) return false;
    value = FindObject(child);
    return objectIs(value, VAR_ARRAY);
}
bool integerChild(unsigned parent, unsigned name, int& value) {
    if (!objectIs(parent, VAR_ARRAY) || !name || name >= 0x10000u) return false;
    const auto child = FindVariable(parent, name);
    if (!childIs(child, VAR_INTEGER)) return false;
    value = GetVariableValueAddress(child)->u.intValue;
    return true;
}
bool floatChild(unsigned parent, unsigned name, float& value) {
    if (!objectIs(parent, VAR_ARRAY) || !name || name >= 0x10000u) return false;
    const auto child = FindVariable(parent, name);
    // These exact BW fields are assigned fractional float literals. An integer
    // or a pointer is a type failure, not silently coerced inspection data.
    if (!childIs(child, VAR_FLOAT)) return false;
    value = GetVariableValueAddress(child)->u.floatValue;
    return std::isfinite(value);
}
bool entityObject(unsigned slot, unsigned& value) {
    const auto parent = g_classMap[CLASS_NUM_ENTITY].entArrayId;
    if (!objectIs(parent, VAR_ARRAY)) return false;
    const auto child = FindArrayVariable(parent, static_cast<int>(slot));
    if (!childIs(child, VAR_POINTER)) return false;
    value = FindObject(child);
    if (!objectIs(value, VAR_ENTITY)) return false;
    const auto& entry = scrVarGlob.variableList[VARIABLELIST_PARENT_BEGIN + value];
    if ((entry.w.classnum >> VAR_NAME_BITS) != CLASS_NUM_ENTITY || entry.u.o.u.entnum != slot)
        return false;
    // Verify the native lookup/getter against the already-checked typed chain;
    // Scr_GetEntityId is intentionally avoided because it can allocate.
    if (FindEntityId(slot, CLASS_NUM_ENTITY) != value) return false;
    const auto ref = Scr_GetEntityIdRef(value);
    return ref.classnum == CLASS_NUM_ENTITY && ref.entnum == slot;
}
bool idle() {
    return scrVarPub.bInited && scrVarPub.programBuffer && scrVarPub.endScriptBuffer &&
        !scrVmPub.function_count && !scrVmPub.inparamcount && !scrVmPub.outparamcount &&
        !scrVmPub.breakpointOutparamcount && !scrVmPub.terminal_error && !scrVmPub.debugCode &&
        !scrVarPub.evaluate && !scrVarPub.error_message && !scrVarPub.error_index &&
        !scrVmGlob.loading && g_script_error_level == -1 &&
        scrVmPub.top == scrVmPub.stack && scrVmPub.maxstack == &scrVmPub.stack[2047] &&
        scrVmPub.function_frame == scrVmPub.function_frame_start &&
        scrVmPub.localVars == reinterpret_cast<std::uint32_t*>(&scrVmGlob.starttime);
}
const char* reject(const char* reason, int slot = -1) {
    std::snprintf(output, sizeof(output),
        "{\"schema\":1,\"complete\":false,\"capacity\":16,\"reason\":\"%s\",\"failed_slot\":%i,\"slots\":[],\"full_bot_acceptance\":false}", reason, slot);
    return output;
}
}

const char* KHZ_BotInspectionJSON() {
    using namespace khz_bot_inspection;
    // Refuse foreign calls before touching any non-atomic gameplay/VM globals,
    // including the owner-local formatter. This literal is immutable storage.
    if (!Sys_IsMainThread()) return "{\"schema\":1,\"complete\":false,\"reason\":\"requires_dedicated_server_owner\",\"slots\":[],\"full_bot_acceptance\":false}";
    if (!Dvar_GetBool("khz_benchmark") || !Dvar_GetBool("khz_test_controls"))
        return reject("requires_benchmark_and_test_controls");
    if (!com_sv_running || !com_sv_running->current.enabled || !svs.initialized || sv.state != SS_GAME ||
        com_inServerFrame || level.initializing || level.clientIsSpawning || !KHZ_ServerTickNumber())
        return reject("requires_completed_loaded_server_tick");
    if (!sv_maxclients || sv_maxclients->current.integer < 1 ||
        sv_maxclients->current.integer > static_cast<int>(Capacity) || !svs.clients ||
        !level.clients || level.gentities != g_entities || level.maxclients != sv_maxclients->current.integer)
        return reject("native_client_entity_storage_invalid");
    if (!idle()) return reject("requires_native_idle_vm");
    const auto session = KHZ_CurrentServerSession(), tick = KHZ_ServerTickNumber();
    if (!session || !sv_serverid || !sv_serverid->current.integer) return reject("missing_native_session");
    Slot values[Capacity]; // Small fixed stack record; no pooled/heap allocation.
    unsigned count = 0, bots = 0, humans = 0;
    Keys keys{}; bool haveKeys = false;
    for (int index = 0; index < sv_maxclients->current.integer; ++index) {
        auto* client = &svs.clients[index];
        if (client->header.state != CS_ACTIVE) continue;
        auto& value = values[count];
        value = {};
        value.slot = static_cast<unsigned>(index);
        const auto& entity = g_entities[index];
        const auto& game = level.clients[index];
        if (client->gentity != &entity || entity.client != &game || entity.s.number != index ||
            entity.s.clientNum != index || game.sess.connected != CON_CONNECTED)
            return reject("native_slot_entity_client_identity_mismatch", index);
        if (!entityObject(value.slot, value.entityObject)) return reject("native_entity_script_object_invalid", index);
        // `client_t.scriptId` is the separate persistent ARRAY, allocated by
        // SV_DirectConnect/BotConnect and passed into native ClientConnect.
        value.persObject = game.sess.scriptPersId;
        if (client->scriptId != value.persObject || !objectIs(value.persObject, VAR_ARRAY))
            return reject("native_client_pers_array_identity_mismatch", index);
        value.bot = KHZ_IsNativeBot(client);
        value.generation = value.bot ? KHZ_BotClientGeneration(client) : KHZ_AdminClientGeneration(client);
        if (!value.generation) return reject("native_slot_admission_generation_invalid", index);
        value.team = static_cast<int>(game.sess.cs.team); value.sessionState = static_cast<int>(game.sess.sessionState);
        if (value.team < TEAM_FREE || value.team >= TEAM_NUM_TEAMS ||
            value.sessionState < SESS_STATE_PLAYING || value.sessionState > SESS_STATE_INTERMISSION)
            return reject("native_team_or_session_state_invalid", index);
        value.health = entity.health; value.weapon = static_cast<int>(game.ps.weapon);
        value.weaponState = static_cast<int>(game.ps.weaponstate);
        value.spawnTime = game.lastSpawnTime; value.connectTime = client->lastConnectTime;
        if (value.bot) {
            if (!haveKeys) {
                // SL_FindString never allocates/refcounts, though its native
                // locked hit path may reorder the string hash cache. This is
                // ordinary diagnostic lookup, not byte-pure VM restoration.
                keys = {SL_FindString("bots"), SL_FindString("skill"), SL_FindString("base"),
                    SL_FindString("aim_time"), SL_FindString("init_react_time"),
                    SL_FindString("reaction_time"), SL_FindString("remember_time"), SL_FindString("fov")};
                haveKeys = true;
            }
            if (!arrayChild(value.persObject, keys.bots, value.botsObject) ||
                !arrayChild(value.botsObject, keys.skill, value.skillObject) ||
                !integerChild(value.skillObject, keys.base, value.base) ||
                !floatChild(value.skillObject, keys.aimTime, value.aimTime) ||
                !integerChild(value.skillObject, keys.initReaction, value.initReaction) ||
                !integerChild(value.skillObject, keys.reaction, value.reaction) ||
                !integerChild(value.skillObject, keys.remember, value.remember) ||
                !floatChild(value.skillObject, keys.fov, value.fov))
                return reject("native_bot_skill_path_missing_or_type_invalid", index);
            // Preserve actual values. These bounds only reject invalid records;
            // matching Recruit/Regular/Hardened/Veteran is a fixture assertion.
            if (value.base < 1 || value.base > 7 || value.aimTime < 0 || value.aimTime > 60 ||
                value.initReaction < 0 || value.initReaction > 60000 || value.reaction < 0 || value.reaction > 60000 ||
                value.remember < 0 || value.remember > 60000 || value.fov < -1 || value.fov > 1)
                return reject("native_bot_skill_value_out_of_supported_bounds", index);
            const auto& control = controls[index];
            if (control.generation != value.generation || slotGenerations[index] != value.generation)
                return reject("native_bot_control_admission_generation_mismatch", index);
            value.commandTicks = control.commands; value.playingTicks = control.playing; value.appliedTick = control.appliedTick;
            ++bots;
        } else ++humans;
        ++count;
    }
    if (!idle() || tick != KHZ_ServerTickNumber() || session != KHZ_CurrentServerSession())
        return reject("native_inspection_owner_frame_changed");
    int used = std::snprintf(output, sizeof(output),
        "{\"schema\":1,\"complete\":true,\"capacity\":16,\"reason\":\"\",\"session\":%llu,\"tick\":%llu,\"server_time\":%i,\"server_id\":%i,\"bot_count\":%u,\"human_count\":%u,\"slots\":[",
        session, tick, svs.time, sv_serverid->current.integer, bots, humans);
    for (unsigned index = 0; index < count; ++index) {
        const auto& value = values[index];
        if (used < 0 || static_cast<unsigned>(used) >= sizeof(output)) return reject("bounded_json_capacity");
        used += std::snprintf(output + used, sizeof(output) - static_cast<unsigned>(used),
            "%s{\"slot\":%u,\"bot\":%s,\"generation\":%llu,\"entity_object\":%u,\"pers_object\":%u,\"team\":%i,\"session_state\":%i,\"health\":%i,\"weapon\":%i,\"weapon_state\":%i,\"last_spawn_time\":%i,\"last_connect_time\":%i",
            index ? "," : "", value.slot, value.bot ? "true" : "false", value.generation,
            value.entityObject, value.persObject, value.team, value.sessionState, value.health,
            value.weapon, value.weaponState, value.spawnTime, value.connectTime);
        if (value.bot) {
            if (used < 0 || static_cast<unsigned>(used) >= sizeof(output)) return reject("bounded_json_capacity");
            used += std::snprintf(output + used, sizeof(output) - static_cast<unsigned>(used),
                ",\"bots_object\":%u,\"skill_object\":%u,\"command_ticks\":%llu,\"playing_ticks\":%llu,\"applied_tick\":%llu,\"skill\":{\"base\":%i,\"aim_time\":%.9g,\"init_react_time\":%i,\"reaction_time\":%i,\"remember_time\":%i,\"fov\":%.9g}",
                value.botsObject, value.skillObject, value.commandTicks, value.playingTicks, value.appliedTick,
                value.base, value.aimTime, value.initReaction, value.reaction, value.remember, value.fov);
        }
        if (used < 0 || static_cast<unsigned>(used) + 2 >= sizeof(output)) return reject("bounded_json_capacity");
        output[used++] = '}'; output[used] = 0;
    }
    if (used < 0 || static_cast<unsigned>(used) + 320 >= sizeof(output)) return reject("bounded_json_capacity");
    std::snprintf(output + used, sizeof(output) - static_cast<unsigned>(used),
        "],\"full_bot_acceptance\":false,\"scope\":\"actual native team/health/weapon and idle VM bot skill values; no objective completion, human reclaim, combat outcome or timing attestation\",\"native_string_hash_cache_may_reorder\":true}");
    return output;
}
#else
const char* KHZ_BotInspectionJSON() {
    return "{\"schema\":1,\"complete\":false,\"reason\":\"requires_dedicated_build\",\"slots\":[],\"full_bot_acceptance\":false}";
}
#endif
