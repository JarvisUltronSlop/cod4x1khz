#include <universal/q_shared.h>
#include "DynEntity_client.h"

extern const DynEntityProps dynEntProps[3];

int32_t __cdecl DynEnt_CompareEntities(const void *left, const void *right)
{
    const auto *a = static_cast<const DynEntityDef *>(left);
    const auto *b = static_cast<const DynEntityDef *>(right);
    iassert(a && b);
    iassert(a->type >= DYNENT_TYPE_INVALID && a->type < DYNENT_TYPE_COUNT);
    iassert(b->type >= DYNENT_TYPE_INVALID && b->type < DYNENT_TYPE_COUNT);
    const int hasModelA = a->xModel != nullptr;
    const int hasModelB = b->xModel != nullptr;
    if (hasModelA != hasModelB)
        return hasModelA - hasModelB;
    const int clientOnlyA = dynEntProps[a->type].clientOnly;
    const int clientOnlyB = dynEntProps[b->type].clientOnly;
    if (clientOnlyA != clientOnlyB)
        return clientOnlyA - clientOnlyB;
    if (hasModelA)
        return reinterpret_cast<uintptr_t>(a->xModel) < reinterpret_cast<uintptr_t>(b->xModel) ? -1 : 1;
    return a->brushModel - b->brushModel;
}

void DynEnt_SaveEntities(MemoryFile *memFile)
{
    iassert(memFile);
    for (int drawType = 0; drawType < DYNENT_DRAW_COUNT; ++drawType)
    {
        const auto count = cm.dynEntCount[drawType];
        MemFile_WriteData(memFile, sizeof(count), &count);
        if (!count)
            continue;
        MemFile_WriteData(memFile, sizeof(DynEntityPose) * count, cm.dynEntPoseList[drawType]);
        MemFile_WriteData(memFile, sizeof(DynEntityClient) * count, cm.dynEntClientList[drawType]);
        for (uint16_t id = 0; id < count; ++id)
        {
            const auto physObjId = cm.dynEntClientList[drawType][id].physObjId;
            const bool hasPhys = physObjId != 0;
            MemFile_WriteData(memFile, sizeof(hasPhys), &hasPhys);
            if (hasPhys)
                Phys_ObjSave(reinterpret_cast<dxBody *>(physObjId), memFile);
        }
    }
}
