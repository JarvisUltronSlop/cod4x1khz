#include <universal/q_shared.h>
#include <universal/com_math.h>
#include "fx_system.h"
#include <physics/phys_local.h>
#include <qcommon/threads.h>
#include <win32/win_local.h>
#include <xanim/xmodel.h>

bool __cdecl FX_SpawnModelPhysics(
    FxSystem* system,
    FxEffect* effect,
    const FxElemDef* elemDef,
    int32_t randomSeed,
    FxElem* elem)
{
    float v6; // [esp+14h] [ebp-C8h]
    float v7; // [esp+18h] [ebp-C4h]
    float v8; // [esp+1Ch] [ebp-C0h]
    float velocity[3]; // [esp+4Ch] [ebp-90h] BYREF
    float angularVelocity[3]; // [esp+58h] [ebp-84h] BYREF
    FxElemVisuals visuals; // [esp+64h] [ebp-78h]
    float msecLifeSpan; // [esp+68h] [ebp-74h]
    float quat[4]; // [esp+6Ch] [ebp-70h] BYREF
    orientation_t orient; // [esp+7Ch] [ebp-60h] BYREF
    float worldOrigin[3]; // [esp+ACh] [ebp-30h] BYREF
    float axis[3][3]; // [esp+B8h] [ebp-24h] BYREF

    FX_GetOrientation(elemDef, &effect->frameAtSpawn, &effect->frameNow, randomSeed, &orient);
    FX_OrientationPosToWorldPos(&orient, elem->origin, worldOrigin);
    FX_GetElemAxis(elemDef, randomSeed, &orient, 0.0, axis);
    AxisToQuat(axis, quat);
    msecLifeSpan = (float)((((elemDef->lifeSpanMsec.amplitude + 1) * LOWORD(fx_randomTable[randomSeed + 17])) >> 16)
        + elemDef->lifeSpanMsec.base);
    FX_GetVelocityAtTime(elemDef, randomSeed, msecLifeSpan, 0.0, &orient, elem->baseVel, velocity);
    v8 = elemDef->angularVelocity[0].amplitude * fx_randomTable[randomSeed + 3] + elemDef->angularVelocity[0].base;
    angularVelocity[0] = v8 * 1000.0;
    v7 = elemDef->angularVelocity[1].amplitude * fx_randomTable[randomSeed + 4] + elemDef->angularVelocity[1].base;
    angularVelocity[1] = v7 * 1000.0;
    v6 = elemDef->angularVelocity[2].amplitude * fx_randomTable[randomSeed + 5] + elemDef->angularVelocity[2].base;
    angularVelocity[2] = v6 * 1000.0;
    Sys_EnterCriticalSection(CRITSECT_PHYSICS);
    visuals = FX_GetElemVisuals(elemDef, randomSeed);
    if (!visuals.model->physPreset)
        MyAssertHandler(".\\EffectsCore\\fx_system.cpp", 1853, 0, "%s", "visuals.model->physPreset");
    elem->physObjId = (intptr_t)Phys_ObjCreate(
        PHYS_WORLD_FX,
        worldOrigin,
        quat,
        velocity,
        visuals.model->physPreset);
    if (elem->physObjId)
    {
        Phys_ObjSetCollisionFromXModel(visuals.model, PHYS_WORLD_FX, (dxBody*)elem->physObjId);
        Phys_ObjSetAngularVelocity((dxBody*)elem->physObjId, angularVelocity);
    }
    Sys_LeaveCriticalSection(CRITSECT_PHYSICS);
    return elem->physObjId != 0;
}
