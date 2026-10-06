#include <universal/q_shared.h>
#include "khz_benchmark_scene.h"
#include "khz_controls.h"
#include "khz_bots.h"
#include "khz_admin_server.h"
#include "khz_runtime.h"
#include <qcommon/cmd.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <server_mp/server_mp.h>
#include <game_mp/g_public_mp.h>
#include <server/sv_game.h>
#include <client_mp/client_mp.h>
#ifndef KISAK_DEDICATED
#include <cgame_mp/cg_local_mp.h>
#include <ragdoll/ragdoll.h>
#include <xanim/dobj.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
// Diagnostic-only controls. The normal client and legacy timing workload do
// not enter this path. A parked owner transfers this small configuration.
struct SceneInput { int mode = 0, checksum = 0, goal = 1; float angles[3]{}, points[2][3]{}, look[3]{}; } input;
struct ProtectedHuman {
    int slot = -1;
    std::uint32_t checksum = 0;
    int serverId = 0, challenge = 0, connected = 0, port = 0;
    std::uint64_t session = 0, generation = 0;
    bool previousGod = false;
} protectedHuman;
bool protectedIdentity(const gentity_s* entity) {
    if (protectedHuman.slot < 0 || !svs.clients || !cm.isInUse ||
        protectedHuman.checksum != cm.checksum || protectedHuman.serverId != sv_serverId_value ||
        protectedHuman.session != KHZ_CurrentServerSession() || entity != &g_entities[protectedHuman.slot]) return false;
    const auto* client = &svs.clients[protectedHuman.slot];
    return client->header.state == CS_ACTIVE && !KHZ_IsNativeBot(client) && entity->client &&
        protectedHuman.challenge == client->challenge && protectedHuman.connected == client->lastConnectTime &&
        protectedHuman.generation == KHZ_AdminClientGeneration(client) &&
        protectedHuman.port == static_cast<unsigned short>(BigShort(client->header.netchan.remoteAddress.port));
}
void releaseProtection() {
    if (protectedHuman.slot >= 0 && protectedIdentity(&g_entities[protectedHuman.slot]) && !protectedHuman.previousGod)
        g_entities[protectedHuman.slot].flags &= ~FL_GODMODE;
    protectedHuman = {};
}
bool number(int arg, float& value) {
    char* end = nullptr; const char* text = Cmd_Argv(arg);
    value = std::strtof(text, &end);
    return *text && end && !*end && std::isfinite(value) && std::abs(value) <= 32768;
}
float ray(const float* eye, float yaw, float pitch) {
    constexpr float radians = 0.017453292519943295f;
    const float y = yaw * radians, p = pitch * radians;
    float end[3]{eye[0] + 2048 * std::cos(p) * std::cos(y), eye[1] + 2048 * std::cos(p) * std::sin(y), eye[2] - 2048 * std::sin(p)};
    const float zero[3]{}; trace_t trace{};
    CM_BoxTrace(&trace, eye, end, zero, zero, 0, CONTENTS_SOLID | CONTENTS_GLASS);
    return trace.startsolid || trace.allsolid ? 0 : 2048 * trace.fraction;
}
bool clear(const float* origin) {
    const float mins[3]{-15, -15, 0}, maxs[3]{15, 15, 72}; trace_t trace{};
    CM_BoxTrace(&trace, origin, origin, mins, maxs, 0, MASK_PLAYERSOLID);
    return !trace.startsolid && !trace.allsolid;
}
gentity_s* human(int port) {
    if (!svs.clients || port < 1024 || port > 65535) return nullptr;
    for (int i = 0; i < sv_maxclients->current.integer; ++i) {
        const auto* client = &svs.clients[i];
        if (client->header.state == CS_ACTIVE && !KHZ_IsNativeBot(client) &&
            static_cast<unsigned short>(BigShort(client->header.netchan.remoteAddress.port)) == port &&
            g_entities[i].client && g_entities[i].client->ps.pm_type == PM_NORMAL && g_entities[i].health > 0)
            return &g_entities[i];
    }
    return nullptr;
}
void reply(const char* text) { Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_RENDER_SCENE %s\n", text); }
void command() {
    iassert(Sys_IsMainThread());
    if (!Dvar_GetBool("khz_test_controls") || !Dvar_GetBool("khz_benchmark") || !cm.isInUse) {
        reply("{\"error\":\"requires loaded isolated benchmark and test controls\"}"); return;
    }
    const char* action = Cmd_Argv(1); char result[8192]{};
    if (com_sv_running->current.enabled) {
        if (!std::strcmp(action, "probe") && Cmd_Argc() == 5) {
            float origin[3]{}; for (int i = 0; i < 3; ++i) if (!number(i + 2, origin[i])) { reply("{\"error\":\"invalid coordinates\"}"); return; }
            float eye[3]{origin[0], origin[1], origin[2] + 60};
            unsigned used = std::snprintf(result, sizeof(result), "{\"clear\":%s,\"headings\":[", clear(origin) ? "true" : "false");
            for (int heading = 0; heading < 16; ++heading) {
                const float yaw = heading * 22.5f;
                used += std::snprintf(result + used, sizeof(result) - used, "%s{\"yaw\":%.1f,\"rays\":[", heading ? "," : "", yaw);
                for (int offset = -2; offset <= 2; ++offset)
                    used += std::snprintf(result + used, sizeof(result) - used, "%s%.3f", offset != -2 ? "," : "", ray(eye, yaw + offset * 15, 0));
                used += std::snprintf(result + used, sizeof(result) - used, "]}");
            }
            std::snprintf(result + used, sizeof(result) - used, "]}"); reply(result); return;
        }
        if (!std::strcmp(action, "segment") && Cmd_Argc() == 8) {
            float points[2][3]{}; for (int i = 0; i < 6; ++i) if (!number(i + 2, points[i / 3][i % 3])) { reply("{\"error\":\"invalid coordinates\"}"); return; }
            const float mins[3]{-15, -15, 0}, maxs[3]{15, 15, 72}; trace_t trace{};
            CM_BoxTrace(&trace, points[0], points[1], mins, maxs, 0, MASK_PLAYERSOLID);
            std::snprintf(result, sizeof(result), "{\"clear\":%s,\"fraction\":%.6f}", !trace.startsolid && !trace.allsolid && trace.fraction == 1 ? "true" : "false", trace.fraction);
            reply(result); return;
        }
        float value = 0;
        if (Cmd_Argc() < 3 || !number(2, value) || value != std::floor(value)) { reply("{\"error\":\"invalid human port\"}"); return; }
        auto* entity = human(static_cast<int>(value));
        if (!entity) { reply("{\"error\":\"active human not found\"}"); return; }
        if (!std::strcmp(action, "weapon") && Cmd_Argc() == 5) {
            const unsigned weapon = G_GetWeaponIndexForName(Cmd_Argv(3));
            float camo = 0;
            if (!weapon || weapon >= 128 || !number(4, camo) || camo != std::floor(camo) || camo < 0 || camo > 7 ||
                !BG_GetWeaponDef(weapon)->gunXModel[int(camo)]) { reply("{\"error\":\"invalid loaded weapon/camo\"}"); return; }
            G_GivePlayerWeapon(&entity->client->ps, weapon, int(camo));
            G_InitializeAmmo(entity, weapon, int(camo), 0);
            char selection[32]; std::snprintf(selection, sizeof(selection), "a %u", weapon);
            SV_GameSendServerCommand(int(entity - g_entities), SV_CMD_RELIABLE, selection);
            std::snprintf(result, sizeof(result), "{\"weapon\":%u,\"camo\":%d}", weapon, int(camo)); reply(result); return;
        }
        if (!std::strcmp(action, "place") && Cmd_Argc() == 8) {
            float origin[3]{}, angles[3]{};
            for (int i = 0; i < 3; ++i) if (!number(i + 3, origin[i])) { reply("{\"error\":\"invalid coordinates\"}"); return; }
            if (!number(6, angles[1]) || !number(7, angles[0]) || std::abs(angles[0]) > 80 || !clear(origin)) { reply("{\"error\":\"invalid view or blocked position\"}"); return; }
            const bool previous = protectedIdentity(entity) ? protectedHuman.previousGod : (entity->flags & FL_GODMODE) != 0;
            releaseProtection();
            const int slot = int(entity - g_entities); const auto* client = &svs.clients[slot];
            protectedHuman = {slot, cm.checksum, sv_serverId_value, client->challenge, client->lastConnectTime,
                int(value), KHZ_CurrentServerSession(), KHZ_AdminClientGeneration(client), previous};
            // Only this owned benchmark human is protected; the fifteen bots
            // retain normal damage, weapons, respawns and decision cadence.
            entity->flags |= FL_GODMODE;
            std::memset(entity->client->ps.velocity, 0, sizeof(entity->client->ps.velocity));
            TeleportPlayer(entity, origin, angles);
            std::snprintf(result, sizeof(result), "{\"placed\":true,\"human_slot\":%d,\"previous_god\":%s}", int(entity - g_entities), previous ? "true" : "false");
            reply(result); return;
        }
        if (!std::strcmp(action, "release") && Cmd_Argc() == 3) {
            const bool matched = protectedIdentity(entity);
            if (matched) releaseProtection();
            std::snprintf(result, sizeof(result), "{\"released\":%s,\"god\":%s}", matched ? "true" : "false", entity->flags & FL_GODMODE ? "true" : "false");
            reply(result); return;
        }
        if (!std::strcmp(action, "status") && Cmd_Argc() == 3) {
            const auto& ps = entity->client->ps;
            float eye[3]{ps.origin[0], ps.origin[1], ps.origin[2] + ps.viewHeightCurrent};
            const unsigned used = std::snprintf(result, sizeof(result), "{\"human_slot\":%d,\"god\":%s,\"external_push_protected\":%s,\"origin\":[%.3f,%.3f,%.3f],\"angles\":[%.3f,%.3f,%.3f],\"rays\":[",
                int(entity - g_entities), entity->flags & FL_GODMODE ? "true" : "false", protectedIdentity(entity) ? "true" : "false", ps.origin[0], ps.origin[1], ps.origin[2], ps.viewangles[0], ps.viewangles[1], ps.viewangles[2]);
            unsigned end = used;
            for (int offset = -2; offset <= 2; ++offset)
                end += std::snprintf(result + end, sizeof(result) - end, "%s%.3f", offset != -2 ? "," : "", ray(eye, ps.viewangles[1] + offset * 15, ps.viewangles[0]));
            std::snprintf(result + end, sizeof(result) - end, "]}"); reply(result); return;
        }
        reply("{\"error\":\"expected probe, segment, place, status or release\"}"); return;
    }
#ifndef KISAK_DEDICATED
    if (CL_GetLocalClientConnectionState(0) != 9) { reply("{\"error\":\"client must be active\"}"); return; }
    SceneInput next{}; next.checksum = cm.checksum;
    if ((!std::strcmp(action, "fixed") || !std::strcmp(action, "fire")) && Cmd_Argc() == 4) {
        next.mode = !std::strcmp(action, "fire") ? 3 : 1;
        if (!number(2, next.angles[1]) || !number(3, next.angles[0]) || std::abs(next.angles[0]) > 80) { reply("{\"error\":\"invalid angles\"}"); return; }
    } else if (!std::strcmp(action, "shuttle") && Cmd_Argc() == 11) {
        next.mode = 2;
        for (int i = 0; i < 9; ++i) {
            float& value = i < 6 ? next.points[i / 3][i % 3] : next.look[i - 6];
            if (!number(i + 2, value)) { reply("{\"error\":\"invalid route\"}"); return; }
        }
    } else if (std::strcmp(action, "legacy") || Cmd_Argc() != 2) { reply("{\"error\":\"expected fixed, shuttle or legacy\"}"); return; }
    KHZ_CommandControlScope controls;
    input = next;
    std::snprintf(result, sizeof(result), "{\"mode\":%d,\"checksum\":%d}", input.mode, input.checksum); reply(result);
#endif
}
}
const char* KHZ_BenchmarkModelsJSON() {
#ifdef KISAK_DEDICATED
    return "{}";
#else
    static char result[8192];
    if (!Dvar_GetBool("khz_test_controls") || !Dvar_GetBool("khz_benchmark") || CL_GetLocalClientConnectionState(0) != 9)
        return "{\"error\":\"requires active isolated benchmark\"}";
    const auto* cg = CG_GetLocalClientGlobals(0);
    std::size_t used = 0;
    bool overflow = false;
    const auto append = [&](const char* format, auto... values) {
        if (overflow) return;
        const int count = std::snprintf(result + used, sizeof(result) - used, format, values...);
        if (count < 0 || std::size_t(count) >= sizeof(result) - used) { overflow = true; return; }
        used += count;
    };
    append("{\"time\":%d,\"ragdolls\":[", cg->time);
    bool comma = false;
    for (int index = 0; index < 32; ++index) {
        auto& body = ragdollBodies[index];
        if (body.references <= 0 || body.state < BS_TUNNEL_TEST) continue;
        const auto* orientations = Ragdoll_BodyBoneOrientations(&body);
        double radius = 0; unsigned invalid = 0;
        for (int bone = 0; bone < body.numBones + body.numLerpBones; ++bone) {
            double squared = 0;
            for (int axis = 0; axis < 3; ++axis) {
                const double delta = orientations[bone].origin[axis] - orientations[0].origin[axis];
                squared += delta * delta;
                if (!std::isfinite(delta)) ++invalid;
            }
            if (std::isfinite(squared)) radius = (std::max)(radius, std::sqrt(squared));
        }
        append(
            "%s{\"handle\":%d,\"entity\":%d,\"state\":%d,\"capture_ms\":%d,\"radius\":%.3f,\"invalid\":%u,\"origin\":[%.3f,%.3f,%.3f]}",
            comma ? "," : "", index + 1, body.dobj, int(body.state), body.velCaptureMsec, radius, invalid,
            orientations[0].origin[0], orientations[0].origin[1], orientations[0].origin[2]);
        comma = true;
    }
    append( "],\"corpses\":["); comma = false;
    for (int index = 64; index < 64 + MAX_CLIENT_CORPSES; ++index) {
        const auto* entity = CG_GetEntity(0, index);
        if (!entity->nextValid || entity->nextState.eType != ET_PLAYER_CORPSE) continue;
        const auto* object = Com_GetClientDObj(index, 0);
        double radius = 0; unsigned invalid = 0, computed = 0;
        if (object && object->skel.timeStamp == CL_GetSkelTimeStamp() && object->skel.mat) for (int bone = 0; bone < object->numBones; ++bone) {
            if (!(object->skel.partBits.skel[bone >> 5] & (0x80000000u >> (bone & 31)))) continue;
            ++computed;
            double squared = 0;
            for (int axis = 0; axis < 3; ++axis) {
                const double delta = object->skel.mat[bone].trans[axis] + cg->refdef.viewOffset[axis] - entity->pose.origin[axis];
                squared += delta * delta;
                if (!std::isfinite(delta)) ++invalid;
            }
            if (std::isfinite(squared)) radius = (std::max)(radius, std::sqrt(squared));
        }
        append(
            "%s{\"entity\":%d,\"handle\":%d,\"bones\":%u,\"computed\":%u,\"radius\":%.3f,\"invalid\":%u,\"origin\":[%.3f,%.3f,%.3f]}",
            comma ? "," : "", index, entity->pose.ragdollHandle, object ? object->numBones : 0, computed, radius, invalid,
            entity->pose.origin[0], entity->pose.origin[1], entity->pose.origin[2]);
        comma = true;
    }
    append("]}");
    return overflow ? "{\"error\":\"model diagnostics exceeded capacity\"}" : result;
#endif
}
bool KHZ_BenchmarkSceneInput(usercmd_s* command) {
#ifdef KISAK_DEDICATED
    return false;
#else
    if (!input.mode || input.checksum != cm.checksum || !Dvar_GetBool("khz_test_controls")) return false;
    const auto& ps = CL_GetLocalClientGlobals(0)->snap.ps;
    if (ps.pm_type != PM_NORMAL || ps.otherFlags & 2) return false;
    float yaw = input.angles[1], pitch = input.angles[0];
    command->forwardmove = command->rightmove = 0; command->buttons = input.mode == 3 ? 1 : 0;
    if (input.mode == 2) {
        float dx = input.points[input.goal][0] - ps.origin[0], dy = input.points[input.goal][1] - ps.origin[1];
        if (dx * dx + dy * dy < 144) {
            input.goal ^= 1; dx = input.points[input.goal][0] - ps.origin[0]; dy = input.points[input.goal][1] - ps.origin[1];
        }
        constexpr float radians = 0.017453292519943295f;
        yaw = std::atan2(input.look[1] - ps.origin[1], input.look[0] - ps.origin[0]) / radians;
        const float forward = dx * std::cos(yaw * radians) + dy * std::sin(yaw * radians);
        const float right = dx * std::sin(yaw * radians) - dy * std::cos(yaw * radians);
        const float length = (std::max)(std::abs(forward), std::abs(right));
        if (length > .001f) {
            command->forwardmove = static_cast<signed char>(std::lround(forward * 127 / length));
            command->rightmove = static_cast<signed char>(std::lround(right * 127 / length));
        }
    }
    // IW3 stores delta_angles in degrees; usercmd angles are packed turns.
    command->angles[0] = static_cast<unsigned short>(int((pitch - ps.delta_angles[0]) * (65536.0f / 360.0f)));
    command->angles[1] = static_cast<unsigned short>(int((yaw - ps.delta_angles[1]) * (65536.0f / 360.0f)));
    command->angles[2] = static_cast<unsigned short>(int(-ps.delta_angles[2] * (65536.0f / 360.0f)));
    return true;
#endif
}
bool KHZ_BenchmarkSceneProtectedHuman(const gentity_s* entity) { return protectedIdentity(entity); }
void KHZ_BenchmarkSceneInit() { static cmd_function_s registration{}; Cmd_AddCommandInternal("khz_scene", command, &registration); }
