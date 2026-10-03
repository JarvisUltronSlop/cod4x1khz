#include <universal/q_shared.h>
#include <bgame/bg_public.h>
#include <bgame/bg_local.h>
#include <qcommon/qcommon.h>
#include <qcommon/msg_mp.h>
#include <game_mp/g_public_mp.h>
#include <game_mp/g_main_mp.h>
#include <client_mp/client_mp.h>
#include "khz_runtime.h"
#include <memory>
#include <cstdio>
#include <cstring>
#include <cmath>

#ifdef COD4X1KHZ
extern thread_local animScriptData_t* globalScriptData;
#else
extern animScriptData_t* globalScriptData;
#endif

void __cdecl KHZ_ReplayTrace(trace_t* trace, const float* start, const float* mins,
    const float* maxs, const float* end, int, int mask) {
    // Static map collision only: no live entity transforms or player events.
    CM_BoxTrace(trace, start, end, mins, maxs, 0, mask);
    if (trace->fraction < 1 || trace->startsolid) { trace->hitType = TRACE_HITTYPE_ENTITY; trace->hitId = ENTITYNUM_WORLD; }
}

namespace {
std::uint64_t hashState(std::uint64_t hash, const playerState_s& state) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&state);
    for (unsigned i = 0; i < sizeof(state); ++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
    return hash;
}
int silentAlias(int, snd_alias_list_t*) { return 0; }
usercmd_s input(unsigned tick, const playerState_s& initial) {
    usercmd_s command{};
    command.serverTime = initial.commandTime + tick;
    command.weapon = initial.weapon;
    command.offHandIndex = initial.offHandIndex;
    command.angles[1] = static_cast<unsigned short>((tick / 2500 % 4) * 16384);
    if (tick < 1000 || (tick >= 2000 && tick < 3000) || (tick >= 4200 && tick < 5000)) command.forwardmove = 127;
    if (tick >= 1000 && tick < 2000) command.rightmove = 127;
    if (tick >= 2000 && tick < 2010) command.buttons |= BUTTON_JUMP;
    if (tick >= 3000 && tick < 3600) command.buttons |= BUTTON_CROUCH;
    if (tick >= 3600 && tick < 4200) command.buttons |= BUTTON_PRONE;
    if (tick >= 4200 && tick < 5000) command.buttons |= BUTTON_SPRINT;
    if (tick >= 5000 && tick < 6500) command.buttons |= BUTTON_ATTACK;
    if (tick >= 6600 && tick < 6610) command.buttons |= BUTTON_RELOAD;
    if (tick >= 7500 && tick < 7510) command.buttons |= BUTTON_FRAG;
    return command;
}
}

