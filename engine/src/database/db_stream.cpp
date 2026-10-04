#include <universal/q_shared.h>
#include "database.h"
#if defined(_WIN64)
#include "asset_native_stream.h"
#include <asset_bootstrap_layout.hpp>
khz::asset_wire::Streams g_assetStreams;
khz::asset_wire::Relocations g_assetRelocations;
#endif

uint32_t g_streamDelayIndex;
XBlock * g_streamBlocks;
uint8_t *g_streamPosArray[9];
StreamDelayInfo g_streamDelayArray[4096];
uint32_t g_streamPosIndex;
XZoneMemory *g_streamZoneMem;
uint8_t *g_streamPos;

StreamPosInfo g_streamPosStack[64];
uint32_t g_streamPosStackIndex;

// --- file-local forward declarations (moved out of database.h) ---
static void __cdecl DB_SetStreamIndex(uint32_t index);
void __cdecl DB_CloneStreamData(uint8_t *destStart);

void __cdecl DB_InitStreams(XZoneMemory *zoneMem, const uint32_t *wireBlockSizes)
{
    int32_t i; // [esp+0h] [ebp-4h]

    g_streamZoneMem = zoneMem;
    g_streamPos = zoneMem->blocks[0].data;
    g_streamPosIndex = 0;
    g_streamDelayIndex = 0;
    g_streamPosStackIndex = 0;
    for (i = 0; i < 9; ++i)
        g_streamPosArray[i] = zoneMem->blocks[i].data;
#if defined(_WIN64)
    std::array<std::span<std::byte>, 9> native;
    for (unsigned block = 0; block < 9; ++block)
        native[block] = {reinterpret_cast<std::byte*>(zoneMem->blocks[block].data), zoneMem->blocks[block].size};
    const std::span<const uint32_t, 9> sizes{wireBlockSizes, 9};
    g_assetStreams.reset(sizes, native);
    g_assetRelocations.reset(sizes);
    DB_ResetNativeLoads();
#endif
}

void __cdecl DB_PushStreamPos(uint32_t index)
{
#if defined(_WIN64)
    if (index >= ARRAY_COUNT(g_streamPosArray) || g_streamPosStackIndex >= ARRAY_COUNT(g_streamPosStack))
        throw std::runtime_error("Native asset stream stack overflow");
#endif
    iassert(index < ARRAY_COUNT(g_streamPosArray));
    iassert(g_streamPosIndex < ARRAY_COUNT(g_streamPosArray));
    iassert(g_streamPosStackIndex < ARRAY_COUNT(g_streamPosStack));

    g_streamPosStack[g_streamPosStackIndex].index = g_streamPosIndex;
#if defined(_WIN64)
    if (index != g_streamPosIndex)
    {
        if (g_streamPosIndex == 7) DB_CloneStreamData(g_streamZoneMem->lockedVertexData);
        if (g_streamPosIndex == 8) DB_CloneStreamData(g_streamZoneMem->lockedIndexData);
        g_streamPosArray[g_streamPosIndex] = g_streamPos;
    }
    g_assetStreams.push(index);
    g_streamPosIndex = index;
    g_streamPos = reinterpret_cast<uint8_t*>(g_assetStreams.position());
#else
    DB_SetStreamIndex(index);
#endif

    g_streamPosStack[g_streamPosStackIndex++].pos = g_streamPos;
}

void __cdecl DB_CloneStreamData(uint8_t *destStart)
{
    if (destStart)
        memcpy(
            &destStart[g_streamPosArray[g_streamPosIndex] - g_streamZoneMem->blocks[g_streamPosIndex].data],
            g_streamPosArray[g_streamPosIndex],
            g_streamPos - g_streamPosArray[g_streamPosIndex]);
}

