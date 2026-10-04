#include <universal/q_shared.h>
#include "r_staticmodelcache.h"
#include "r_init.h"

static_model_cache_t s_cache;

static_model_leaf_t *SMC_GetLeaf(uint32_t cacheIndex)
{
    iassert(cacheIndex);
    static_model_leaf_t *retval = &s_cache.leafs[(cacheIndex - 1) / 32][(cacheIndex - 1) % 32];
    iassert((char *)retval == ((char *)&s_cache.leafs + (sizeof(static_model_leaf_t) * (cacheIndex - 1))));
    return retval;
}

char __cdecl SMC_ForceFreeBlock(uint32_t smcIndex)
{
    static_model_leaf_t *leafs; // [esp+8h] [ebp-8h]
    static_model_tree_t *treenode; // [esp+Ch] [ebp-4h]

    treenode = (static_model_tree_t *)s_cache.usedlist[smcIndex].prev;
    iassert( (uintptr_t)treenode != (uintptr_t)&s_cache.usedlist[smcIndex] );
    if (rg.frontEndFrameCount - treenode->frameCount < 4)
        return 0;
    if (treenode->nodes[0].usedVerts <= 0)
        MyAssertHandler(
            ".\\r_staticmodelcache.cpp",
            208,
            0,
            "%s\n\t(tree->nodes[0].usedVerts) = %i",
            "(tree->nodes[0].usedVerts > 0)",
            treenode->nodes[0].usedVerts);
    leafs = s_cache.leafs[((char *)treenode - (char *)&s_cache) / sizeof(static_model_tree_t)];
    SMC_FreeCachedSurface_r(treenode, leafs, 0, 5);
    treenode->usedlist.next->prev = treenode->usedlist.prev;
    treenode->usedlist.prev->next = treenode->usedlist.next;
    leafs->freenode.prev = s_cache.freelist[smcIndex];
    leafs->freenode.next = s_cache.freelist[smcIndex][0].next;
    leafs->freenode.prev->next = (static_model_node_list_t *)leafs;
    leafs->freenode.next->prev = (static_model_node_list_t *)leafs;
    return 1;
}

char __cdecl SMC_GetFreeBlockOfSize(uint32_t smcIndex, uint32_t listIndex)
{
    static_model_node_list_t *block; // [esp+Ch] [ebp-1Ch]
    static_model_node_list_t *blocka; // [esp+Ch] [ebp-1Ch]
    static_model_tree_t *tree; // [esp+10h] [ebp-18h]
    static_model_leaf_t *leafs; // [esp+14h] [ebp-14h]
    static_model_node_list_t *freelist; // [esp+18h] [ebp-10h]
    uint32_t index; // [esp+1Ch] [ebp-Ch]
    uint32_t treeIndex; // [esp+24h] [ebp-4h]

    if (listIndex >= 6)
        MyAssertHandler(
            ".\\r_staticmodelcache.cpp",
            233,
            0,
            "listIndex doesn't index ARRAY_COUNT( s_cache.freelist[smcIndex] )\n\t%i not in [0, %i)",
            listIndex,
            6);
    if (s_cache.freelist[smcIndex][listIndex].next != &s_cache.freelist[smcIndex][listIndex])
        MyAssertHandler(
            ".\\r_staticmodelcache.cpp",
            234,
            0,
            "%s",
            "s_cache.freelist[smcIndex][listIndex].next == &s_cache.freelist[smcIndex][listIndex]");
    if (s_cache.freelist[smcIndex][listIndex].prev != &s_cache.freelist[smcIndex][listIndex])
        MyAssertHandler(
            ".\\r_staticmodelcache.cpp",
            235,
            0,
            "%s",
            "s_cache.freelist[smcIndex][listIndex].prev == &s_cache.freelist[smcIndex][listIndex]");
    if (!listIndex)
        return SMC_ForceFreeBlock(smcIndex);
    freelist = &s_cache.freelist[smcIndex][listIndex - 1];
    if (freelist->next == freelist && !SMC_GetFreeBlockOfSize(smcIndex, listIndex - 1))
        return 0;
    block = freelist->next;
    iassert( (uintptr_t)block != (uintptr_t)freelist );
    block->next->prev = block->prev;
    block->prev->next = block->next;
    treeIndex = ((char*)block - (char*)s_cache.leafs) / sizeof(s_cache.leafs[0]);
    bcassert(treeIndex, ARRAY_COUNT(s_cache.trees));
    tree = &s_cache.trees[treeIndex];
    if (listIndex == 1)
    {
        tree->usedlist.prev = &s_cache.usedlist[smcIndex];
        tree->usedlist.next = s_cache.usedlist[smcIndex].next;
        tree->usedlist.prev->next = &tree->usedlist;
        tree->usedlist.next->prev = &tree->usedlist;
    }
    leafs = s_cache.leafs[treeIndex];
    index = ((char*)block - (char*)leafs) / sizeof(static_model_leaf_t);
    bcassert(index, ARRAY_COUNT(s_cache.leafs[treeIndex]));
    if (block != (static_model_node_list_t *)&leafs[index])
        MyAssertHandler(
            ".\\r_staticmodelcache.cpp",
            265,
            0,
            "%s\n\t(index) = %i",
            "(block == &leafs[index].freenode)",
            index);
    block->prev = &s_cache.freelist[smcIndex][listIndex];
    block->next = s_cache.freelist[smcIndex][listIndex].next;
    block->prev->next = block;
    block->next->prev = block;
    blocka = (static_model_node_list_t *)&leafs[index + (1 << (5 - listIndex))];
    blocka->prev = &s_cache.freelist[smcIndex][listIndex];
    blocka->next = s_cache.freelist[smcIndex][listIndex].next;
    blocka->prev->next = blocka;
    blocka->next->prev = blocka;
    return 1;
}