void KHZ_RunMovementReplay() {
    if (!com_sv_running->current.enabled) { Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_REPLAY needs a loaded server map\n"); return; }
    int client = -1;
    for (int i = 0; i < level.maxclients; ++i) {
        if (level.clients[i].sess.connected == CON_CONNECTED && level.clients[i].ps.pm_type == PM_NORMAL) { client = i; break; }
    }
    if (client < 0) { Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_REPLAY needs a spawned player\n"); return; }
    const auto initial = level.clients[client].ps;
    auto context = std::make_unique<bgs_t>(); // isolated benchmark, outside steady ticks
    auto* savedBgs = bgs;
    auto* savedScript = globalScriptData;
    std::uint64_t hashes[3]{};
    unsigned frames[3]{};
    constexpr unsigned caps[3] = {125, 240, 1000};
    const auto started = khz::counter();
    for (unsigned run = 0; run < 3; ++run) {
        *context = level_bgs;
        context->animScriptData.playSoundAlias = silentAlias;
        bgs = context.get(); globalScriptData = &context->animScriptData;
        auto state = initial;
        pmove_t move{};
        move.ps = &state; move.handler = 2; move.tracemask = MASK_PLAYERSOLID;
        move.maxSprintTimeMultiplier = level.clients[client].sess.cs.maxSprintTimeMultiplier;
        move.oldcmd = input(0, initial);
        std::uint64_t hash = 14695981039346656037ULL;
        for (unsigned tick = 1; tick <= 10000; ++tick) {
            context->time = initial.commandTime + tick; context->frametime = 1;
            move.cmd = input(tick, initial);
            Pmove(&move);
            hash = hashState(hash, state);
            // Presentation reads a copy; its cadence cannot advance simulation.
            if ((std::uint64_t(tick) * caps[run]) / 1000 > (std::uint64_t(tick - 1) * caps[run]) / 1000) {
                const auto presentation = state;
                volatile float observed = presentation.origin[0]; (void)observed;
                ++frames[run];
            }
        }
        hashes[run] = hash;
    }
    bgs = savedBgs; globalScriptData = savedScript;
    const auto passed = hashes[0] == hashes[1] && hashes[0] == hashes[2];
    const auto milliseconds = (khz::counter() - started) * 1000 / khz::frequency();
    char result[1024];
    std::snprintf(result, sizeof(result), "{\"passed\":%s,\"ticks_per_run\":10000,\"elapsed_ms\":%llu,\"hashes\":[\"%016llx\",\"%016llx\",\"%016llx\"],\"presentation_frames\":[%u,%u,%u],\"scope\":\"actual shared Pmove with static BSP collision and immutable presentation reads; full authoritative-world and real-renderer replay remain required\"}", passed ? "true" : "false", milliseconds, hashes[0], hashes[1], hashes[2], frames[0], frames[1], frames[2]);
    if (auto* file = std::fopen("khz_replay.json", "wb")) { std::fputs(result, file); std::fclose(file); }
    Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_REPLAY %s\n", result);
}

void KHZ_RunRewindProbe() {
    if (!com_sv_running->current.enabled || !g_antilag->current.enabled) {
        Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_REWIND {\"passed\":false,\"error\":\"a loaded map and g_antilag are required\"}\n"); return;
    }
    int tested = 0, failures = 0, restored = 0;
    float origin[3], mins[3], maxs[3];
    for (int client = 0; client < level.maxclients; ++client) {
        auto& entity = g_entities[client]; auto& player = level.clients[client];
        if (!entity.r.inuse || entity.health <= 0 || player.sess.connected != CON_CONNECTED || player.sess.sessionState != SESS_STATE_PLAYING) continue;
        ++tested;
        // Both ends of the retained range must exist. A current-tick request
        // and an expired request must never silently select a different pose.
        for (int age : {1, 100, 250}) {
            if (!KHZ_RewindClient(client, level.time - age, origin, mins, maxs)) ++failures;
        }
        for (int age : {0, 251}) {
            if (KHZ_RewindClient(client, level.time - age, origin, mins, maxs)) ++failures;
        }
        const int generation = entity.useCount;
        ++entity.useCount;
        if (KHZ_RewindClient(client, level.time - 100, origin, mins, maxs)) ++failures;
        entity.useCount = generation;
        const int spawnTime = player.lastSpawnTime;
        ++player.lastSpawnTime;
        if (KHZ_RewindClient(client, level.time - 100, origin, mins, maxs)) ++failures;
        player.lastSpawnTime = spawnTime;
    }
    struct Pose { float origin[3], mins[3], maxs[3]; bool linked; };
    std::array<Pose, 64> before{};
    for (int client = 0; client < level.maxclients; ++client) {
        const auto& entity = g_entities[client];
        std::memcpy(before[client].origin, entity.r.currentOrigin, sizeof(origin));
        std::memcpy(before[client].mins, entity.r.mins, sizeof(mins));
        std::memcpy(before[client].maxs, entity.r.maxs, sizeof(maxs));
        before[client].linked = entity.r.linked;
    }
    // Exercise the actual weapon rewind/relink/restore path repeatedly.
    for (int age : {1, 100, 250}) {
        AntilagClientStore store{};
        G_AntiLagRewindClientPos(level.time - age, &store);
        for (int client = 0; client < level.maxclients; ++client) {
            if (!store.clientMoved[client]) continue;
            const auto& entity = g_entities[client];
            if (!KHZ_RewindClient(client, level.time - age, origin, mins, maxs)
                || std::memcmp(origin, entity.r.currentOrigin, sizeof(origin))
                || std::memcmp(mins, entity.r.mins, sizeof(mins))
                || std::memcmp(maxs, entity.r.maxs, sizeof(maxs))) ++failures;
            ++restored;
        }
        G_AntiLag_RestoreClientPos(&store);
        for (int client = 0; client < level.maxclients; ++client) {
            const auto& entity = g_entities[client];
            if (std::memcmp(before[client].origin, entity.r.currentOrigin, sizeof(origin))
                || std::memcmp(before[client].mins, entity.r.mins, sizeof(mins))
                || std::memcmp(before[client].maxs, entity.r.maxs, sizeof(maxs))
                || before[client].linked != bool(entity.r.linked)) ++failures;
        }
    }
    const bool passed = tested > 0 && failures == 0 && restored == tested * 3;
    Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_REWIND {\"passed\":%s,\"players\":%i,\"failures\":%i,\"restorations\":%i,\"scope\":\"historical body origins and stance bounds, generation and respawn guards, exact 250 ms boundary, weapon rewind and restore; historical bone poses and hit correctness remain separate\"}\n",
        passed ? "true" : "false", tested, failures, restored);
}

void KHZ_RunPhysicsProbe() {
    if (!com_sv_running->current.enabled) {
        Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_PHYSICS {\"passed\":false,\"error\":\"a loaded map is required\"}\n"); return;
    }
    // Isolated entities exercise actual gameplay functions without allocating,
    // linking synthetic players, changing weapon definitions, or firing bullets.
    const auto weapon = G_GetWeaponIndexForName("rpg_mp");
    if (!weapon) { Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_PHYSICS {\"passed\":false,\"error\":\"RPG definition unavailable\"}\n"); return; }
    const auto* definition = BG_GetWeaponDef(weapon);
    const int oldTime = level.time, oldPrevious = level.previousTime, oldFrame = level.frametime;
    level.frametime = 1;
    int failures = 0;
    turretInfo_s turret{};
    gentity_s turretEntity{};
    turretEntity.s.eType = ET_MG42; turretEntity.pTurretInfo = &turret;
    float desired[2] = {90, 90};
    turret_UpdateTargetAngles(&turretEntity, desired, 0);
    const float turnFirst = turretEntity.s.lerp.u.turret.gunAngles[0];
    if (std::fabs(turnFirst - 0.2f) > .00001f) ++failures;
    for (int tick = 1; tick < 500; ++tick) turret_UpdateTargetAngles(&turretEntity, desired, 0);
    if (std::fabs(turretEntity.s.lerp.u.turret.gunAngles[0] - 90.0f) > .001f) ++failures;
    turret.fireSndDelay = 50;
    turret_UpdateSound(&turretEntity);
    const int soundFirst = turret.fireSndDelay;
    if (soundFirst != 49) ++failures;
    for (int tick = 1; tick < 50; ++tick) turret_UpdateSound(&turretEntity);
    if (turret.fireSndDelay != 0) ++failures;

    gentity_s javelin{};
    javelin.s.eType = ET_MISSILE;
    javelin.missile.stage = MISSILESTAGE_ASCENT;
    javelin.missile.flightMode = MISSILEFLIGHTMODE_TOP;
    float velocity[3] = {200, 0, 0}, target[3] = {1, 0, 0}, next[3];
    JavelinRotateVelocity(&javelin, velocity, target, next);
    const float climbAccel = Dvar_GetFloat("missileJavAccelClimb");
    const float climbLimit = Dvar_GetFloat("missileJavSpeedLimitClimb");
    const float expectedFirst = (std::min)(climbLimit, 200.0f + climbAccel * .001f);
    const float climbFirst = next[0];
    if (std::fabs(climbFirst - expectedFirst) > .001f) ++failures;
    for (int tick = 1; tick < 1000; ++tick) {
        std::memcpy(velocity, next, sizeof(velocity));
        JavelinRotateVelocity(&javelin, velocity, target, next);
    }
    if (std::fabs(next[0] - (std::min)(climbLimit, 200.0f + climbAccel)) > .2f) ++failures;

    gentity_s rocket{};
    rocket.s.eType = ET_MISSILE; rocket.s.weapon = weapon;
    rocket.handler = ENT_HANDLER_ROCKET; rocket.s.lerp.pos.trType = TR_ACCELERATE;
    rocket.s.lerp.pos.trDelta[0] = definition->iProjectileSpeed * .5f;
    rocket.s.lerp.u.missile.launchTime = oldTime;
    level.time = oldTime + 1; level.previousTime = oldTime;
    float position[3];
    MissileTrajectory(&rocket, position);
    float expectedSpeed = definition->iProjectileSpeed * .5f;
    if (definition->timeToAccelerate > 0) expectedSpeed += definition->iProjectileSpeed / definition->timeToAccelerate * .001f;
    const float rocketStep = position[0];
    if (!std::isfinite(position[0]) || std::fabs(rocket.s.lerp.pos.trDelta[0] - expectedSpeed) > .001f
        || std::fabs(position[0] - expectedSpeed * .001f) > .00001f) ++failures;
    scr_vehicle_s helicopter{};
    gentity_s helicopterEntity{};
    helicopterEntity.scr_vehicle = &helicopter;
    char controls[4] = {127, 0, 0, 0};
    float bodyAccel[3], rotAccel[3];
    HELI_CalcAccel(&helicopterEntity, controls, bodyAccel, rotAccel);
    const float helicopterAccel = bodyAccel[0];
    if (!std::isfinite(helicopterAccel) || helicopterAccel <= 0) ++failures;
    helicopter = {};
    level.frametime = 50;
    HELI_CalcAccel(&helicopterEntity, controls, bodyAccel, rotAccel);
    if (std::fabs(bodyAccel[0] - helicopterAccel) > (std::max)(.01f, helicopterAccel * .02f)) ++failures;
    level.frametime = 1;
    VehicleJitter jitter{};
    jitter.jitterPeriodMin = 1000; jitter.jitterPeriodMax = 1001;
    jitter.jitterAccel[0] = 100;
    HELI_UpdateJitter(&jitter); // zero ranges choose a zero target deterministically
    const float jitterFirst = jitter.jitterAccel[0];
    if (std::fabs(jitterFirst - 99.9f) > .001f) ++failures;
    for (int tick = 1; tick < 500; ++tick) {
        level.time = oldTime + tick;
        HELI_UpdateJitter(&jitter);
    }
    if (std::fabs(jitter.jitterAccel[0] - 50.0f) > .01f || !std::isfinite(jitter.jitterPos[0])) ++failures;
    // At half a phase millisecond per gameplay step, integer truncation used
    // to freeze texture motion. Compare the actual vehicle phase at 1/50 ms.
    const auto savedInfo = s_vehicleInfos[0];
    s_vehicleInfos[0].texScroll = 1; s_vehicleInfos[0].texScrollScale = 1;
    helicopter.infoIdx = 0;
    const float scale = Dvar_GetFloat("vehTextureScrollScale");
    helicopter.speed = 88.0f / (scale > 0 ? scale : 1.0f);
    helicopterEntity.s.lerp.u.vehicle.materialTime = 0;
    for (int tick = 0; tick < 1000; ++tick) VEH_UpdateMaterialTime(&helicopterEntity, .001f);
    const int materialFine = helicopterEntity.s.lerp.u.vehicle.materialTime;
    helicopterEntity.s.lerp.u.vehicle.materialTime = 0;
    for (int tick = 0; tick < 20; ++tick) VEH_UpdateMaterialTime(&helicopterEntity, .05f);
    const int materialCoarse = helicopterEntity.s.lerp.u.vehicle.materialTime;
    if (std::abs(materialFine - 500) > 1 || materialFine != materialCoarse) ++failures;
    s_vehicleInfos[0] = savedInfo;
    level.time = oldTime; level.previousTime = oldPrevious; level.frametime = oldFrame;
    Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_PHYSICS {\"passed\":%s,\"failures\":%i,\"turret_first_step_degrees\":%.6f,\"turret_sound_timer_after_1ms\":%i,\"javelin_speed_after_1ms\":%.6f,\"javelin_speed_after_1s\":%.6f,\"rocket_position_after_1ms\":%.6f,\"helicopter_acceleration\":%.6f,\"jitter_after_1ms\":%.6f,\"jitter_after_500ms\":%.6f,\"material_phase_fine\":%i,\"material_phase_coarse\":%i,\"scope\":\"actual turret rotation/sound timers, Javelin acceleration, rocket trajectory, helicopter acceleration/jitter and vehicle material phase; collision, firing and guided-target scenarios remain separate\"}\n",
        failures ? "false" : "true", failures, turnFirst, soundFirst, climbFirst, next[0], rocketStep, helicopterAccel, jitterFirst, jitter.jitterAccel[0], materialFine, materialCoarse);
}
