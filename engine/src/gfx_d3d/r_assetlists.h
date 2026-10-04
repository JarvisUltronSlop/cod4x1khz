#pragma once
#include <xanim/xanim.h>
#include "r_material.h"

struct RModelList
{
    int count;
    XModel *sorted[2050];
};

struct RMaterialList
{
    uint32_t count;
    MaterialMemory sorted[2049];
};

void __cdecl R_GetModelList(XAssetHeader header, void *data);
void __cdecl R_GetMaterialList(XAssetHeader header, void *data);
