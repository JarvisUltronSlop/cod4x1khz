// Native CoD4x-compatible bot controls, adapted to the fixed millisecond owner.
// CoD4x's public method names and action vocabulary are retained. Its original
// implementation and notices remain in upstream/cod4x_server/src/sv_bots.*.
#include <universal/q_shared.h>
#include "khz_bots.h"
#include <core.hpp>
#if defined(COD4X1KHZ) && defined(KISAK_MP)
#include <server_mp/server_mp.h>
#include <game_mp/g_public_mp.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <game/game_public.h>
#include <script/scr_vm.h>
#include <script/scr_const.h>
#include <script/scr_variable.h>
#include <script/scr_stringlist.h>
#include <qcommon/qcommon.h>
#include <qcommon/msg_mp.h>
#include <qcommon/threads.h>
#include <universal/com_files.h>
#include <universal/com_math.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
struct BotControl {
    std::uint64_t generation = 0, appliedTick = 0;
    unsigned buttons = 0;
    unsigned pressedButtons = 0;
    float moveTo[2]{};
    bool moving = false;
    bool aiming = false;
    float aimStart[3]{}, aimDelta[3]{};
    std::uint64_t aimBegin = 0, aimEnd = 0;
    std::uint64_t respawnAt = 0, nextUse = 0;
    bool wasAlive = false;
    unsigned weapon = 0;
    bool weaponSelected = false;
    std::uint64_t commands = 0, playing = 0;
};
std::array<BotControl, KHZ_MaxMatchClients> controls{};
std::array<std::uint64_t, KHZ_MaxMatchClients> slotGenerations{};
khz::Histogram controlWork;
std::uint64_t nowTick = 0, nextSettings = 0;
std::uint64_t commandTicks = 0, playingTicks = 0, aimTicks = 0;
std::uint64_t moveGoals = 0, aimGoals = 0, respawnPulses = 0;
bool scriptsStarted = false;
int configuredTarget = -1, configuredTier = -1;
const dvar_t *enabled = nullptr, *desiredCount = nullptr, *difficulty = nullptr;

constexpr int FileCount = 16;
constexpr unsigned FileIndexBits = 5;
constexpr unsigned FileIndexMask = (1u << FileIndexBits) - 1;
struct ScriptFile {
    int engineHandle = 0;
    FILE* readHandle = nullptr;
    FILE* writeHandle = nullptr;
    char path[MAX_OSPATH]{};
    unsigned token = 0;
    std::array<unsigned char, 4096> buffer{};
    unsigned cursor = 0, available = 0;
};
std::array<ScriptFile, FileCount> files{};
unsigned nextFileToken = 0;