void __cdecl DB_SetStreamIndex(uint32_t index)
{
    if (index != g_streamPosIndex)
    {
        if (g_streamPosIndex == 7)
        {
            DB_CloneStreamData(g_streamZoneMem->lockedVertexData);
        }
        else if (g_streamPosIndex == 8)
        {
            DB_CloneStreamData(g_streamZoneMem->lockedIndexData);
        }
        iassert(index < arr_cnt(g_streamPosArray));
        g_streamPosArray[g_streamPosIndex] = g_streamPos;
        g_streamPosIndex = index;
        g_streamPos = g_streamPosArray[index];
    }
}

void __cdecl DB_PopStreamPos()
{
#if defined(_WIN64)
    if (!g_streamPosStackIndex) throw std::runtime_error("Native asset stream stack underflow");
#endif
    vassert(g_streamPosStackIndex > 0, "(g_streamPosStackIndex = %d)", g_streamPosStackIndex);
    --g_streamPosStackIndex;
#if defined(_WIN64)
    if (g_streamPosIndex == 0)
        DB_CheckNativeTemporaryReferences(g_streamPosStack[g_streamPosStackIndex].pos, g_streamPos);
    if (g_streamPosIndex == 7) DB_CloneStreamData(g_streamZoneMem->lockedVertexData);
    if (g_streamPosIndex == 8) DB_CloneStreamData(g_streamZoneMem->lockedIndexData);
    g_streamPosArray[g_streamPosIndex] = g_streamPosIndex == 0 ?
        g_streamPosStack[g_streamPosStackIndex].pos : g_streamPos;
    g_assetStreams.pop(g_assetRelocations);
    g_streamPosIndex = g_assetStreams.index();
    g_streamPos = reinterpret_cast<uint8_t*>(g_assetStreams.position());
#else
    if (!g_streamPosIndex)
        g_streamPos = g_streamPosStack[g_streamPosStackIndex].pos;
    DB_SetStreamIndex(g_streamPosStack[g_streamPosStackIndex].index);
#endif
}

uint8_t *__cdecl DB_GetStreamPos()
{
    return g_streamPos;
}

uint8_t *__cdecl DB_AllocStreamPos(int32_t alignment)
{
    iassert(g_streamPos);
#if defined(_WIN64)
    g_assetStreams.align(static_cast<unsigned>(alignment));
    g_streamPos = reinterpret_cast<uint8_t*>(g_assetStreams.position());
#else
    g_streamPos = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(g_streamPos) + alignment) & ~uintptr_t(alignment));
#endif
    return g_streamPos;
}

void __cdecl DB_IncStreamPos(int32_t size)
{
    iassert(g_streamPos);
    iassert(g_streamPos + size <= g_streamZoneMem->blocks[g_streamPosIndex].data + g_streamZoneMem->blocks[g_streamPosIndex].size);

#if defined(_WIN64)
    DB_AdvanceNativeStream(size, size);
#else
    g_streamPos += size;
#endif
}

const void **__cdecl DB_InsertPointer()
{
    const void **pData; // [esp+0h] [ebp-4h]

    DB_PushStreamPos(4);
    pData = (const void **)DB_AllocStreamPos(3);
#if defined(_WIN64)
    g_assetStreams.check(4, sizeof(void*));
    *pData = nullptr;
    g_assetRelocations.add(4, g_assetStreams.wirePosition(), khz::asset_wire::pointerLayout(),
        {reinterpret_cast<std::byte*>(pData), sizeof(void*)});
    DB_AdvanceNativeStream(4, sizeof(void*));
#else
    DB_IncStreamPos(4);
#endif
    DB_PopStreamPos();
    return pData;
}

#if defined(_WIN64)
void DB_AdvanceNativeStream(std::uint32_t wireBytes, std::uint32_t nativeBytes)
{
    g_assetStreams.advance(wireBytes, nativeBytes);
    g_streamPos = reinterpret_cast<uint8_t*>(g_assetStreams.position());
}
void DB_RegisterNativeBytes(std::uint8_t* pointer, std::uint32_t bytes)
{
    g_assetRelocations.addBytes(g_streamPosIndex, g_assetStreams.wirePosition(),
        {reinterpret_cast<std::byte*>(pointer), bytes});
}
#endif

