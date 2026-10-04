#include <universal/q_shared.h>
#if defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
#include <khz/khz_server_collision_checkpoint.h>
#endif
#include "qcommon.h"
#include "mem_track.h"
#include <universal/com_memory.h>
#include <database/database.h>
#include <win32/win_local.h>
#include "com_bsp.h"
#include <gfx_d3d/rb_backend.h>
#include <qcommon/threads.h>
#include <gfx_d3d/r_dvars.h>

//Line 53199 : 0006 : 006e75b8       struct clipMap_t cm        82e975b8     cm_load.obj
clipMap_t cm;
#if defined(COD4X1KHZ) && defined(KISAK_MP)
// Trace context 7 is deliberately disjoint from the renderer's 0..3 scratch.
// The unused cinematic/title/database slots do not receive partition storage.
cbrush_t g_box_brush[THREAD_CONTEXT_COUNT];
cmodel_t g_box_model[THREAD_CONTEXT_COUNT];
#else
cbrush_t g_box_brush[THREAD_CONTEXT_TRACE_COUNT];
cmodel_t g_box_model[THREAD_CONTEXT_TRACE_COUNT];
#endif

void __cdecl TRACK_cm_load()
{
    //track_static_alloc_internal(&cm, 284, "cm", 25);
    //track_static_alloc_internal(g_box_brush, 320, "g_box_brush", 25);
    //track_static_alloc_internal(g_box_model, 288, "g_box_model", 25);
}

static void CM_InitAllThreadData()
{
    uint32_t workerIndex; // [esp+0h] [ebp-4h]

    CM_InitThreadData(THREAD_CONTEXT_MAIN);
    CM_InitThreadData(THREAD_CONTEXT_BACKEND);

    // r_smp_worker threads
    for (workerIndex = 0; workerIndex < R_RENDER_WORKER_LIMIT; ++workerIndex)
        CM_InitThreadData(Sys_WorkerContext(workerIndex));

#if defined(COD4X1KHZ) && defined(KISAK_MP)
    // Called during the parked map load, before the simulation owner starts.
    // Allocation stays outside active ticks and remains pinned to this hunk.
    CM_InitThreadData(THREAD_CONTEXT_CLIENT_SIM);
#endif

#ifdef KISAK_SP
    CM_InitThreadData(THREAD_CONTEXT_SERVER);
#endif
}

void __cdecl CM_LoadMap(const char *name, int *checksum)
{
#if defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
    KHZ_ServerCollisionQueryScope collisionQuery(KHZ_COLLISION_CM_SCRATCH);
    if (!collisionQuery.allowed()) { return; }
    if (!KHZ_ServerCollisionAllowUnsupportedEffect("server_collision_map_load")) return;
    if (!KHZ_ServerCollisionInvalidateMapBinding()) return;
#endif
    if (!name || !*name)
        Com_Error(ERR_DROP, "CM_LoadMap: NULL name");
    CM_LoadMapData(name);
    CM_InitAllThreadData();
    cm.isInUse = 1;
    *checksum = cm.checksum;
}

extern TraceThreadInfo g_traceThreadInfo[THREAD_CONTEXT_COUNT];
void __cdecl CM_InitThreadData(uint32_t threadContext)
{
#if defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
    KHZ_ServerCollisionQueryScope collisionQuery(KHZ_COLLISION_CM_SCRATCH);
    if (!collisionQuery.allowed()) { return; }
    if (!KHZ_ServerCollisionAllowUnsupportedEffect("server_collision_trace_scratch_reallocation")) return;
    if (!KHZ_ServerCollisionInvalidateMapBinding()) return;
#endif
    TraceThreadInfo *traceThreadInfo; // [esp+8h] [ebp-4h]

#if defined(COD4X1KHZ) && defined(KISAK_MP)
    iassert(threadContext < THREAD_CONTEXT_TRACE_COUNT || threadContext == THREAD_CONTEXT_CLIENT_SIM
#ifdef COD4X1KHZ_NATIVE_VULKAN
        || threadContext == THREAD_CONTEXT_WORKER2 || threadContext == THREAD_CONTEXT_WORKER3
#endif
    );
#else
    bcassert(threadContext, THREAD_CONTEXT_TRACE_COUNT);
#endif

    traceThreadInfo = &g_traceThreadInfo[threadContext];
    traceThreadInfo->checkcount.global = 0;
    traceThreadInfo->checkcount.partitions = (int *)Hunk_Alloc(4 * cm.partitionCount, "CM_InitThreadData", 28);
    traceThreadInfo->box_brush = &g_box_brush[threadContext];
    memcpy(traceThreadInfo->box_brush, cm.box_brush, sizeof(cbrush_t));
    traceThreadInfo->box_model = &g_box_model[threadContext];
    memcpy(traceThreadInfo->box_model, &cm.box_model, sizeof(cmodel_t));
}