void owner() { iassert(Sys_IsMainThread()); }
void resetControl(unsigned index) {
    auto generation = ++slotGenerations[index];
    if (!generation) generation = ++slotGenerations[index];
    controls[index] = BotControl{};
    controls[index].generation = generation;
}
int slot(const client_t* client) {
    if (!client || !svs.clients) return -1;
    const auto index = client - svs.clients;
    return index >= 0 && index < KHZ_MaxMatchClients ? static_cast<int>(index) : -1;
}
gentity_s* botEntity(scr_entref_t ref) {
    owner();
    if (ref.classnum || ref.entnum >= KHZ_MaxMatchClients ||
        !svs.clients || !KHZ_IsNativeBot(&svs.clients[ref.entnum]) ||
        svs.clients[ref.entnum].header.state < CS_CONNECTED || !g_entities[ref.entnum].client) {
        Scr_ObjectError("not a native bot player");
        return nullptr;
    }
    return &g_entities[ref.entnum];
}
void finiteVector(const float* vector, unsigned param) {
    for (int i = 0; i != 3; ++i)
        if (!std::isfinite(vector[i])) Scr_ParamError(param, "vector must be finite");
}
std::uint64_t durationTicks(float seconds, unsigned param) {
    if (!std::isfinite(seconds) || seconds < 0.001f || seconds > 3600.0f) {
        Scr_ParamError(param, "duration must be between 0.001 and 3600 seconds");
        return 1;
    }
    return static_cast<std::uint64_t>((std::max)(1.0, std::ceil(double(seconds) * 1000.0 - 0.00001)));
}
void aimAngles(gentity_s* entity, const float* angles, std::uint64_t duration) {
    auto& control = controls[entity->s.clientNum];
    for (int i = 0; i != 3; ++i) {
        control.aimStart[i] = entity->client->ps.viewangles[i];
        control.aimDelta[i] = AngleDelta(angles[i], control.aimStart[i]);
    }
    control.aimBegin = nowTick;
    control.aimEnd = nowTick + duration;
    control.aiming = true;
    ++aimGoals;
}
void aimOrigin(gentity_s* entity, const float* origin, std::uint64_t duration) {
    float eye[3], direction[3], angles[3];
    G_GetPlayerViewOrigin(&entity->client->ps, eye);
    Vec3Sub(origin, eye, direction);
    if (Vec3LengthSq(direction) <= 0.000001f) return;
    vectoangles(direction, angles);
    angles[2] = 0;
    aimAngles(entity, angles, duration);
}
void countClients(int& humans, int& bots) {
    humans = bots = 0;
    if (!svs.clients || !sv_maxclients) return;
    for (int i = 0; i < (std::min)(KHZ_MaxMatchClients, sv_maxclients->current.integer); ++i) {
        const auto& client = svs.clients[i];
        if (client.header.state >= CS_CONNECTED) {
            if (KHZ_IsNativeBot(&client)) ++bots;
            else ++humans;
        }
    }
}
void syncSettings() {
    if (!enabled || !desiredCount || !difficulty) return;
    int humans, bots;
    countClients(humans, bots);
    const int capacity = sv_maxclients ? (std::min)(KHZ_MaxMatchClients, sv_maxclients->current.integer) : KHZ_MaxMatchClients;
    const int target = enabled->current.enabled ?
        (std::min)(desiredCount->current.integer, (std::max)(0, capacity - (std::max)(humans, 1))) : 0;
    // These are Bot Warfare's existing manager controls. Its own add_bot owns
    // script setup; admission never starts a second, competing bot manager.
    if (target != configuredTarget) {
        Dvar_SetIntByName("bots_manage_fill", target);
        configuredTarget = target;
    }
    constexpr int tiers[4] = {1, 3, 5, 7};
    const int tier = tiers[(std::clamp)(difficulty->current.integer, 0, 3)];
    if (tier != configuredTier) {
        Dvar_SetIntByName("bots_skill", tier);
        configuredTier = tier;
    }
}
void closeFile(ScriptFile& file) {
    if (file.engineHandle) FS_FCloseFile(file.engineHandle);
    if (file.readHandle) fclose(file.readHandle);
    if (file.writeHandle) fclose(file.writeHandle);
    file.engineHandle = 0; file.readHandle = file.writeHandle = nullptr;
    file.path[0] = 0;
    file.token = 0; file.cursor = file.available = 0;
}
bool pathForScriptData(const char* supplied, char* result, unsigned size) {
    if (!supplied || !*supplied || supplied[0] == '/' || supplied[0] == '\\') return false;
    if (std::strlen(supplied) + 12 >= size) return false;
    char normalized[MAX_OSPATH];
    I_strncpyz(normalized, supplied, sizeof(normalized));
    for (char* p = normalized; *p; ++p) {
        if (*p == '\\') *p = '/';
        if (static_cast<unsigned char>(*p) < 32 || *p == ':' || *p == '"') return false;
    }
    const char* component = normalized;
    for (const char* p = normalized; ; ++p) {
        if (!*p || *p == '/') {
            const auto length = p - component;
            if (!length || (length == 1 && component[0] == '.') ||
                (length == 2 && component[0] == '.' && component[1] == '.')) return false;
            component = p + 1;
            if (!*p) break;
        }
    }
    if (!I_strnicmp(normalized, "scriptdata/", 11)) I_strncpyz(result, normalized, size);
    else Com_sprintf(result, size, "scriptdata/%s", normalized);
    return true;
}
ScriptFile* getFile(int handle) {
    if (handle <= 0) return nullptr;
    const unsigned index = static_cast<unsigned>(handle) & FileIndexMask;
    if (!index || index > files.size()) return nullptr;
    auto& file = files[index - 1];
    return file.token == static_cast<unsigned>(handle) && (file.engineHandle || file.readHandle || file.writeHandle) ? &file : nullptr;
}
bool fileByte(ScriptFile& file, unsigned char& value) {
    if (file.cursor == file.available) {
        const auto count = file.readHandle ? static_cast<unsigned>(fread(file.buffer.data(), 1, file.buffer.size(), file.readHandle)) :
            FS_Read(file.buffer.data(), static_cast<unsigned>(file.buffer.size()), file.engineHandle);
        if (!count || count > file.buffer.size()) return false;
        file.available = count; file.cursor = 0;
    }
    value = file.buffer[file.cursor++];
    return true;
}
void requireParams(unsigned count, const char* usage) {
    owner(); if (Scr_GetNumParam() != count) Scr_Error(usage);
}
}

