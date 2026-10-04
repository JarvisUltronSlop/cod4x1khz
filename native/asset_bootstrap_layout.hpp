#pragma once
#include <database/asset_bootstrap.h>
#include <asset_wire.hpp>

namespace khz::asset_wire {
inline Layout stringTableLayout() {
    static constexpr Field fields[] = {
        {0, offsetof(StringTable, name), 4, FieldKind::Pointer32},
        {4, offsetof(StringTable, columnCount), 8},
        {12, offsetof(StringTable, values), 4, FieldKind::Pointer32}
    };
    return {16, sizeof(StringTable), fields};
}
inline Layout scriptStringsLayout() {
    static constexpr Field fields[] = {
        {0, offsetof(ScriptStringList, count), 4},
        {4, offsetof(ScriptStringList, strings), 4, FieldKind::Pointer32}
    };
    return {8, sizeof(ScriptStringList), fields};
}
inline Layout assetListLayout() {
    static constexpr Field fields[] = {
        {0, offsetof(XAssetList, stringList) + offsetof(ScriptStringList, count), 4},
        {4, offsetof(XAssetList, stringList) + offsetof(ScriptStringList, strings), 4, FieldKind::Pointer32},
        {8, offsetof(XAssetList, assetCount), 4},
        {12, offsetof(XAssetList, assets), 4, FieldKind::Pointer32}
    };
    return {16, sizeof(XAssetList), fields};
}
inline Layout pointerLayout() {
    static constexpr Field field{0, 0, 4, FieldKind::Pointer32};
    return {4, sizeof(void*), {&field, 1}};
}
template<class NativeAsset> Layout assetLayout() {
    static constexpr Field fields[] = {
        {0, offsetof(NativeAsset, type), 4},
        {4, offsetof(NativeAsset, header), 4, FieldKind::Pointer32}
    };
    return {8, sizeof(NativeAsset), fields};
}
}