uint16_t __cdecl SMC_Allocate(uint32_t smcIndex, uint32_t bitCount)
{
    static_model_node_list_t *block; // [esp+8h] [ebp-2Ch]
    uint32_t listIndex; // [esp+Ch] [ebp-28h]
    static_model_tree_t *tree; // [esp+10h] [ebp-24h]
    uint32_t nodeIndex; // [esp+14h] [ebp-20h]
    static_model_leaf_t *leafs; // [esp+1Ch] [ebp-18h]
    static_model_node_list_t *freelist; // [esp+20h] [ebp-14h]
    uint32_t index; // [esp+24h] [ebp-10h]
    uint32_t treeIndex; // [esp+30h] [ebp-4h]

    iassert(bitCount >= 4 && bitCount <= 9);

    listIndex = 9 - bitCount;
    freelist = &s_cache.freelist[smcIndex][9 - bitCount];
    if (freelist->next == freelist && !SMC_GetFreeBlockOfSize(smcIndex, listIndex))
        return 0;
    block = freelist->next;
    iassert(block->next->prev == block);
    iassert(block->prev->next == block);
    block->next->prev = block->prev;
    block->prev->next = block->next;
    static_assert(sizeof(s_cache.leafs[0]) == (sizeof(void*) == 8 ? 512 : 256));
    treeIndex = ((char *)block - (char *)s_cache.leafs) / sizeof(s_cache.leafs[0]);
    bcassert(treeIndex, ARRAY_COUNT(s_cache.trees));
    tree = &s_cache.trees[treeIndex];
    if (!listIndex)
    {
        tree->usedlist.prev = &s_cache.usedlist[smcIndex];
        tree->usedlist.next = s_cache.usedlist[smcIndex].next;
        tree->usedlist.prev->next = &tree->usedlist;
        tree->usedlist.next->prev = &tree->usedlist;
    }
    leafs = s_cache.leafs[treeIndex];
    index = ((char *)block - (char *)leafs) / sizeof(static_model_leaf_t);
    bcassert(index, ARRAY_COUNT(s_cache.leafs[treeIndex]));
    iassert(block == &leafs[index].freenode);
    nodeIndex = ((index + 32) >> (5 - listIndex)) - 1;
    bcassert(nodeIndex, ARRAY_COUNT(tree->nodes));
    tree->nodes[nodeIndex].inuse = 1;

    while (1)
    {
        tree->nodes[nodeIndex].usedVerts += 1 << bitCount;
        if (!nodeIndex)
            break;
        nodeIndex = (nodeIndex - 1) >> 1;
    }

    unsigned short cacheIndex = (32 * treeIndex + index + 1);
    iassert(cacheIndex);
    iassert(&leafs[index] == SMC_GetLeaf(cacheIndex));

    leafs[index].cachedSurf.baseVertIndex = 16 * index + (treeIndex << 9);
    return cacheIndex;
}