void KHZ_BotInit() {
    owner();
    enabled = Dvar_RegisterBool("khz_bots", false, DVAR_ARCHIVE | DVAR_LATCH, "Enable the project Bot Warfare scripts");
    desiredCount = Dvar_RegisterInt("khz_bot_count", 0, 0, 15, DVAR_ARCHIVE, "Desired native bots within the 16 total match slots");
    difficulty = Dvar_RegisterInt("khz_bot_difficulty", 1, 0, 3, DVAR_ARCHIVE, "0 Recruit, 1 Regular, 2 Hardened, 3 Veteran");
}
bool KHZ_BotsEnabled() { return enabled && enabled->current.enabled; }
bool KHZ_IsNativeBot(const client_t* client) {
    return client && client->header.netchan.remoteAddress.type == NA_BOT;
}
void KHZ_BotResetSlot(client_t* client) {
    owner(); const int index = slot(client); if (index >= 0) resetControl(static_cast<unsigned>(index));
}
void KHZ_BotResetWorkStats() { owner(); controlWork = {}; }
void KHZ_BotResetAll() {
    owner(); for (unsigned i = 0; i < controls.size(); ++i) resetControl(i);
    nowTick = nextSettings = 0; KHZ_BotResetWorkStats();
    commandTicks = playingTicks = aimTicks = moveGoals = aimGoals = respawnPulses = 0;
    scriptsStarted = false;
    configuredTarget = configuredTier = -1;
}
void KHZ_BotShutdownFiles() { owner(); for (auto& file : files) closeFile(file); }
void KHZ_BotPrepareMap() {
    owner(); KHZ_BotShutdownFiles(); KHZ_BotResetAll(); syncSettings();
    // Readiness is independent of the first human. Native admission reserves a
    // human slot, and the manager can fill after its normal startup delay.
    Dvar_SetIntByName("bots_main_waitForHostTime", 0);
    Dvar_SetIntByName("bots_manage_fill_mode", 1);
    Dvar_SetIntByName("bots_manage_fill_spec", 0);
    Dvar_SetIntByName("bots_manage_fill_kick", 1);
    Dvar_SetIntByName("bots_manage_add", KHZ_BotsEnabled() ? desiredCount->current.integer : 0);
}
void KHZ_BotScriptStarted() { owner(); scriptsStarted = true; }
void KHZ_BotTick(std::uint64_t tick) {
    owner(); nowTick = tick;
    if (tick >= nextSettings) { syncSettings(); nextSettings = tick + 100; }
}
std::uint64_t KHZ_BotClientGeneration(const client_t* client) {
    owner();
    if (!client || !svs.clients || !sv_maxclients) return 0;
    const auto address = reinterpret_cast<std::uintptr_t>(client);
    const auto base = reinterpret_cast<std::uintptr_t>(svs.clients);
    if (address < base || (address - base) % sizeof(client_t)) return 0;
    const auto index = (address - base) / sizeof(client_t);
    if (index >= KHZ_MaxMatchClients || index >= unsigned(sv_maxclients->current.integer) ||
        !KHZ_IsNativeBot(client) || client->header.state != CS_ACTIVE || !client->gentity) return 0;
    const auto generation = controls[index].generation;
    return generation && generation == slotGenerations[index] ? generation : 0;
}
bool KHZ_BotBuildCommand(client_t* client, usercmd_s* command) {
    owner(); const int index = slot(client);
    if (index < 0 || !command || !KHZ_IsNativeBot(client) || client->header.state != CS_ACTIVE || !client->gentity) return false;
    const auto workStart = khz::counter();
    auto& control = controls[index];
    if (!control.generation || control.generation != slotGenerations[index]) resetControl(static_cast<unsigned>(index));
    auto* entity = client->gentity;
    auto* ps = &entity->client->ps;
    const bool alive = entity->health > 0 && entity->client->sess.sessionState == SESS_STATE_PLAYING;
    if (control.wasAlive && !alive) {
        control.buttons = control.pressedButtons = 0; control.moving = control.aiming = false;
        control.weaponSelected = false;
        control.respawnAt = nowTick + 3000; control.nextUse = control.respawnAt;
    }
    control.wasAlive = alive;
    *command = client->lastUsercmd;
    command->serverTime = svs.time;
    command->buttons = control.buttons | control.pressedButtons;
    control.pressedButtons = 0;
    if (G_GetClientArchiveTime(index)) command->buttons = 0;
    command->forwardmove = command->rightmove = 0;
    command->weapon = static_cast<unsigned char>(control.weaponSelected ? control.weapon : ps->weapon);
    if (control.weaponSelected && control.weapon == ps->weapon) control.weaponSelected = false;
    // Holding the previous command angles preserves setplayerangles' delta
    // offset. Only the owner updates the float angle goal, never a render frame.
    if (alive && control.aiming) {
        const double fraction = nowTick >= control.aimEnd ? 1.0 :
            double(nowTick - control.aimBegin) / double(control.aimEnd - control.aimBegin);
        float angles[3];
        for (int i = 0; i != 3; ++i) angles[i] = control.aimStart[i] + float(fraction * control.aimDelta[i]);
        SetClientViewAngle(entity, angles);
        ++aimTicks;
        if (nowTick >= control.aimEnd) { control.aiming = false; Scr_Notify(entity, scr_const.rotatedone, 0); }
    }
    if (alive && control.moving && !G_GetClientArchiveTime(index)) {
        const float dx = control.moveTo[0] - ps->origin[0], dy = control.moveTo[1] - ps->origin[1];
        if (dx * dx + dy * dy <= 49.0f) {
            control.moving = false; Scr_Notify(entity, scr_const.movedone, 0);
        } else {
            const double yaw = ps->viewangles[1] * (3.14159265358979323846 / 180.0);
            const double forward = dx * std::cos(yaw) + dy * std::sin(yaw);
            const double right = dx * std::sin(yaw) - dy * std::cos(yaw);
            const double maximum = (std::max)(std::abs(forward), std::abs(right));
            if (maximum > 0.000001) {
                command->forwardmove = static_cast<signed char>((std::clamp)(int(std::lround(forward * 127.0 / maximum)), -127, 127));
                command->rightmove = static_cast<signed char>((std::clamp)(int(std::lround(right * 127.0 / maximum)), -127, 127));
            }
        }
    }
    if (!alive && control.respawnAt && nowTick >= control.nextUse) {
        command->buttons |= BUTTON_USE | BUTTON_USE_RELOAD;
        control.nextUse = nowTick + 250; ++respawnPulses;
    }
    if (alive) { control.respawnAt = control.nextUse = 0; ++playingTicks; ++control.playing; }
    client->lastPacketTime = svs.time;
    ++commandTicks; ++control.commands;
    control.appliedTick = nowTick;
    controlWork.add((khz::counter() - workStart) * 1000000 / khz::frequency());
    return true;
}
const char* KHZ_BotStatsJSON() {
    // Sixteen entries with all 64-bit counters at their maximum still fit.
    static char json[6144]; int humans, bots; countClients(humans, bots);
    unsigned open = 0; for (const auto& file : files) if (file.engineHandle || file.readHandle || file.writeHandle) ++open;
    int length = std::snprintf(json, sizeof(json), "{\"active\":%d,\"humans\":%d,\"requested\":%d,\"difficulty\":%d,\"script_bootstrap\":%s,\"command_ticks\":%llu,\"playing_ticks\":%llu,\"aim_ticks\":%llu,\"move_goals\":%llu,\"aim_goals\":%llu,\"respawn_pulses\":%llu,\"files_open\":%u,\"control_work\":{\"count\":%llu,\"total_us\":%llu,\"p99_us\":%u,\"max_us\":%llu},\"slots\":[",
        bots, humans, desiredCount ? desiredCount->current.integer : 0, difficulty ? difficulty->current.integer : 1,
        scriptsStarted ? "true" : "false", commandTicks, playingTicks, aimTicks, moveGoals, aimGoals, respawnPulses, open,
        controlWork.count, controlWork.total, controlWork.percentile(99), controlWork.maximum);
    bool first = true;
    if (svs.clients && sv_maxclients) for (int i = 0; i < (std::min)(KHZ_MaxMatchClients, sv_maxclients->current.integer); ++i) {
        if (svs.clients[i].header.state < CS_CONNECTED || !KHZ_IsNativeBot(&svs.clients[i])) continue;
        const auto& control = controls[i];
        length += std::snprintf(json + length, sizeof(json) - length,
            "%s{\"slot\":%d,\"generation\":%llu,\"applied_tick\":%llu,\"command_ticks\":%llu,\"playing_ticks\":%llu}", first ? "" : ",", i,
            control.generation, control.appliedTick, control.commands, control.playing);
        first = false;
    }
    std::snprintf(json + length, sizeof(json) - length, "]}");
    return json;
}

