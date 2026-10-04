#include <universal/q_shared.h>
#include "database.h"
#include <game/g_bsp.h>

//int32_t marker_db_assetnames 828ddeec     db_assetnames.obj

const char *__cdecl DB_StringTableGetName(const XAssetHeader *header);
const char *__cdecl DB_LocalizeEntryGetName(const XAssetHeader *header);
const char *__cdecl DB_ImageGetName(const XAssetHeader *header);

const char *(__cdecl *DB_XAssetGetNameHandler[33])(const XAssetHeader *) =
{
    // KISAKTODO: these got Identical COMDAT folded into 1 function because name is usually the 1st field.
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_ImageGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    0,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_LocalizeEntryGetName,
    DB_StringTableGetName,
    0,
    DB_StringTableGetName,
    DB_StringTableGetName,
    0,
    0,
    0,
    0,
    DB_StringTableGetName,
    DB_StringTableGetName
};

void __cdecl DB_StringTableSetName(XAssetHeader *header, const char *name);
void __cdecl DB_ImageSetName(XAssetHeader *header, const char *name);
void __cdecl DB_LocalizeEntrySetName(XAssetHeader *header, const char *name);

void(__cdecl *DB_XAssetSetNameHandler[33])(XAssetHeader *, const char *) =
{
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_ImageSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    0,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_LocalizeEntrySetName,
    DB_StringTableSetName,
    0,
    DB_StringTableSetName,
    DB_StringTableSetName,
    0,
    0,
    0,
    0,
    DB_StringTableSetName,
    DB_StringTableSetName
};

// KISAKTODO: make these non-fixed
// --- file-local forward declarations (moved out of database.h) ---
static void __cdecl DB_StringTableSetName(XAssetHeader *header, const char *name);
static const char*__cdecl DB_ImageGetName(const XAssetHeader *header);
static void __cdecl DB_ImageSetName(XAssetHeader *header, const char *name);
static const char *__cdecl DB_StringTableGetName(const XAssetHeader *header);
static const char *__cdecl DB_LocalizeEntryGetName(const XAssetHeader *header);
static void __cdecl DB_LocalizeEntrySetName(XAssetHeader *header, const char *name);

// Original x86 COMDAT aliases cannot be reused: each native asset's size
// depends on its own pointer fields and alignment.
template<class T>
static int32_t __cdecl DB_SizeofXAsset()
{
    return sizeof(T);
}
int(__cdecl *DB_GetXAssetSizeHandler[ASSET_TYPE_COUNT])() =
{
    DB_SizeofXAsset<XModelPieces>,
    DB_SizeofXAsset<PhysPreset>,
    DB_SizeofXAsset<XAnimParts>,
    DB_SizeofXAsset<XModel>,
    DB_SizeofXAsset<Material>,
    DB_SizeofXAsset<MaterialTechniqueSet>,
    DB_SizeofXAsset<GfxImage>,
    DB_SizeofXAsset<snd_alias_list_t>,
    DB_SizeofXAsset<SndCurve>,
    DB_SizeofXAsset<LoadedSound>,
    DB_SizeofXAsset<clipMap_t>,
    DB_SizeofXAsset<clipMap_t>,
    DB_SizeofXAsset<ComWorld>,
    DB_SizeofXAsset<GameWorldSp>,
    DB_SizeofXAsset<GameWorldMp>,
    DB_SizeofXAsset<MapEnts>,
    DB_SizeofXAsset<GfxWorld>,
    DB_SizeofXAsset<GfxLightDef>,
    nullptr,
    DB_SizeofXAsset<Font_s>,
    DB_SizeofXAsset<MenuList>,
    DB_SizeofXAsset<menuDef_t>,
    DB_SizeofXAsset<LocalizeEntry>,
    DB_SizeofXAsset<WeaponDef>,
    nullptr,
    DB_SizeofXAsset<FxEffectDef>,
    DB_SizeofXAsset<FxImpactTable>,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    DB_SizeofXAsset<RawFile>,
    DB_SizeofXAsset<StringTable>,
};

void __cdecl DB_StringTableSetName(XAssetHeader *header, const char *name)
{
    header->xmodelPieces->name = name;
}

const char *__cdecl DB_ImageGetName(const XAssetHeader *header)
{
    return header->image->name;
}

void __cdecl DB_ImageSetName(XAssetHeader *header, const char *name)
{
    //header->xmodelPieces[2].pieces = name;
    //header->xmodelPieces[2].name = name;
    header->image->name = name;
}

const char *__cdecl DB_StringTableGetName(const XAssetHeader *header)
{
    return header->stringTable->name;
}

const char *__cdecl DB_LocalizeEntryGetName(const XAssetHeader *header)
{
    return header->localize->name;
}

void __cdecl DB_LocalizeEntrySetName(XAssetHeader *header, const char *name)
{
    header->localize->name = name;
}

const char *__cdecl DB_GetXAssetHeaderName(int32_t type, const XAssetHeader *header)
{
    const char *name; // [esp+0h] [ebp-4h]

    iassert(header);
    iassert(DB_XAssetGetNameHandler[type]);
    iassert(header->data);

    name = DB_XAssetGetNameHandler[type](header);

    iassert(name);
    //if (!name)
    //{
    //    MyAssertHandler(".\\database\\db_assetnames.cpp", 594, 0, "%s\n\t%s", "name", 
    //      va("Name not found for asset type %s\n", g_assetNames[type]));
    //}
    return name;
}

const char *__cdecl DB_GetXAssetName(const XAsset *asset)
{
    iassert(asset);
    return DB_GetXAssetHeaderName(asset->type, &asset->header);
}

void __cdecl DB_SetXAssetName(XAsset *asset, const char *name)
{
    if (!DB_XAssetSetNameHandler[asset->type])
        MyAssertHandler(".\\database\\db_assetnames.cpp", 608, 0, "%s", "DB_XAssetSetNameHandler[asset->type]");
    DB_XAssetSetNameHandler[asset->type](&asset->header, name);
}

int32_t __cdecl DB_GetXAssetTypeSize(int32_t type)
{
    if (!DB_GetXAssetSizeHandler[type])
        MyAssertHandler(".\\database\\db_assetnames.cpp", 615, 0, "%s", "DB_GetXAssetSizeHandler[type]");
    return DB_GetXAssetSizeHandler[type]();
}

const char *__cdecl DB_GetXAssetTypeName(uint32_t type)
{
    if (type >= ASSET_TYPE_COUNT)
        MyAssertHandler(".\\database\\db_assetnames.cpp", 621, 0, "%s", "type >= 0 && type < ASSET_TYPE_COUNT");
    return g_assetNames[type];
}

