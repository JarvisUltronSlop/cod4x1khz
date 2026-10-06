#include "cg_local_mp.h"
#include <EffectsCore/fx_system.h>

void __cdecl CG_ClearUnion(int32_t localClientNum, centity_s *cent)
{
    switch (cent->pose.eTypeUnion)
    {
    case ET_PLAYER:
        cent->pose.player = {};
        break;
    case ET_FX:
    case ET_LOOP_FX:
    {
        // On x64 the pointer is at offset eight: the retail eight-byte clear
        // left it pointing at an effect already released to garbage collection.
        auto* effect = cent->pose.fx.effect;
        cent->pose.fx = {};
        if (effect) FX_ThroughWithEffect(localClientNum, effect);
        break;
    }
    case ET_MG42:
        cent->pose.turret = {};
        break;
    case ET_HELICOPTER:
    case ET_VEHICLE:
        cent->pose.vehicle = {};
        break;
    default:
        break;
    }
    cent->pose.eTypeUnion = ET_GENERAL;
}

void __cdecl CG_SetUnionType(int32_t localClientNum, centity_s *cent)
{
    switch (cent->nextState.eType)
    {
    case ET_PLAYER:
    case ET_FX:
    case ET_LOOP_FX:
    case ET_MG42:
    case ET_HELICOPTER:
    case ET_VEHICLE:
        cent->pose.eTypeUnion = cent->nextState.eType;
        if (cent->pose.eTypeUnion != cent->nextState.eType)
            MyAssertHandler(".\\cgame_mp\\cg_ents_mp.cpp", 1674, 0, "%s", "cent->pose.eTypeUnion == cent->nextState.eType");
        break;
    default:
        cent->pose.eTypeUnion = ET_GENERAL;
        break;
    }
}

void __cdecl CG_UpdatePoseUnion(int32_t localClientNum, centity_s *cent)
{
    CG_ClearUnion(localClientNum, cent);
    CG_SetUnionType(localClientNum, cent);
}