void KHZ_ScrBotMoveTo(scr_entref_t ref) {
    requireParams(1, "Usage: bot botmoveto(<position>);"); auto* entity = botEntity(ref);
    float position[3]; Scr_GetVector(0, position); finiteVector(position, 0);
    auto& control = controls[entity->s.clientNum]; control.moveTo[0] = position[0]; control.moveTo[1] = position[1]; control.moving = true; ++moveGoals;
}
void KHZ_ScrBotLookAt(scr_entref_t ref) {
    auto* entity = botEntity(ref); const auto count = Scr_GetNumParam();
    if (count != 1 && count != 2) Scr_Error("Usage: bot botlookat(<origin>, [duration]);");
    float origin[3]; Scr_GetVector(0, origin); finiteVector(origin, 0);
    aimOrigin(entity, origin, count == 2 ? durationTicks(Scr_GetFloat(1), 1) : 1);
}
void KHZ_ScrBotStop(scr_entref_t ref) {
    requireParams(0, "Usage: bot botstop();"); auto* entity = botEntity(ref);
    auto& control = controls[entity->s.clientNum]; control.buttons = control.pressedButtons = 0; control.moving = control.aiming = false; control.weaponSelected = false;
}
void KHZ_ScrBotAction(scr_entref_t ref) {
    requireParams(1, "Usage: bot botaction(<+action or -action>);"); auto* entity = botEntity(ref);
    const char* action = Scr_GetString(0);
    if (action[0] != '+' && action[0] != '-') Scr_ParamError(0, "action requires '+' or '-' prefix");
    struct Action { const char* name; unsigned mask; };
    // Use this engine's actual command interpretation. BUTTON_ADS is held ADS;
    // the public binary client's additional 0x80000 bit has another name here.
    static constexpr Action actions[] = {
        {"gostand", BUTTON_JUMP}, {"gocrouch", BUTTON_CROUCH}, {"goprone", BUTTON_PRONE},
        {"fire", BUTTON_ATTACK}, {"melee", BUTTON_MELEE}, {"frag", BUTTON_FRAG}, {"smoke", BUTTON_SMOKE},
        {"reload", BUTTON_RELOAD}, {"sprint", BUTTON_SPRINT}, {"leanleft", BUTTON_LEAN_LEFT},
        {"leanright", BUTTON_LEAN_RIGHT}, {"ads", BUTTON_ADS}, {"holdbreath", BUTTON_BREATH}, {"activate", BUTTON_USE}
    };
    for (const auto& mapping : actions) if (!I_stricmp(action + 1, mapping.name)) {
        auto& control = controls[entity->s.clientNum];
        if (action[0] == '+') {
            control.pressedButtons |= mapping.mask & ~control.buttons;
            control.buttons |= mapping.mask;
        } else control.buttons &= ~mapping.mask;
        return;
    }
    Scr_ParamError(0, "unknown bot action");
}
void KHZ_ScrBotLookAtPlayer(scr_entref_t ref) {
    auto* entity = botEntity(ref); const auto count = Scr_GetNumParam();
    if (count != 1 && count != 2) Scr_Error("Usage: bot botlookatplayer(<player>, [tag]);");
    auto* target = Scr_GetEntity(0);
    if (!target || !target->client) Scr_ParamError(0, "target must be a player");
    const auto tag = count == 2 ? Scr_GetConstString(1) : scr_const.pelvis;
    float origin[3];
    if (!G_DObjGetWorldTagPos(target, tag, origin)) Scr_ParamError(1, "target tag does not exist");
    // This public method requests one simulation tick. Bot Warfare's adapted
    // aim helper supplies its own 50 ms prediction horizon instead.
    for (int i = 0; i != 3; ++i) origin[i] += target->client->ps.velocity[i] * 0.001f;
    aimOrigin(entity, origin, 1);
}
void KHZ_ScrBotWeapon(scr_entref_t ref) {
    requireParams(1, "Usage: bot botweapon(<weapon name>);"); auto* entity = botEntity(ref);
    const char* name = Scr_GetString(0); const unsigned weapon = *name ? G_GetWeaponIndexForName(name) : 1;
    if (!weapon || weapon > 255) Scr_ParamError(0, "invalid bot weapon");
    auto& control = controls[entity->s.clientNum]; control.weapon = weapon; control.weaponSelected = true;
}
void KHZ_ScrBotAim(scr_entref_t ref) {
    requireParams(2, "Usage: bot khz_botaim(<angles>, <seconds>);"); auto* entity = botEntity(ref);
    float angles[3]; Scr_GetVector(0, angles); finiteVector(angles, 0);
    aimAngles(entity, angles, durationTicks(Scr_GetFloat(1), 1));
}
void KHZ_ScrBotAimCancel(scr_entref_t ref) { requireParams(0, "Usage: bot khz_botaimcancel();"); auto* entity = botEntity(ref); controls[entity->s.clientNum].aiming = false; }
void KHZ_RemoveBotClients(int count) {
    owner(); if (!svs.clients || !sv_maxclients) return;
    for (int i = (std::min)(KHZ_MaxMatchClients, sv_maxclients->current.integer) - 1; i >= 0 && count > 0; --i) {
        auto* client = &svs.clients[i];
        if (client->header.state >= CS_CONNECTED && KHZ_IsNativeBot(client)) { SV_DropClient(client, "EXE_DISCONNECTED", false); --count; }
    }
}
void KHZ_ScrRemoveAllBots() { requireParams(0, "Usage: removealltestclients();"); KHZ_RemoveBotClients(KHZ_MaxMatchClients); }
void KHZ_ScrRemoveBot() { requireParams(0, "Usage: removetestclient();"); KHZ_RemoveBotClients(1); }

