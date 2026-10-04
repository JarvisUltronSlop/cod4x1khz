#include <universal/q_shared.h>
#include "r_assetlists.h"
#include <universal/assertive.h>

void __cdecl R_GetModelList(XAssetHeader header, void *data)
{
    auto *list = static_cast<RModelList *>(data);
    iassert(list->count >= 0 && list->count < ARRAY_COUNT(list->sorted));
    if (list->count < 0 || list->count >= ARRAY_COUNT(list->sorted))
        return;
    list->sorted[list->count++] = header.model;
}

void __cdecl R_GetMaterialList(XAssetHeader header, void *data)
{
    const int memory = R_GetMaterialMemory(header.material);
    if (!memory)
        return;
    auto *list = static_cast<RMaterialList *>(data);
    iassert(list->count < ARRAY_COUNT(list->sorted));
    if (list->count >= ARRAY_COUNT(list->sorted))
        return;
    list->sorted[list->count++] = {header.material, memory};
}
