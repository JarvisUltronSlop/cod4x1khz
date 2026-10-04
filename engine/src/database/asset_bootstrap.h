#pragma once
#include <cstddef>
#include <cstdint>

// Native objects. Disk layouts below never contain compiler-sized pointers.
struct XAsset;
struct StringTable {
    const char* name;
    int columnCount;
    int rowCount;
    const char** values;
};
struct ScriptStringList { int count; const char** strings; };
struct XAssetList { ScriptStringList stringList; int assetCount; XAsset* assets; };
static_assert(sizeof(StringTable) == (sizeof(void*) == 8 ? 24 : 16));
static_assert(sizeof(ScriptStringList) == (sizeof(void*) == 8 ? 16 : 8));
static_assert(sizeof(XAssetList) == (sizeof(void*) == 8 ? 32 : 16));

namespace khz::asset_wire {
struct StringTable32 { std::uint32_t name; std::int32_t columnCount, rowCount; std::uint32_t values; };
struct ScriptStringList32 { std::int32_t count; std::uint32_t strings; };
struct XAssetList32 { ScriptStringList32 stringList; std::int32_t assetCount; std::uint32_t assets; };
static_assert(sizeof(StringTable32) == 16 && offsetof(StringTable32, values) == 12);
static_assert(sizeof(ScriptStringList32) == 8 && offsetof(ScriptStringList32, strings) == 4);
static_assert(sizeof(XAssetList32) == 16 && offsetof(XAssetList32, assets) == 12);
}