void KHZ_ScrFileOpen() {
    requireParams(2, "Usage: fs_fopen(<filename>, <read|write|append>);");
    char path[MAX_OSPATH];
    if (!pathForScriptData(Scr_GetString(0), path, sizeof(path))) Scr_ParamError(0, "file must be a relative scriptdata path");
    const char* mode = Scr_GetString(1);
    const bool read = !I_stricmp(mode, "read");
    const bool append = !I_stricmp(mode, "append");
    if (!read && !append && I_stricmp(mode, "write")) Scr_ParamError(1, "mode must be read, write, or append");
    for (unsigned i = 0; i < files.size(); ++i) {
        auto& file = files[i]; if (file.engineHandle || file.readHandle || file.writeHandle) continue;
        if (read) {
            // Prefer files written by the editor beneath runtime/scriptdata;
            // packaged stock waypoint files still use the game search paths.
            char absolute[MAX_OSPATH]; FS_BuildOSPath(fs_homepath->current.string, "", path, absolute);
            fopen_s(&file.readHandle, absolute, "rb");
            if (!file.readHandle) {
                const auto length = FS_FOpenFileRead(path, &file.engineHandle);
                if (length == ~0u || !file.engineHandle) { Scr_AddInt(0); return; }
            }
        } else {
            // Explicit empty game directory keeps editor writes in
            // <project runtime>/scriptdata, independent of fs_game.
            char absolute[MAX_OSPATH]; FS_BuildOSPath(fs_homepath->current.string, "", path, absolute);
            if (FS_CreatePath(absolute)) { Scr_AddInt(0); return; }
            if (fopen_s(&file.writeHandle, absolute, append ? "ab" : "wb") || !file.writeHandle) { Scr_AddInt(0); return; }
        }
        nextFileToken = (nextFileToken + 1) & 0x03ffffffu;
        if (!nextFileToken) ++nextFileToken;
        file.token = (nextFileToken << FileIndexBits) | (i + 1);
        I_strncpyz(file.path, path, sizeof(file.path));
        file.cursor = file.available = 0; Scr_AddInt(static_cast<int>(file.token)); return;
    }
    Scr_AddInt(0);
}
void KHZ_ScrFileClose() {
    requireParams(1, "Usage: fs_fclose(<handle>);");
    auto* file = getFile(Scr_GetInt(0)); if (!file) Scr_ParamError(0, "invalid or closed file handle"); closeFile(*file);
}
void KHZ_ScrFileCloseAll() { requireParams(0, "Usage: fs_fcloseall();"); KHZ_BotShutdownFiles(); }
void KHZ_ScrFileReadLine() {
    requireParams(1, "Usage: fs_readline(<handle>);");
    auto* file = getFile(Scr_GetInt(0));
    if (!file || (!file->engineHandle && !file->readHandle)) { Scr_AddUndefined(); return; }
    char line[8192]; unsigned length = 0; unsigned char value = 0; bool got = false;
    while (fileByte(*file, value)) {
        got = true; if (value == '\n') break;
        if (!value) Scr_Error("fs_readline requires a text file");
        if (length == sizeof(line) - 1) Scr_Error("fs_readline line exceeds 8191 bytes");
        line[length++] = static_cast<char>(value);
    }
    if (!got) { Scr_AddUndefined(); return; }
    if (length && line[length - 1] == '\r') --length;
    line[length] = 0; Scr_AddString(line);
}
void KHZ_ScrFileWriteLine() {
    requireParams(2, "Usage: fs_writeline(<handle>, <text>);");
    auto* file = getFile(Scr_GetInt(0)); if (!file || !file->writeHandle) { Scr_AddBool(false); return; }
    const char* text = Scr_GetString(1); const auto length = std::strlen(text);
    if (length > 8191) Scr_ParamError(1, "line exceeds 8191 bytes");
    const bool wrote = fwrite(text, 1, length, file->writeHandle) == length && fwrite("\n", 1, 1, file->writeHandle) == 1;
    Scr_AddBool(wrote);
}
void KHZ_ScrFileExists() {
    requireParams(1, "Usage: fs_testfile(<filename>);"); char path[MAX_OSPATH];
    if (!pathForScriptData(Scr_GetString(0), path, sizeof(path))) Scr_ParamError(0, "file must be a relative scriptdata path");
    char absolute[MAX_OSPATH]; FS_BuildOSPath(fs_homepath->current.string, "", path, absolute);
    FILE* file = nullptr; fopen_s(&file, absolute, "rb");
    if (file) { fclose(file); Scr_AddBool(true); return; }
    int handle = 0; const auto length = FS_FOpenFileRead(path, &handle);
    if (handle) FS_FCloseFile(handle); Scr_AddBool(length != ~0u);
}
void KHZ_ScrFileRemove() {
    requireParams(1, "Usage: fs_remove(<filename>);"); char path[MAX_OSPATH], absolute[MAX_OSPATH];
    if (!pathForScriptData(Scr_GetString(0), path, sizeof(path))) Scr_ParamError(0, "file must be a relative scriptdata path");
    for (const auto& file : files) if (file.token && !I_stricmp(file.path, path)) Scr_Error("fs_remove cannot delete an open file");
    FS_BuildOSPath(fs_homepath->current.string, "", path, absolute); Scr_AddBool(std::remove(absolute) == 0);
}
#endif

#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
// Completed-owner, explicitly gated native VM inspection; never a tick helper.
#include "khz_bot_inspection.inl"
#endif
