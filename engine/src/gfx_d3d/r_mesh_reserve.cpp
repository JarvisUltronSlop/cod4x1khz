#include <universal/q_shared.h>
#include "r_meshdata.h"

char __cdecl R_ReserveMeshIndices(GfxMeshData *mesh, int indexCount, r_double_index_t **indicesOut)
{
    uint32_t usedCodeMeshIndexCount; // [esp+0h] [ebp-4h]

    iassert( (indexCount >= 0) );
    iassert( (!(indexCount & 1)) );
    iassert( indicesOut );
    iassert( mesh->indexCount <= mesh->totalIndexCount );
    usedCodeMeshIndexCount = mesh->indexCount;
    if (indexCount < 0 || mesh->indexCount > mesh->totalIndexCount ||
        static_cast<uint32_t>(indexCount) > mesh->totalIndexCount - mesh->indexCount)
        return 0;
    mesh->indexCount = indexCount + usedCodeMeshIndexCount;
    *indicesOut = (r_double_index_t *)&mesh->indices[usedCodeMeshIndexCount];
    if (((uint32_t)*indicesOut & 3) != 0)
        MyAssertHandler(
            ".\\r_meshdata.cpp",
            67,
            0,
            "%s\n\t((uint)(*indicesOut)) = %i",
            "(!((uint)(*indicesOut) & 3))",
            *indicesOut);
    return 1;
}

char __cdecl R_ReserveMeshVerts(GfxMeshData *mesh, int vertCount, uint16_t *baseVertex)
{
    volatile uint32_t usedCodeMeshVertBytes; // [esp+8h] [ebp-8h]

    iassert( (vertCount >= 0) );
    iassert( baseVertex );
    usedCodeMeshVertBytes = mesh->vb.used;
    // Failed reservations must not advance the cursor or wrap byte arithmetic.
    // Decals may span hundreds of clipped stair/prop triangles in one impact.
    if (vertCount < 0 || !mesh->vertSize || usedCodeMeshVertBytes > mesh->vb.total ||
        static_cast<uint32_t>(vertCount) > (mesh->vb.total - usedCodeMeshVertBytes) / mesh->vertSize ||
        usedCodeMeshVertBytes / mesh->vertSize > UINT16_MAX ||
        static_cast<uint32_t>(vertCount) > UINT16_MAX + 1u - usedCodeMeshVertBytes / mesh->vertSize)
        return 0;
    *baseVertex = usedCodeMeshVertBytes / mesh->vertSize;
    mesh->vb.used = mesh->vertSize * vertCount + usedCodeMeshVertBytes;
    return 1;
}