void __cdecl CM_LoadMapData(const char *name)
{
#if defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
    KHZ_ServerCollisionQueryScope collisionQuery(KHZ_COLLISION_CM_SCRATCH);
    if (!collisionQuery.allowed()) { return; }
    if (!KHZ_ServerCollisionAllowUnsupportedEffect("server_collision_map_data_load")) return;
    if (!KHZ_ServerCollisionInvalidateMapBinding()) return;
#endif
    if (IsFastFileLoad())
        CM_LoadMapData_FastFile(name);
    else
        CM_LoadMapData_LoadObj(name);
}

void __cdecl CM_LoadMapData_FastFile(const char *name)
{
#if defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
    KHZ_ServerCollisionQueryScope collisionQuery(KHZ_COLLISION_CM_SCRATCH);
    if (!collisionQuery.allowed()) { return; }
    if (!KHZ_ServerCollisionAllowUnsupportedEffect("server_collision_fastfile_map_lookup")) return;
    if (!KHZ_ServerCollisionInvalidateMapBinding()) return;
#endif
#ifdef KISAK_MP
    if (DB_FindXAssetHeader(ASSET_TYPE_CLIPMAP_PVS, name).clipMap != &cm)
        MyAssertHandler(".\\qcommon\\cm_load.cpp", 133, 0, "%s", "clipMap == &cm");
#elif KISAK_SP
    if (DB_FindXAssetHeader(ASSET_TYPE_CLIPMAP, name).clipMap != &cm)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\qcommon\\cm_load.cpp", 133, 0, "%s", "clipMap == &cm");
#endif
}

void __cdecl CM_Unload()
{
#if defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
    KHZ_ServerCollisionQueryScope collisionQuery(KHZ_COLLISION_CM_SCRATCH);
    if (!collisionQuery.allowed()) { return; }
    if (!KHZ_ServerCollisionAllowUnsupportedEffect("server_collision_map_unload")) return;
    if (!KHZ_ServerCollisionInvalidateMapBinding()) return;
#endif
    iassert( IsFastFileLoad() );
    if (cm.isInUse)
        Sys_Error("Cannot unload collision while it is in use");
}

int __cdecl CM_LeafCluster(uint32_t leafnum)
{
#if defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
    KHZ_ServerCollisionQueryScope collisionQuery(KHZ_COLLISION_CM_SCRATCH);
    if (!collisionQuery.allowed()) { return -1; }
#endif
    bcassert(leafnum, cm.numLeafs);
    return cm.leafs[leafnum].cluster;
}

void __cdecl CM_ModelBounds(uint32_t model, float *mins, float *maxs)
{
#if defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
    KHZ_ServerCollisionQueryScope collisionQuery(KHZ_COLLISION_CM_SCRATCH);
    if (!collisionQuery.allowed()) { if (mins) memset(mins, 0, 3 * sizeof(float)); if (maxs) memset(maxs, 0, 3 * sizeof(float)); return; }
#endif
    cmodel_t *cmodel; // eax

    cmodel = CM_ClipHandleToModel(model);

    mins[0] = cmodel->mins[0];
    mins[1] = cmodel->mins[1];
    mins[2] = cmodel->mins[2];

    maxs[0] = cmodel->maxs[0];
    maxs[1] = cmodel->maxs[1];
    maxs[2] = cmodel->maxs[2];
}

#if defined(COD4X1KHZ_SERVER_CHECKPOINT) && defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
#include <khz/khz_server_collision_load_checkpoint.inl>
#endif
