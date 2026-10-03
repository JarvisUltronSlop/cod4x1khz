#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include <universal/q_shared.h>
#include "client_mp.h"

#include <gfx_d3d/r_scene.h>
#include <xanim/dobj_utils.h>
#if defined(COD4X1KHZ)
#include <khz/khz_client_owner.h>
#endif

namespace {
unsigned skeletonLocalClient() {
#if defined(COD4X1KHZ)
    if (KHZ_IsClientSimulationThread()) return 0;
#endif
    return R_GetLocalClientNum();
}
}

char *__cdecl CL_AllocSkelMemory(uint32_t size)
{
    volatile uint32_t *Addend; // [esp+0h] [ebp-Ch]
    char *result; // [esp+4h] [ebp-8h]
    int skelMemPos; // [esp+8h] [ebp-4h]
    uint32_t sizea; // [esp+14h] [ebp+8h]

    if (!size)
        MyAssertHandler(".\\client_mp\\cl_pose_mp.cpp", 30, 0, "%s", "size");
    sizea = (size + 15) & 0xFFFFFFF0;
    if (sizea > 0x3FFF0)
        MyAssertHandler(".\\client_mp\\cl_pose_mp.cpp", 33, 0, "%s", "size <= CL_SKEL_MEMORY_SIZE - SKEL_MEM_ALIGNMENT");
    if (!CL_ACTIVE_ARRAY[skeletonLocalClient()].skelMemoryStart)
        MyAssertHandler(".\\client_mp\\cl_pose_mp.cpp", 35, 0, "%s", "skel_glob->skelMemoryStart");
    Addend = &CL_ACTIVE_ARRAY[skeletonLocalClient()].skelMemPos;
    skelMemPos = InterlockedExchangeAdd(Addend, sizea);
    result = &CL_ACTIVE_ARRAY[skeletonLocalClient()].skelMemoryStart[skelMemPos];
    if (sizea + skelMemPos > 0x3FFF0)
        return 0;
    if (!result)
        MyAssertHandler(".\\client_mp\\cl_pose_mp.cpp", 46, 0, "%s", "result");
    return result;
}

int __cdecl CL_GetSkelTimeStamp()
{
    return CL_ACTIVE_ARRAY[skeletonLocalClient()].skelTimeStamp;
}

int warnCount_0;
int __cdecl CL_DObjCreateSkelForBones(const DObj_s *obj, int *partBits, DObjAnimMat **pMatOut)
{
    char *buf; // [esp+0h] [ebp-Ch]
    uint32_t len; // [esp+4h] [ebp-8h]
    int timeStamp; // [esp+8h] [ebp-4h]

    iassert(obj);

    timeStamp = CL_GetSkelTimeStamp();
    if (DObjSkelExists(obj, timeStamp))
    {
        *pMatOut = I_dmaGetDObjSkel(obj);
        return DObjSkelAreBonesUpToDate(obj, partBits);
    }
    else
    {
        len = DObjGetAllocSkelSize(obj);
        buf = CL_AllocSkelMemory(len);
        if (buf)
        {
            *pMatOut = (DObjAnimMat *)buf;
            DObjCreateSkel((DObj_s*)obj, buf, timeStamp);
            return 0;
        }
        else
        {
            *pMatOut = 0;
#if defined(COD4X1KHZ)
            if (KHZ_IsClientSimulationThread())
                Com_Error(ERR_DROP, "Gameplay pose skeleton arena capacity exhausted");
#endif
            if (warnCount_0 != timeStamp)
            {
                warnCount_0 = timeStamp;
                Com_PrintWarning(CON_CHANNEL_CLIENT, "WARNING: CL_SKEL_MEMORY_SIZE exceeded - not calculating skeleton\n");
            }
            return 1;
        }
    }
}

