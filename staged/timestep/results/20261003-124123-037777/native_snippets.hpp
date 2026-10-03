namespace current {

using namespace dependency;

int G_VehicleFrameMsec() {
#ifdef COD4X1KHZ
    return level.frametime > 0 ? level.frametime : 1;
#else
    return 50;
#endif
}

float G_VehicleFrameSeconds() { return G_VehicleFrameMsec() * 0.001f; }

float MissileStepSeconds() {
#ifdef COD4X1KHZ
    return level.frametime * 0.001f;
#else
    return 0.05f;
#endif
}

void __cdecl BG_EvaluateTrajectory(const trajectory_t *tr, int32_t atTime, float *result)
{
    float v3; // [esp+Ch] [ebp-7Ch]
    float v4; // [esp+14h] [ebp-74h]
    float scale; // [esp+24h] [ebp-64h]
    float v6; // [esp+28h] [ebp-60h]
    float phase; // [esp+74h] [ebp-14h]
    float phasea; // [esp+74h] [ebp-14h]
    float deltaTimea; // [esp+78h] [ebp-10h]
    float deltaTimeb; // [esp+78h] [ebp-10h]
    float deltaTime; // [esp+78h] [ebp-10h]
    float deltaTimec; // [esp+78h] [ebp-10h]
    float deltaTimed; // [esp+78h] [ebp-10h]
    float deltaTimee; // [esp+78h] [ebp-10h]
    float v[3]; // [esp+7Ch] [ebp-Ch] BYREF

    iassert(tr);

    iassert(!IS_NAN((tr->trBase)[0] && !IS_NAN((tr->trBase)[1] && !IS_NAN((tr->trBase)[2]))));
    iassert(!IS_NAN((tr->trDelta)[0] && !IS_NAN((tr->trDelta)[1] && !IS_NAN((tr->trDelta)[2]))));

    switch (tr->trType)
    {
    case TR_STATIONARY:
    case TR_INTERPOLATE:
    case TR_PHYSICS:
    case TR_RAGDOLL_INTERPOLATE:
        *result = tr->trBase[0];
        result[1] = tr->trBase[1];
        result[2] = tr->trBase[2];
        break;
    case TR_LINEAR:
    case TR_FIRST_RAGDOLL:
        deltaTimea = (double)(atTime - tr->trTime) * EQUAL_EPSILON;
        Vec3Mad(tr->trBase, deltaTimea, tr->trDelta, result);
        break;
    case TR_LINEAR_STOP:
        if (atTime > tr->trDuration + tr->trTime)
            atTime = tr->trDuration + tr->trTime;
        deltaTime = (double)(atTime - tr->trTime) * EQUAL_EPSILON;
        if (deltaTime < 0.0)
            deltaTime = 0.0;
        Vec3Mad(tr->trBase, deltaTime, tr->trDelta, result);
        break;
    case TR_SINE:
        deltaTimeb = (double)(atTime - tr->trTime) / (double)tr->trDuration;
        v6 = deltaTimeb * 3.141592741012573 + deltaTimeb * 3.141592741012573;
        scale = sin(v6);
        Vec3Mad(tr->trBase, scale, tr->trDelta, result);
        break;
    case TR_GRAVITY:
    case TR_RAGDOLL_GRAVITY:
        deltaTimec = (double)(atTime - tr->trTime) * EQUAL_EPSILON;
        Vec3Mad(tr->trBase, deltaTimec, tr->trDelta, result);
        result[2] = result[2] - deltaTimec * 400.0 * deltaTimec;
        break;
    case TR_ACCELERATE:
        if (atTime > tr->trDuration + tr->trTime)
            atTime = tr->trDuration + tr->trTime;
        deltaTimed = (double)(atTime - tr->trTime) * EQUAL_EPSILON;
        phase = Vec3Length(tr->trDelta) / ((double)tr->trDuration * EQUAL_EPSILON);
        Vec3NormalizeTo(tr->trDelta, result);
        v4 = phase * 0.5 * deltaTimed * deltaTimed;
        Vec3Mad(tr->trBase, v4, result, result);
        break;
    case TR_DECELERATE:
        if (atTime > tr->trDuration + tr->trTime)
            atTime = tr->trDuration + tr->trTime;
        deltaTimee = (double)(atTime - tr->trTime) * EQUAL_EPSILON;
        phasea = Vec3Length(tr->trDelta) / ((double)tr->trDuration * EQUAL_EPSILON);
        Vec3NormalizeTo(tr->trDelta, result);
        Vec3Mad(tr->trBase, deltaTimee, tr->trDelta, v);
        v3 = -phasea * 0.5 * deltaTimee * deltaTimee;
        Vec3Mad(v, v3, result, result);
        break;
    default:
        Com_Error(ERR_DROP, "BG_EvaluateTrajectory: unknown trType: %i", tr->trType);
        break;
    }

    iassert(!IS_NAN((tr->trBase)[0]) && !IS_NAN((tr->trBase)[1]) && !IS_NAN((tr->trBase)[2]));
    iassert(!IS_NAN((tr->trDelta)[0]) && !IS_NAN((tr->trDelta)[1]) && !IS_NAN((tr->trDelta)[2]));
}

void __cdecl G_SetOrigin(gentity_s *ent, const float *origin)
{
#if defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
    KHZ_ServerCollisionQueryScope collisionQuery(KHZ_COLLISION_SV_WORLD);
    if (!collisionQuery.allowed()) { return; }
    if (!KHZ_ServerCollisionAllowBrushTransform(ent, origin, nullptr, "server_collision_brush_set_origin")) return;
#endif
    ent->s.lerp.pos.trBase[0] = *origin;
    ent->s.lerp.pos.trBase[1] = origin[1];
    ent->s.lerp.pos.trBase[2] = origin[2];
    ent->s.lerp.pos.trType = TR_STATIONARY;
    ent->s.lerp.pos.trTime = 0;
    ent->s.lerp.pos.trDuration = 0;
    ent->s.lerp.pos.trDelta[0] = 0.0f;
    ent->s.lerp.pos.trDelta[1] = 0.0f;
    ent->s.lerp.pos.trDelta[2] = 0.0f;
    ent->r.currentOrigin[0] = *origin;
    ent->r.currentOrigin[1] = origin[1];
    ent->r.currentOrigin[2] = origin[2];
}

void Scr_Vehicle_Think(gentity_s* e) { ++observed.heliCalls; observed.integratedSeconds += G_VehicleFrameSeconds(); e->nextthink=level.time+G_VehicleFrameMsec(); }

void __cdecl G_RunItem(gentity_s *ent)
{
    int32_t passEntityNum; // [esp+10h] [ebp-ACh]
    float diff[6]; // [esp+24h] [ebp-98h] BYREF
    float v4; // [esp+3Ch] [ebp-80h]
    float v5; // [esp+40h] [ebp-7Ch]
    float v6; // [esp+44h] [ebp-78h]
    float v7; // [esp+48h] [ebp-74h]
    float v8; // [esp+4Ch] [ebp-70h]
    float v9; // [esp+50h] [ebp-6Ch]
    float *trDelta; // [esp+54h] [ebp-68h]
    float *trBase; // [esp+58h] [ebp-64h]
    float *currentOrigin; // [esp+5Ch] [ebp-60h]
    int32_t contents; // [esp+60h] [ebp-5Ch]
    float delta[3]; // [esp+64h] [ebp-58h] BYREF
    float origin[3]; // [esp+70h] [ebp-4Ch] BYREF
    float endpos[3]; // [esp+7Ch] [ebp-40h] BYREF
    trace_t tr; // [esp+88h] [ebp-34h] BYREF
    int32_t mask; // [esp+B4h] [ebp-8h]
    float dot; // [esp+B8h] [ebp-4h]

    //if (ent->s.eType == ET_PLAYER_CORPSE)
    //    MyAssertHandler(".\\game\\g_items.cpp", 1465, 0, "%s", "ent->s.eType != ET_PLAYER_CORPSE");

    if ((ent->s.groundEntityNum == ENTITYNUM_NONE
            || level.gentities[ent->s.groundEntityNum].s.lerp.pos.trType != TR_STATIONARY)
        && ent->s.lerp.pos.trType != TR_GRAVITY
        && ent->s.lerp.pos.trType != TR_RAGDOLL_GRAVITY
        && (ent->spawnflags & 1) == 0)
    {
        ent->s.lerp.pos.trType = TR_GRAVITY;
        ent->s.lerp.pos.trTime = level.time;
        trBase = ent->s.lerp.pos.trBase;
        currentOrigin = ent->r.currentOrigin;
        ent->s.lerp.pos.trBase[0] = ent->r.currentOrigin[0];
        trBase[1] = currentOrigin[1];
        trBase[2] = currentOrigin[2];
        trDelta = ent->s.lerp.pos.trDelta;
        ent->s.lerp.pos.trDelta[0] = 0.0;
        trDelta[1] = 0.0;
        trDelta[2] = 0.0;
    }
    v9 = ent->s.lerp.pos.trBase[0];
    if ((LODWORD(v9) & 0x7F800000) == 0x7F800000
        || (v8 = ent->s.lerp.pos.trBase[1], (LODWORD(v8) & 0x7F800000) == 0x7F800000)
        || (v7 = ent->s.lerp.pos.trBase[2], (LODWORD(v7) & 0x7F800000) == 0x7F800000))
    {
        MyAssertHandler(
            ".\\game\\g_items.cpp",
            1492,
            0,
            "%s",
            "!IS_NAN((ent->s.lerp.pos.trBase)[0]) && !IS_NAN((ent->s.lerp.pos.trBase)[1]) && !IS_NAN((ent->s.lerp.pos.trBase)[2])");
    }
    v6 = ent->s.lerp.pos.trDelta[0];
    if ((LODWORD(v6) & 0x7F800000) == 0x7F800000
        || (v5 = ent->s.lerp.pos.trDelta[1], (LODWORD(v5) & 0x7F800000) == 0x7F800000)
        || (v4 = ent->s.lerp.pos.trDelta[2], (LODWORD(v4) & 0x7F800000) == 0x7F800000))
    {
        MyAssertHandler(
            ".\\game\\g_items.cpp",
            1493,
            0,
            "%s",
            "!IS_NAN((ent->s.lerp.pos.trDelta)[0]) && !IS_NAN((ent->s.lerp.pos.trDelta)[1]) && !IS_NAN((ent->s.lerp.pos.trDelta)[2])");
    }
    if (ent->s.lerp.pos.trType != TR_STATIONARY && !ent->tagInfo)
    {
        BG_EvaluateTrajectory(&ent->s.lerp.pos, level.time + 50, origin);
        diff[5] = origin[0];
        if ((LODWORD(origin[0]) & 0x7F800000) == 0x7F800000
            || (diff[4] = origin[1], (LODWORD(origin[1]) & 0x7F800000) == 0x7F800000)
            || (diff[3] = origin[2], (LODWORD(origin[2]) & 0x7F800000) == 0x7F800000))
        {
            MyAssertHandler(
                ".\\game\\g_items.cpp",
                1505,
                0,
                "%s",
                "!IS_NAN((origin)[0]) && !IS_NAN((origin)[1]) && !IS_NAN((origin)[2])");
        }
        mask = G_ItemClipMask(ent);
        if ((mask & ent->r.contents) != 0)
            MyAssertHandler(".\\game\\g_items.cpp", 1511, 0, "%s", "!( ent->r.contents & mask )");
        Vec3Sub(origin, ent->r.currentOrigin, diff);
        if (Vec3LengthSq(diff) < 0.1000000014901161)
            origin[2] = origin[2] - 1.0;
        if (ent->r.ownerNum.isDefined())
        {
            passEntityNum = ent->r.ownerNum.entnum();
            G_TraceCapsule(&tr, ent->r.currentOrigin, ent->r.mins, ent->r.maxs, origin, passEntityNum, mask);
        }
        else
        {
            G_TraceCapsule(&tr, ent->r.currentOrigin, ent->r.mins, ent->r.maxs, origin, ENTITYNUM_NONE, mask);
        }
        if (tr.fraction >= 1.0)
        {
            ent->r.currentOrigin[0] = origin[0];
            ent->r.currentOrigin[1] = origin[1];
            ent->r.currentOrigin[2] = origin[2];
        }
        else
        {
            Vec3Lerp(ent->r.currentOrigin, origin, tr.fraction, endpos);
            if (!tr.startsolid && tr.fraction < 0.009999999776482582 && tr.normal[2] < 0.5)
            {
                Vec3Sub(origin, ent->r.currentOrigin, delta);
                dot = 1.0 - Vec3Dot(delta, tr.normal);
                Vec3Mad(origin, dot, tr.normal, origin);
                if (ent->r.ownerNum.isDefined())
                {
                    G_TraceCapsule(&tr, endpos, ent->r.mins, ent->r.maxs, origin, ent->r.ownerNum.entnum(), mask);
                }
                else
                {
                    G_TraceCapsule(&tr, endpos, ent->r.mins, ent->r.maxs, origin, ENTITYNUM_NONE, mask);
                }
                Vec3Lerp(endpos, origin, tr.fraction, endpos);
            }
            ent->s.lerp.pos.trType = TR_LINEAR_STOP;
            ent->s.lerp.pos.trTime = level.time;
            ent->s.lerp.pos.trDuration = 50;
            ent->s.lerp.pos.trBase[0] = ent->r.currentOrigin[0];
            ent->s.lerp.pos.trBase[1] = ent->r.currentOrigin[1];
            ent->s.lerp.pos.trBase[2] = ent->r.currentOrigin[2];
            Vec3Sub(endpos, ent->r.currentOrigin, ent->s.lerp.pos.trDelta);
            Vec3Scale(ent->s.lerp.pos.trDelta, 20.0, ent->s.lerp.pos.trDelta);
            ent->r.currentOrigin[0] = endpos[0];
            ent->r.currentOrigin[1] = endpos[1];
            ent->r.currentOrigin[2] = endpos[2];
        }
        SV_LinkEntity(ent);
        G_RunThink(ent);
        if (ent->r.inuse && tr.fraction < 0.009999999776482582)
        {
            if (tr.normal[2] <= 0.0 || (contents = SV_PointContents(ent->r.currentOrigin, -1, 0x80000000)) != 0)
            {
                G_FreeEntity(ent);
            }
            else
            {
                G_OrientItemToGround(ent, &tr);
                G_SetOrigin(ent, endpos);
                ent->s.groundEntityNum = Trace_GetEntityHitId(&tr);
                g_entities[ent->s.groundEntityNum].flags |= FL_GROUND_ENT;
                SV_LinkEntity(ent);
            }
        }
    }
    else
    {
        G_RunThink(ent);
    }
}

void __cdecl Helicopter_Think(gentity_s *ent)
{
    Scr_Vehicle_Think(ent);
    ent->nextthink = level.time + 50;
}

void __cdecl VEH_UpdateWeapon(gentity_s *ent)
{
#ifdef KISAK_SP
    scr_vehicle_s *veh;
    vehicle_info_t *info;
    gentity_s *player;
    const playerState_s *ps;
    int fireTime;
    int ownerEntNum;
    const char *flashTagName;
    float forward[3];
    float viewOrigin[3];
    float endPos[3];
    float flashPos[3];
    float barrelPos[3];
    trace_t trace;

    veh = ent->scr_vehicle;
    info = &s_vehicleInfos[veh->infoIdx];
    if (!ent->r.ownerNum.isDefined() || !ent->s.weapon)
        return;

    player = ent->r.ownerNum.ent();
    if (!player->client)
        MyAssertHandler(".\\game\\g_scr_vehicle.cpp", 2307, 0, "%s", "client");
    ps = &player->client->ps;
    if ((player->client->ps.eFlags & 0x80000) != 0)
        return;

    fireTime = veh->turret.fireTime;
    if (fireTime > 0)
    {
        veh->turret.fireTime = fireTime - 50;
    }
    else if ((player->client->pers.cmd.buttons & BUTTON_ATTACK) != 0
        && (player->client->ps.pm_flags & 0xC00) == 0)
    {
        Scr_Notify(ent, scr_const.turret_fire, 0);
    }

    veh->hasTarget = 1;

    if (veh->boneIndex.flash[0] < 0)
    {
        flashTagName = SL_ConvertToString(*s_flashTags[0]);
        Com_Error(ERR_DROP, "Player vehicle has no %s\n", flashTagName);
    }
    if (veh->boneIndex.barrel < 0)
        Com_Error(ERR_DROP, "Player vehicle has no tag_barrel\n");

    G_DObjGetWorldBoneIndexPos(ent, veh->boneIndex.flash[0], flashPos);
    G_DObjGetWorldBoneIndexPos(ent, veh->boneIndex.barrel, barrelPos);
    G_GetPlayerViewOrigin(ps, viewOrigin);

    if (vehHelicopterHeadSwayDontSwayTheTurret->current.enabled
        && info->type == VEH_HELICOPTER
        && (player->client->ps.eFlags & 0x40000) != 0)
    {
        AngleVectors(ent->r.currentAngles, forward, 0, 0);
    }
    else
    {
        G_GetPlayerViewDirection(player, forward, 0, 0);
    }

    endPos[0] = forward[0] * 10240.0f + viewOrigin[0];
    endPos[1] = forward[1] * 10240.0f + viewOrigin[1];
    endPos[2] = forward[2] * 10240.0f + viewOrigin[2];
    veh->targetOrigin[0] = endPos[0];
    veh->targetOrigin[1] = endPos[1];
    veh->targetOrigin[2] = endPos[2];

    ownerEntNum = ent->r.ownerNum.entnum();
    G_LocationalTrace(&trace, viewOrigin, endPos, ownerEntNum, 0x0280E091, 0);
    if (trace.fraction < 1.0f)
    {
        veh->targetOrigin[0] = (endPos[0] - viewOrigin[0]) * trace.fraction + viewOrigin[0];
        veh->targetOrigin[1] = (endPos[1] - viewOrigin[1]) * trace.fraction + viewOrigin[1];
        veh->targetOrigin[2] = (endPos[2] - viewOrigin[2]) * trace.fraction + viewOrigin[2];
    }

    // Capsule barrel→flash: if blocked, suppress firing.
    if (!G_TraceCapsuleComplete(barrelPos, vec3_origin, vec3_origin, flashPos, ent->s.number, ent->clipmask))
        veh->turret.barrelBlocked = 1;
#elif KISAK_MP
    iassert(ent);

    int playerEntNum = VehicleEntGunner(ent);

    if (playerEntNum != ENTITYNUM_NONE && ent->s.weapon)
    {
        scr_vehicle_s *veh = ent->scr_vehicle;
        iassert(veh);
        gclient_s *client = g_entities[playerEntNum].client;
        iassert(client);

        if (veh->turret.fireTime <= 0 || (veh->turret.fireTime -= level.frametime, veh->turret.fireTime <= 0))
        {
            if ((client->buttons & BUTTON_ATTACK) != 0)
                FireTurret(ent, &g_entities[playerEntNum]);
        }
    }
#endif
}

void __cdecl JavelinRotateVelocity(gentity_s *ent, const float *currentVel, const float *targetDir, float *resultVel)
{
    float currentDir[3]; // [esp+8h] [ebp-20h] BYREF
    float len; // [esp+14h] [ebp-14h]
    float resultDir[3]; // [esp+18h] [ebp-10h] BYREF
    float turnDiff; // [esp+24h] [ebp-4h]

    len = Vec3NormalizeTo(currentVel, currentDir);
    turnDiff = JavelinRotateDir(ent, currentDir, targetDir, resultDir);
    if (turnDiff < 30.0f)
    {
        if (ent->missile.stage == MISSILESTAGE_ASCENT || currentVel[2] > 0.0f)
        {
            len = missileJavAccelClimb->current.value * MissileStepSeconds() + len;
            if (missileJavSpeedLimitClimb->current.value < len)
                len = missileJavSpeedLimitClimb->current.value;
        }
        else
        {
            if (ent->missile.stage != MISSILESTAGE_DESCENT)
                MyAssertHandler(".\\game\\g_missile.cpp", 1804, 0, "%s", "ent->missile.stage == MISSILESTAGE_DESCENT");
            len = missileJavAccelDescend->current.value * MissileStepSeconds() + len;
            if (missileJavSpeedLimitDescend->current.value < len)
                len = missileJavSpeedLimitDescend->current.value;
        }
    }
    len = (1.0 - turnDiff / 180.0 * missileJavTurnDecel->current.value) * len;
    Vec3Scale(resultDir, len, resultVel);
}

}

