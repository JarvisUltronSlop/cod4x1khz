#include <universal/q_shared.h>
#include "ragdoll.h"
#include <qcommon/threads.h>
#ifdef KISAK_MP
#include <cgame_mp/cg_local_mp.h>
#else
#include <cgame/cg_main.h>
#endif

extern bool ragdollFirstInit;
namespace { int lastPresentationTime = -1; }

int Ragdoll_PresentationMsec(int time)
{
    const int elapsed = lastPresentationTime < 0 ? 0 : time - lastPresentationTime;
    lastPresentationTime = time;
    return elapsed > 0 ? elapsed : 0;
}

void __cdecl Ragdoll_Init()
{
    int i; // [esp+0h] [ebp-4h]

    iassert( Sys_IsMainThread() );
    iassert( ragdollFirstInit );
    if (!ragdollInited)
    {
        if (ragdoll_enable->current.enabled && ragdoll_max_simulating->current.integer < 8)
            Dvar_SetInt((dvar_s *)ragdoll_max_simulating, 8);
        memset(ragdollBodies, 0, sizeof(ragdollBodies));
        lastPresentationTime = -1;
        for (i = 0; i < 2; ++i)
        {
            ragdollDefs[i].bound = 0;
            ragdollDefs[i].inUse = 0;
        }
        ragdollInited = 1;
    }
}

void __cdecl Ragdoll_Shutdown()
{
    iassert( Sys_IsMainThread() );
    ragdollInited = 0;
}

void __cdecl Ragdoll_UpdateVelocityCapture(RagdollBody *body)
{
    BoneOrientation *snapshot; // [esp+0h] [ebp-4h]
    BoneOrientation *snapshota; // [esp+0h] [ebp-4h]

    iassert( body );
    if (body->stateFrames >= 1)
    {
        // These poses come from presentation time. A render stall can let the
        // simulation advance much further than Com_Frame's earlier duration.
        const int elapsed = CG_GetLocalClientGlobals(body->localClientNum)->time - body->velCaptureTime;
        if (elapsed <= 0) return; // A repeated presentation is not a new velocity sample.
        body->velCaptureMsec = elapsed;
        body->curOrientationBuffer ^= 1u;
        snapshota = Ragdoll_BodyBoneOrientations(body);
        Ragdoll_SnapshotAnimOrientations(body, snapshota);
        Ragdoll_BodyNewState(body, BodyState_t::BS_TUNNEL_TEST);
    }
    else
    {
        body->curOrientationBuffer ^= 1u;
        body->velCaptureTime = CG_GetLocalClientGlobals(body->localClientNum)->time;
        snapshot = Ragdoll_BodyBoneOrientations(body);
        Ragdoll_SnapshotAnimOrientations(body, snapshot);
    }
}
