#include <universal/q_shared.h>
#include "r_dpvs.h"
#include "r_bsp.h"
#include "r_utils.h"
#include <qcommon/threads.h>

void __cdecl R_AddEntToCell(FilterEntInfo *entInfo, uint32_t cellIndex)
{
    uint32_t bit; // [esp+0h] [ebp-1Ch]
    uint32_t localClientNum; // [esp+4h] [ebp-18h]
    uint32_t offset; // [esp+8h] [ebp-14h]
    uint32_t *entCellBits; // [esp+Ch] [ebp-10h]
    uint32_t entnum; // [esp+14h] [ebp-8h]

    iassert( Sys_IsMainThread() );
    localClientNum = entInfo->localClientNum;
    if (entInfo->localClientNum >= gfxCfg.maxClientViews)
        MyAssertHandler(
            ".\\r_dpvs.cpp",
            1840,
            0,
            "localClientNum doesn't index gfxCfg.maxClientViews\n\t%i not in [0, %i)",
            localClientNum,
            gfxCfg.maxClientViews);
    entnum = entInfo->entnum;
    iassert( gfxCfg.maxClientViews * gfxCfg.entCount <= MAX_TOTAL_ENT_COUNT );
    if ((gfxCfg.entCount & 7) != 0)
        MyAssertHandler(
            ".\\r_dpvs.cpp",
            1846,
            0,
            "%s\n\t(gfxCfg.entCount) = %i",
            "(!(gfxCfg.entCount & 7))",
            gfxCfg.entCount);
    offset = localClientNum * (gfxCfg.entCount >> 5);
    if (offset >= 0x80)
        MyAssertHandler(
            ".\\r_dpvs.cpp",
            1849,
            0,
            "offset doesn't index MAX_TOTAL_ENT_COUNT >> 5\n\t%i not in [0, %i)",
            offset,
            128);
    entCellBits = &rgp.world->dpvsPlanes.sceneEntCellBits[128 * entInfo->cellOffset + 128 * cellIndex + offset];
    bit = 0x80000000 >> (entnum & 0x1F);
    entCellBits[entnum >> 5] |= bit;
    dpvsGlob.entVisBits[localClientNum][entnum >> 5] |= bit;
    scene.dpvs.entInfo[localClientNum][entnum].bmodel = entInfo->info.bmodel;
}

void __cdecl R_InitSceneData(int localClientNum)
{
    uint32_t cellIndex; // [esp+0h] [ebp-10h]
    uint32_t offset; // [esp+4h] [ebp-Ch]
    uint32_t cellCount; // [esp+Ch] [ebp-4h]

    iassert( Sys_IsMainThread() );
    iassert( rgp.world );
    iassert( gfxCfg.maxClientViews * gfxCfg.entCount <= MAX_TOTAL_ENT_COUNT );
    cellCount = rgp.world->dpvsPlanes.cellCount;
    if ((gfxCfg.entCount & 0x1F) != 0)
        MyAssertHandler(
            ".\\r_dpvs.cpp",
            3153,
            0,
            "%s\n\t(gfxCfg.entCount) = %i",
            "(!(gfxCfg.entCount & 31))",
            gfxCfg.entCount);
    offset = localClientNum * (gfxCfg.entCount >> 5);
    if (offset >= 0x80)
        MyAssertHandler(
            ".\\r_dpvs.cpp",
            3156,
            0,
            "offset doesn't index MAX_TOTAL_ENT_COUNT >> 5\n\t%i not in [0, %i)",
            offset,
            128);
    for (cellIndex = 0; cellIndex < 2 * cellCount; ++cellIndex)
        Com_Memset(&rgp.world->dpvsPlanes.sceneEntCellBits[128 * cellIndex + offset], 0, 4 * (gfxCfg.entCount >> 5));
    memset((uint8_t *)dpvsGlob.entVisBits[localClientNum], 0, 4 * (gfxCfg.entCount >> 5));
    memset(scene.dpvs.entInfo[localClientNum], 0, sizeof(GfxEntCellRefInfo) * gfxCfg.entCount);
}

void __cdecl R_InitSceneBuffers()
{
    uint32_t localClientNum; // [esp+0h] [ebp-8h]
    uint32_t viewIndex; // [esp+4h] [ebp-4h]

    iassert( (gfxCfg.entCount & 31) == 0 );
    scene.entOverflowedDrawBuf = (uint32_t *)R_AllocGlobalVariable(gfxCfg.entCount >> 3, "R_InitSceneBuffers");
    for (viewIndex = 0; viewIndex < 7; ++viewIndex)
        scene.dpvs.entVisData[viewIndex] = (uint8_t *)R_AllocGlobalVariable(gfxCfg.entCount, "R_InitSceneBuffers");
    scene.dpvs.sceneXModelIndex = (uint16_t *)R_AllocGlobalVariable(2 * gfxCfg.entCount, "R_InitSceneBuffers");
    scene.dpvs.sceneDObjIndex = (uint16_t *)R_AllocGlobalVariable(2 * gfxCfg.entCount, "R_InitSceneBuffers");
    for (localClientNum = 0; localClientNum < gfxCfg.maxClientViews; ++localClientNum)
    {
        dpvsGlob.entVisBits[localClientNum] = (uint32_t *)R_AllocGlobalVariable(
            4 * (gfxCfg.entCount >> 5),
            "R_InitSceneBuffers");
        scene.dpvs.entInfo[localClientNum] = (GfxEntCellRefInfo *)R_AllocGlobalVariable(
            sizeof(GfxEntCellRefInfo) * gfxCfg.entCount,
            "R_InitSceneBuffers");
    }
}