void __cdecl SMC_FreeCachedSurface_r(
    static_model_tree_t *tree,
    static_model_leaf_t *leafs,
    int nodeIndex,
    int levelsToLeaf)
{
    GfxCachedSModelSurf *cachedSurf; // [esp+0h] [ebp-Ch]
    static_model_leaf_t *freenode; // [esp+8h] [ebp-4h]

    if (tree->nodes[nodeIndex].usedVerts)
    {
        tree->nodes[nodeIndex].usedVerts = 0;
        if (tree->nodes[nodeIndex].inuse)
        {
            cachedSurf = &leafs[((nodeIndex + 1) << levelsToLeaf) - 32].cachedSurf;
            if (cachedSurf->smodelIndex != 0xFFFF)
            {
                rgp.world->dpvs.smodelDrawInsts[cachedSurf->smodelIndex].smodelCacheIndex[cachedSurf->lodIndex] = 0;
                cachedSurf->smodelIndex = -1;
            }
            tree->nodes[nodeIndex].inuse = 0;
        }
        else
        {
            SMC_FreeCachedSurface_r(tree, leafs, 2 * nodeIndex + 1, levelsToLeaf - 1);
            SMC_FreeCachedSurface_r(tree, leafs, 2 * nodeIndex + 2, levelsToLeaf - 1);
        }
    }
    else
    {
        freenode = &leafs[((nodeIndex + 1) << levelsToLeaf) - 32];
        //freenode->freenode.next->prev = (static_model_node_list_t *)freenode->cachedSurf.baseVertIndex;
        freenode->freenode.next->prev = freenode->freenode.prev;
        freenode->freenode.prev->next = freenode->freenode.next;
        //*(_DWORD *)(freenode->cachedSurf.baseVertIndex + 4) = (uint32_t)freenode->freenode.next;
    }
}

void SMC_ClearCache()
{
    static_model_leaf_t *v0; // [esp+4h] [ebp-14h]
    uint32_t treeIter; // [esp+8h] [ebp-10h]
    uint32_t treeItera; // [esp+8h] [ebp-10h]
    uint32_t leafIter; // [esp+Ch] [ebp-Ch]
    uint32_t listIter; // [esp+10h] [ebp-8h]
    uint32_t smcIter; // [esp+14h] [ebp-4h]

    for (treeIter = 0; treeIter < 0x200; ++treeIter)
    {
        for (leafIter = 0; leafIter < 0x20; ++leafIter)
            s_cache.leafs[treeIter][leafIter].cachedSurf.smodelIndex = -1;
    }
    for (smcIter = 0; smcIter < 4; ++smcIter)
    {
        s_cache.usedlist[smcIter].prev = &s_cache.usedlist[smcIter];
        s_cache.usedlist[smcIter].next = &s_cache.usedlist[smcIter];
        for (listIter = 0; listIter < 6; ++listIter)
        {
            s_cache.freelist[smcIter][listIter].prev = &s_cache.freelist[smcIter][listIter];
            s_cache.freelist[smcIter][listIter].next = &s_cache.freelist[smcIter][listIter];
        }
        for (treeItera = 0; treeItera < 0x80; ++treeItera)
        {
            v0 = s_cache.leafs[treeItera + (smcIter << 7)];
            v0->freenode.prev = s_cache.freelist[smcIter];
            v0->freenode.next = s_cache.freelist[smcIter][0].next;
            v0->freenode.prev->next = (static_model_node_list_t *)v0;
            v0->freenode.next->prev = (static_model_node_list_t *)v0;
        }
    }
}

void __cdecl R_FlushStaticModelCache()
{
    static_model_tree_list_t *next; // [esp+4h] [ebp-10h]
    static_model_tree_t *tree; // [esp+8h] [ebp-Ch]
    uint32_t smcIter; // [esp+Ch] [ebp-8h]
    static_model_leaf_t *leafs; // [esp+10h] [ebp-4h]

    if (s_cache.usedlist[0].next)
    {
        for (smcIter = 0; smcIter < 4; ++smcIter)
        {
            while (s_cache.usedlist[smcIter].next != &s_cache.usedlist[smcIter])
            {
                iassert(s_cache.usedlist[smcIter].next);
                tree = (static_model_tree_t *)s_cache.usedlist[smcIter].next;
                leafs = s_cache.leafs[tree - s_cache.trees];
                SMC_FreeCachedSurface_r(tree, leafs, 0, 5);
                next = s_cache.usedlist[smcIter].next;
                iassert(next->next->prev == next);
                iassert(next->prev->next == next);
                next->next->prev = next->prev;
                next->prev->next = next->next;
                leafs->freenode.prev = s_cache.freelist[smcIter];
                leafs->freenode.next = s_cache.freelist[smcIter][0].next;
                leafs->freenode.prev->next = &leafs->freenode;
                leafs->freenode.next->prev = &leafs->freenode;
            }
        }
        SMC_ClearCache();
    }
}