namespace proposed {

using namespace dependency;

int G_VehicleFrameMsec() {
#ifdef COD4X1KHZ
    return level.frametime > 0 ? level.frametime : 1;
#else
    return 50;
#endif
}

float G_VehicleFrameSeconds() { return G_VehicleFrameMsec() * 0.001f; }

float MissileStepSeconds() {
#ifdef COD4X1KHZ
    return level.frametime * 0.001f;
#else
    return 0.05f;
#endif
}

void __cdecl BG_EvaluateTrajectory(const trajectory_t *tr, int32_t atTime, float *result)
{
    float v3; // [esp+Ch] [ebp-7Ch]
    float v4; // [esp+14h] [ebp-74h]
    float scale; // [esp+24h] [ebp-64h]
    float v6; // [esp+28h] [ebp-60h]
    float phase; // [esp+74h] [ebp-14h]
    float phasea; // [esp+74h] [ebp-14h]
    float deltaTimea; // [esp+78h] [ebp-10h]
    float deltaTimeb; // [esp+78h] [ebp-10h]
    float deltaTime; // [esp+78h] [ebp-10h]
    float deltaTimec; // [esp+78h] [ebp-10h]
    float deltaTimed; // [esp+78h] [ebp-10h]
    float deltaTimee; // [esp+78h] [ebp-10h]
    float v[3]; // [esp+7Ch] [ebp-Ch] BYREF

    iassert(tr);

    iassert(!IS_NAN((tr->trBase)[0] && !IS_NAN((tr->trBase)[1] && !IS_NAN((tr->trBase)[2]))));
    iassert(!IS_NAN((tr->trDelta)[0] && !IS_NAN((tr->trDelta)[1] && !IS_NAN((tr->trDelta)[2]))));

    switch (tr->trType)
    {
    case TR_STATIONARY:
    case TR_INTERPOLATE:
    case TR_PHYSICS:
    case TR_RAGDOLL_INTERPOLATE:
        *result = tr->trBase[0];
        result[1] = tr->trBase[1];
        result[2] = tr->trBase[2];
        break;
    case TR_LINEAR:
    case TR_FIRST_RAGDOLL:
        deltaTimea = (double)(atTime - tr->trTime) * EQUAL_EPSILON;
        Vec3Mad(tr->trBase, deltaTimea, tr->trDelta, result);
        break;
    case TR_LINEAR_STOP:
        if (atTime > tr->trDuration + tr->trTime)
            atTime = tr->trDuration + tr->trTime;
        deltaTime = (double)(atTime - tr->trTime) * EQUAL_EPSILON;
        if (deltaTime < 0.0)
            deltaTime = 0.0;
        Vec3Mad(tr->trBase, deltaTime, tr->trDelta, result);
        break;
    case TR_SINE:
        deltaTimeb = (double)(atTime - tr->trTime) / (double)tr->trDuration;
        v6 = deltaTimeb * 3.141592741012573 + deltaTimeb * 3.141592741012573;
        scale = sin(v6);
        Vec3Mad(tr->trBase, scale, tr->trDelta, result);
        break;
    case TR_GRAVITY:
    case TR_RAGDOLL_GRAVITY:
        deltaTimec = (double)(atTime - tr->trTime) * EQUAL_EPSILON;
        Vec3Mad(tr->trBase, deltaTimec, tr->trDelta, result);
        result[2] = result[2] - deltaTimec * 400.0 * deltaTimec;
        break;
    case TR_ACCELERATE:
        if (atTime > tr->trDuration + tr->trTime)
            atTime = tr->trDuration + tr->trTime;
        deltaTimed = (double)(atTime - tr->trTime) * EQUAL_EPSILON;
        phase = Vec3Length(tr->trDelta) / ((double)tr->trDuration * EQUAL_EPSILON);
        Vec3NormalizeTo(tr->trDelta, result);
        v4 = phase * 0.5 * deltaTimed * deltaTimed;
        Vec3Mad(tr->trBase, v4, result, result);
        break;
    case TR_DECELERATE:
        if (atTime > tr->trDuration + tr->trTime)
            atTime = tr->trDuration + tr->trTime;
        deltaTimee = (double)(atTime - tr->trTime) * EQUAL_EPSILON;
        phasea = Vec3Length(tr->trDelta) / ((double)tr->trDuration * EQUAL_EPSILON);
        Vec3NormalizeTo(tr->trDelta, result);
        Vec3Mad(tr->trBase, deltaTimee, tr->trDelta, v);
        v3 = -phasea * 0.5 * deltaTimee * deltaTimee;
        Vec3Mad(v, v3, result, result);
        break;
    default:
        Com_Error(ERR_DROP, "BG_EvaluateTrajectory: unknown trType: %i", tr->trType);
        break;
    }

    iassert(!IS_NAN((tr->trBase)[0]) && !IS_NAN((tr->trBase)[1]) && !IS_NAN((tr->trBase)[2]));
    iassert(!IS_NAN((tr->trDelta)[0]) && !IS_NAN((tr->trDelta)[1]) && !IS_NAN((tr->trDelta)[2]));
}

void __cdecl G_SetOrigin(gentity_s *ent, const float *origin)
{
#if defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
    KHZ_ServerCollisionQueryScope collisionQuery(KHZ_COLLISION_SV_WORLD);
    if (!collisionQuery.allowed()) { return; }
    if (!KHZ_ServerCollisionAllowBrushTransform(ent, origin, nullptr, "server_collision_brush_set_origin")) return;
#endif
    ent->s.lerp.pos.trBase[0] = *origin;
    ent->s.lerp.pos.trBase[1] = origin[1];
    ent->s.lerp.pos.trBase[2] = origin[2];
    ent->s.lerp.pos.trType = TR_STATIONARY;
    ent->s.lerp.pos.trTime = 0;
    ent->s.lerp.pos.trDuration = 0;
    ent->s.lerp.pos.trDelta[0] = 0.0f;
    ent->s.lerp.pos.trDelta[1] = 0.0f;
    ent->s.lerp.pos.trDelta[2] = 0.0f;
    ent->r.currentOrigin[0] = *origin;
    ent->r.currentOrigin[1] = origin[1];
    ent->r.currentOrigin[2] = origin[2];
}

void Scr_Vehicle_Think(gentity_s* e) { ++observed.heliCalls; observed.integratedSeconds += G_VehicleFrameSeconds(); e->nextthink=level.time+G_VehicleFrameMsec(); }

void __cdecl G_RunItem(gentity_s *ent)
{
    int32_t passEntityNum; // [esp+10h] [ebp-ACh]
    float diff[6]; // [esp+24h] [ebp-98h] BYREF
    float v4; // [esp+3Ch] [ebp-80h]
    float v5; // [esp+40h] [ebp-7Ch]
    float v6; // [esp+44h] [ebp-78h]
    float v7; // [esp+48h] [ebp-74h]
    float v8; // [esp+4Ch] [ebp-70h]
    float v9; // [esp+50h] [ebp-6Ch]
    float *trDelta; // [esp+54h] [ebp-68h]
    float *trBase; // [esp+58h] [ebp-64h]
    float *currentOrigin; // [esp+5Ch] [ebp-60h]
    int32_t contents; // [esp+60h] [ebp-5Ch]
    float delta[3]; // [esp+64h] [ebp-58h] BYREF
    float origin[3]; // [esp+70h] [ebp-4Ch] BYREF
    float endpos[3]; // [esp+7Ch] [ebp-40h] BYREF
    trace_t tr; // [esp+88h] [ebp-34h] BYREF
    int32_t mask; // [esp+B4h] [ebp-8h]
    float dot; // [esp+B8h] [ebp-4h]
#ifdef COD4X1KHZ
    const int itemStepMsec = level.frametime > 0 ? level.frametime : 1;
    bool needsSupportProbe = false;
#else
    const int itemStepMsec = 50;
#endif

    //if (ent->s.eType == ET_PLAYER_CORPSE)
    //    MyAssertHandler(".\\game\\g_items.cpp", 1465, 0, "%s", "ent->s.eType != ET_PLAYER_CORPSE");

    if ((ent->s.groundEntityNum == ENTITYNUM_NONE
            || level.gentities[ent->s.groundEntityNum].s.lerp.pos.trType != TR_STATIONARY)
        && ent->s.lerp.pos.trType != TR_GRAVITY
        && ent->s.lerp.pos.trType != TR_RAGDOLL_GRAVITY
        && (ent->spawnflags & 1) == 0)
    {
        ent->s.lerp.pos.trType = TR_GRAVITY;
        ent->s.lerp.pos.trTime = level.time;
        trBase = ent->s.lerp.pos.trBase;
        currentOrigin = ent->r.currentOrigin;
        ent->s.lerp.pos.trBase[0] = ent->r.currentOrigin[0];
        trBase[1] = currentOrigin[1];
        trBase[2] = currentOrigin[2];
        trDelta = ent->s.lerp.pos.trDelta;
        ent->s.lerp.pos.trDelta[0] = 0.0;
        trDelta[1] = 0.0;
        trDelta[2] = 0.0;
    }
    v9 = ent->s.lerp.pos.trBase[0];
    if ((LODWORD(v9) & 0x7F800000) == 0x7F800000
        || (v8 = ent->s.lerp.pos.trBase[1], (LODWORD(v8) & 0x7F800000) == 0x7F800000)
        || (v7 = ent->s.lerp.pos.trBase[2], (LODWORD(v7) & 0x7F800000) == 0x7F800000))
    {
        MyAssertHandler(
            ".\\game\\g_items.cpp",
            1492,
            0,
            "%s",
            "!IS_NAN((ent->s.lerp.pos.trBase)[0]) && !IS_NAN((ent->s.lerp.pos.trBase)[1]) && !IS_NAN((ent->s.lerp.pos.trBase)[2])");
    }
    v6 = ent->s.lerp.pos.trDelta[0];
    if ((LODWORD(v6) & 0x7F800000) == 0x7F800000
        || (v5 = ent->s.lerp.pos.trDelta[1], (LODWORD(v5) & 0x7F800000) == 0x7F800000)
        || (v4 = ent->s.lerp.pos.trDelta[2], (LODWORD(v4) & 0x7F800000) == 0x7F800000))
    {
        MyAssertHandler(
            ".\\game\\g_items.cpp",
            1493,
            0,
            "%s",
            "!IS_NAN((ent->s.lerp.pos.trDelta)[0]) && !IS_NAN((ent->s.lerp.pos.trDelta)[1]) && !IS_NAN((ent->s.lerp.pos.trDelta)[2])");
    }
    if (ent->s.lerp.pos.trType != TR_STATIONARY && !ent->tagInfo)
    {
        BG_EvaluateTrajectory(&ent->s.lerp.pos, level.time + itemStepMsec, origin);
        diff[5] = origin[0];
        if ((LODWORD(origin[0]) & 0x7F800000) == 0x7F800000
            || (diff[4] = origin[1], (LODWORD(origin[1]) & 0x7F800000) == 0x7F800000)
            || (diff[3] = origin[2], (LODWORD(origin[2]) & 0x7F800000) == 0x7F800000))
        {
            MyAssertHandler(
                ".\\game\\g_items.cpp",
                1505,
                0,
                "%s",
                "!IS_NAN((origin)[0]) && !IS_NAN((origin)[1]) && !IS_NAN((origin)[2])");
        }
        mask = G_ItemClipMask(ent);
        if ((mask & ent->r.contents) != 0)
            MyAssertHandler(".\\game\\g_items.cpp", 1511, 0, "%s", "!( ent->r.contents & mask )");
        Vec3Sub(origin, ent->r.currentOrigin, diff);
#ifdef COD4X1KHZ
        needsSupportProbe = Vec3LengthSq(diff) < 0.1000000014901161;
#else
        if (Vec3LengthSq(diff) < 0.1000000014901161)
            origin[2] = origin[2] - 1.0;
#endif
        if (ent->r.ownerNum.isDefined())
        {
            passEntityNum = ent->r.ownerNum.entnum();
            G_TraceCapsule(&tr, ent->r.currentOrigin, ent->r.mins, ent->r.maxs, origin, passEntityNum, mask);
        }
        else
        {
            G_TraceCapsule(&tr, ent->r.currentOrigin, ent->r.mins, ent->r.maxs, origin, ENTITYNUM_NONE, mask);
        }
        if (tr.fraction >= 1.0)
        {
            ent->r.currentOrigin[0] = origin[0];
            ent->r.currentOrigin[1] = origin[1];
            ent->r.currentOrigin[2] = origin[2];
        }
        else
        {
            Vec3Lerp(ent->r.currentOrigin, origin, tr.fraction, endpos);
            if (!tr.startsolid && tr.fraction < 0.009999999776482582 && tr.normal[2] < 0.5)
            {
                Vec3Sub(origin, ent->r.currentOrigin, delta);
                dot = 1.0 - Vec3Dot(delta, tr.normal);
                Vec3Mad(origin, dot, tr.normal, origin);
                if (ent->r.ownerNum.isDefined())
                {
                    G_TraceCapsule(&tr, endpos, ent->r.mins, ent->r.maxs, origin, ent->r.ownerNum.entnum(), mask);
                }
                else
                {
                    G_TraceCapsule(&tr, endpos, ent->r.mins, ent->r.maxs, origin, ENTITYNUM_NONE, mask);
                }
                Vec3Lerp(endpos, origin, tr.fraction, endpos);
            }
            ent->s.lerp.pos.trType = TR_LINEAR_STOP;
            ent->s.lerp.pos.trTime = level.time;
            ent->s.lerp.pos.trDuration = itemStepMsec;
            ent->s.lerp.pos.trBase[0] = ent->r.currentOrigin[0];
            ent->s.lerp.pos.trBase[1] = ent->r.currentOrigin[1];
            ent->s.lerp.pos.trBase[2] = ent->r.currentOrigin[2];
            Vec3Sub(endpos, ent->r.currentOrigin, ent->s.lerp.pos.trDelta);
            Vec3Scale(ent->s.lerp.pos.trDelta, 1000.0 / itemStepMsec, ent->s.lerp.pos.trDelta);
            ent->r.currentOrigin[0] = endpos[0];
            ent->r.currentOrigin[1] = endpos[1];
            ent->r.currentOrigin[2] = endpos[2];
        }
#ifdef COD4X1KHZ
        // Probe support independently; a clear probe is not additional falling motion.
        if (needsSupportProbe && tr.fraction >= 1.0f)
        {
            trace_t supportTrace;
            float supportEnd[3];
            supportEnd[0] = ent->r.currentOrigin[0];
            supportEnd[1] = ent->r.currentOrigin[1];
            supportEnd[2] = ent->r.currentOrigin[2] - 1.0f;
            const int supportPassEntity = ent->r.ownerNum.isDefined()
                ? ent->r.ownerNum.entnum() : ENTITYNUM_NONE;
            G_TraceCapsule(&supportTrace, ent->r.currentOrigin, ent->r.mins,
                ent->r.maxs, supportEnd, supportPassEntity, mask);
            if (supportTrace.fraction < 0.009999999776482582
                && supportTrace.normal[2] > 0.0f)
            {
                Vec3Lerp(ent->r.currentOrigin, supportEnd, supportTrace.fraction, endpos);
                tr = supportTrace;
            }
        }
#endif
        SV_LinkEntity(ent);
        G_RunThink(ent);
        if (ent->r.inuse && tr.fraction < 0.009999999776482582)
        {
            if (tr.normal[2] <= 0.0 || (contents = SV_PointContents(ent->r.currentOrigin, -1, 0x80000000)) != 0)
            {
                G_FreeEntity(ent);
            }
            else
            {
                G_OrientItemToGround(ent, &tr);
                G_SetOrigin(ent, endpos);
                ent->s.groundEntityNum = Trace_GetEntityHitId(&tr);
                g_entities[ent->s.groundEntityNum].flags |= FL_GROUND_ENT;
                SV_LinkEntity(ent);
            }
        }
    }
    else
    {
        G_RunThink(ent);
    }
}

void __cdecl Helicopter_Think(gentity_s *ent)
{
    Scr_Vehicle_Think(ent);
    ent->nextthink = level.time + G_VehicleFrameMsec();
}

void __cdecl VEH_UpdateWeapon(gentity_s *ent)
{
#ifdef KISAK_SP
    scr_vehicle_s *veh;
    vehicle_info_t *info;
    gentity_s *player;
    const playerState_s *ps;
    int fireTime;
    int ownerEntNum;
    const char *flashTagName;
    float forward[3];
    float viewOrigin[3];
    float endPos[3];
    float flashPos[3];
    float barrelPos[3];
    trace_t trace;

    veh = ent->scr_vehicle;
    info = &s_vehicleInfos[veh->infoIdx];
    if (!ent->r.ownerNum.isDefined() || !ent->s.weapon)
        return;

    player = ent->r.ownerNum.ent();
    if (!player->client)
        MyAssertHandler(".\\game\\g_scr_vehicle.cpp", 2307, 0, "%s", "client");
    ps = &player->client->ps;
    if ((player->client->ps.eFlags & 0x80000) != 0)
        return;

    fireTime = veh->turret.fireTime;
    if (fireTime > 0)
    {
        veh->turret.fireTime = fireTime - 50;
    }
    else if ((player->client->pers.cmd.buttons & BUTTON_ATTACK) != 0
        && (player->client->ps.pm_flags & 0xC00) == 0)
    {
        Scr_Notify(ent, scr_const.turret_fire, 0);
    }

    veh->hasTarget = 1;

    if (veh->boneIndex.flash[0] < 0)
    {
        flashTagName = SL_ConvertToString(*s_flashTags[0]);
        Com_Error(ERR_DROP, "Player vehicle has no %s\n", flashTagName);
    }
    if (veh->boneIndex.barrel < 0)
        Com_Error(ERR_DROP, "Player vehicle has no tag_barrel\n");

    G_DObjGetWorldBoneIndexPos(ent, veh->boneIndex.flash[0], flashPos);
    G_DObjGetWorldBoneIndexPos(ent, veh->boneIndex.barrel, barrelPos);
    G_GetPlayerViewOrigin(ps, viewOrigin);

    if (vehHelicopterHeadSwayDontSwayTheTurret->current.enabled
        && info->type == VEH_HELICOPTER
        && (player->client->ps.eFlags & 0x40000) != 0)
    {
        AngleVectors(ent->r.currentAngles, forward, 0, 0);
    }
    else
    {
        G_GetPlayerViewDirection(player, forward, 0, 0);
    }

    endPos[0] = forward[0] * 10240.0f + viewOrigin[0];
    endPos[1] = forward[1] * 10240.0f + viewOrigin[1];
    endPos[2] = forward[2] * 10240.0f + viewOrigin[2];
    veh->targetOrigin[0] = endPos[0];
    veh->targetOrigin[1] = endPos[1];
    veh->targetOrigin[2] = endPos[2];

    ownerEntNum = ent->r.ownerNum.entnum();
    G_LocationalTrace(&trace, viewOrigin, endPos, ownerEntNum, 0x0280E091, 0);
    if (trace.fraction < 1.0f)
    {
        veh->targetOrigin[0] = (endPos[0] - viewOrigin[0]) * trace.fraction + viewOrigin[0];
        veh->targetOrigin[1] = (endPos[1] - viewOrigin[1]) * trace.fraction + viewOrigin[1];
        veh->targetOrigin[2] = (endPos[2] - viewOrigin[2]) * trace.fraction + viewOrigin[2];
    }

    // Capsule barrel→flash: if blocked, suppress firing.
    if (!G_TraceCapsuleComplete(barrelPos, vec3_origin, vec3_origin, flashPos, ent->s.number, ent->clipmask))
        veh->turret.barrelBlocked = 1;
#elif KISAK_MP
    iassert(ent);

    int playerEntNum = VehicleEntGunner(ent);

    if (playerEntNum != ENTITYNUM_NONE && ent->s.weapon)
    {
        scr_vehicle_s *veh = ent->scr_vehicle;
        iassert(veh);
        gclient_s *client = g_entities[playerEntNum].client;
        iassert(client);

        if (veh->turret.fireTime <= 0 || (veh->turret.fireTime -= level.frametime, veh->turret.fireTime <= 0))
        {
            if ((client->buttons & BUTTON_ATTACK) != 0)
                FireTurret(ent, &g_entities[playerEntNum]);
        }
    }
#endif
}

void __cdecl JavelinRotateVelocity(gentity_s *ent, const float *currentVel, const float *targetDir, float *resultVel)
{
    float currentDir[3]; // [esp+8h] [ebp-20h] BYREF
    float len; // [esp+14h] [ebp-14h]
    float resultDir[3]; // [esp+18h] [ebp-10h] BYREF
    float turnDiff; // [esp+24h] [ebp-4h]

    len = Vec3NormalizeTo(currentVel, currentDir);
    turnDiff = JavelinRotateDir(ent, currentDir, targetDir, resultDir);
    if (turnDiff < 30.0f)
    {
        if (ent->missile.stage == MISSILESTAGE_ASCENT || currentVel[2] > 0.0f)
        {
            len = missileJavAccelClimb->current.value * MissileStepSeconds() + len;
            if (missileJavSpeedLimitClimb->current.value < len)
                len = missileJavSpeedLimitClimb->current.value;
        }
        else
        {
            if (ent->missile.stage != MISSILESTAGE_DESCENT)
                MyAssertHandler(".\\game\\g_missile.cpp", 1804, 0, "%s", "ent->missile.stage == MISSILESTAGE_DESCENT");
            len = missileJavAccelDescend->current.value * MissileStepSeconds() + len;
            if (missileJavSpeedLimitDescend->current.value < len)
                len = missileJavSpeedLimitDescend->current.value;
        }
    }
#ifdef COD4X1KHZ
    // This is stock 50 ms speed retention, not a retention to repeat every 1 ms.
    const double stockRetention = 1.0 - turnDiff / 180.0 * missileJavTurnDecel->current.value;
    const double step = MissileStepSeconds();
    const double retention = step == double(0.05f)
        ? stockRetention : pow(stockRetention, step / double(0.05f));
    len = retention * len;
#else
    len = (1.0 - turnDiff / 180.0 * missileJavTurnDecel->current.value) * len;
#endif
    Vec3Scale(resultDir, len, resultVel);
}

}