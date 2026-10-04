#include "fx_system.h"

void __cdecl FX_LinkSystemBuffers(FxSystem *system, FxSystemBuffers *systemBuffers)
{
    system->elems = systemBuffers->elems;
    system->effects = systemBuffers->effects;
    system->trails = systemBuffers->trails;
    system->trailElems = systemBuffers->trailElems;
    system->visState = systemBuffers->visState;
    system->deferredElems = systemBuffers->deferredElems;
}

int32_t __cdecl FX_EffectToHandle(FxSystem *system, FxEffect *effect)
{
    iassert(system);
    iassert(effect && effect >= &system->effects[0] && effect < &system->effects[FX_EFFECT_LIMIT]);

    return ((char *)effect - (char *)system->effects) / 4;
}


void __cdecl FX_RelocateSystem(FxSystem *system, intptr_t relocationDistance)
{
    if (relocationDistance)
    {
        system->visStateBufferRead = (const FxVisState *)((char *)system->visStateBufferRead + relocationDistance);
        system->visStateBufferWrite = (FxVisState *)((char *)system->visStateBufferWrite + relocationDistance);
    }
}
