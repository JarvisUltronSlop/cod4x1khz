#include <universal/q_shared.h>
#include <database/database.h>
#include <database/asset_native_stream.h>
#include <asset_bootstrap_layout.hpp>
#include <sound/snd_local.h>
#include <gfx_d3d/fxprimitives.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_gfx.h>
#include <gfx_d3d/r_water.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_sky.h>
#include <gfx_d3d/r_primarylights.h>
#include <DynEntity/DynEntity_client.h>
#include <universal/com_sndalias.h>
#include <game/g_bsp.h>
#include <database/asset_schemas.generated.h>
#include <array>
#include <vector>
#include <stdexcept>
#include <cstdio>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::abort(); } } while (0)
namespace {
std::vector<std::byte> file;
std::size_t filePosition{};
template<class T> void input(const T& value) {
    const auto* begin = reinterpret_cast<const std::byte*>(&value);
    file.assign(begin, begin + sizeof(value)); filePosition = 0;
}
template<class F> void rejects(F function) {
    bool caught{}; try { function(); } catch (const std::runtime_error&) { caught = true; } CHECK(caught);
}
}
const char* g_assetNames[ASSET_TYPE_COUNT]{};
void MyAssertHandler(const char*, int, int, const char*, ...) { throw std::runtime_error("Production loader assertion failed"); }
void DB_LoadXFileData(std::uint8_t* pointer, std::uint32_t bytes) {
    CHECK(bytes <= file.size() - filePosition);
    std::memcpy(pointer, file.data() + filePosition, bytes); filePosition += bytes;
}
uint32_t SL_GetString(const char*, uint32_t) { throw std::runtime_error("String interning is outside this stream fixture"); }

int main() {
    using namespace khz::asset_wire;
    std::array<std::byte, 44 * 3> diskTree{};
    const int32_t forwardChild = 44 * 2, backwardChild = -44 * 2;
    memcpy(diskTree.data() + 40, &forwardChild, sizeof(forwardChild));
    memcpy(diskTree.data() + 44 * 2 + 40, &backwardChild, sizeof(backwardChild));
    struct TreeGuard { std::array<uint8_t, 16> before; GfxAabbTree trees[3]; std::array<uint8_t, 16> after; } treeGuard;
    memset(&treeGuard, 0xa7, sizeof(treeGuard));
    std::vector<Field> treeFields;
    const auto treeLayout = record_GfxAabbTree.select(diskTree, treeFields);
    const auto nativeTrees = std::span<std::byte>(reinterpret_cast<std::byte*>(treeGuard.trees), sizeof(treeGuard.trees));
    decode(treeLayout, diskTree, nativeTrees, 3);
    CHECK(reinterpret_cast<const char*>(&treeGuard.trees[0]) + treeGuard.trees[0].childrenOffset == reinterpret_cast<const char*>(&treeGuard.trees[2]));
    CHECK(reinterpret_cast<const char*>(&treeGuard.trees[2]) + treeGuard.trees[2].childrenOffset == reinterpret_cast<const char*>(&treeGuard.trees[0]));
    CHECK(!treeGuard.trees[1].childrenOffset);
    for (auto byte : treeGuard.before) CHECK(byte == 0xa7);
    for (auto byte : treeGuard.after) CHECK(byte == 0xa7);
    const int32_t invalidChild = 43;
    memcpy(diskTree.data() + 40, &invalidChild, sizeof(invalidChild));
    rejects([&] { decode(treeLayout, diskTree, nativeTrees, 3); });
    if constexpr (sizeof(void*) == 8) {
        const int32_t tooFar = INT32_MAX / 44 * 44;
        memcpy(diskTree.data() + 40, &tooFar, sizeof(tooFar));
        rejects([&] { decode(treeLayout, diskTree, nativeTrees, 3); });
    }
    XZoneMemory zone{};
    std::array<std::uint32_t, 9> wireSizes; wireSizes.fill(256);
    for (unsigned block = 0; block < 9; ++block) {
        const auto capacity = block >= 7 ? 256u : 528u;
        zone.blocks[block].data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, capacity,
            MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
        CHECK(zone.blocks[block].data); zone.blocks[block].size = capacity;
        if constexpr (sizeof(void*) == 8) CHECK(reinterpret_cast<uintptr_t>(zone.blocks[block].data) > UINT32_MAX);
    }
    DB_InitStreams(&zone, wireSizes.data());
    DB_PushStreamPos(4);
    auto* tables = reinterpret_cast<StringTable*>(DB_AllocStreamPos(3));
    const std::array<StringTable32, 2> disk{{{UINT32_MAX, 3, 5, 0}, {UINT32_MAX - 1, 7, 11, 0}}};
    input(disk);
    static constexpr Schema tableSchema{"StringTable", 16, sizeof(StringTable),
        [](std::span<const std::byte>, std::vector<Field>&) { return stringTableLayout(); }};
    DB_LoadTypedStream(true, reinterpret_cast<std::uint8_t*>(tables), sizeof(disk), tableSchema);
    CHECK(filePosition == sizeof(disk));
    CHECK(tables[0].columnCount == 3 && tables[1].rowCount == 11);
    CHECK(reinterpret_cast<uintptr_t>(tables[0].name) == pointerBits(UINT32_MAX));
    CHECK(reinterpret_cast<uintptr_t>(tables[1].name) == pointerBits(UINT32_MAX - 1));
    CHECK(DB_GetStreamPos() == reinterpret_cast<std::uint8_t*>(tables + 2));
#if defined(_WIN64)
    CHECK(g_assetStreams.wirePosition() == 32);
    CHECK(g_assetRelocations.resolve(offset(4, 16 + 4)) == reinterpret_cast<std::byte*>(&tables[1].columnCount));
    const auto insertedOffset = g_assetStreams.wirePosition();
    auto inserted = DB_InsertPointer();
    *inserted = tables;
    uintptr_t alias = offset(4, insertedOffset);
    DB_ConvertOffsetToAlias(&alias); CHECK(alias == reinterpret_cast<uintptr_t>(tables));
    CHECK(g_assetStreams.wirePosition() == insertedOffset + 4);
    rejects([&] { DB_LoadTypedStream(true, DB_GetStreamPos(), 15, tableSchema); });
#endif
    DB_PopStreamPos();

    // Temporary records reuse their arena after the nested stream is popped.
    for (int iteration = 0; iteration < 3; ++iteration) {
        DB_PushStreamPos(0);
        auto* temporary = DB_AllocStreamPos(3); input(disk[0]);
        DB_LoadTypedStream(true, temporary, sizeof(StringTable32), tableSchema);
        CHECK(reinterpret_cast<StringTable*>(temporary)->rowCount == 5);
        DB_PopStreamPos(); CHECK(DB_GetStreamPos() == zone.blocks[0].data);
    }

    // Image records contain a 16-byte header followed by exactly resourceSize
    // bytes. sizeof(GfxImageLoadDef) also includes its inline tail and padding.
    GfxImageLoadDef imageHeader{}; imageHeader.resourceSize = 5;
    std::array<std::byte, 21> imageWire{};
    std::memcpy(imageWire.data(), &imageHeader, 16);
    for (unsigned i = 16; i < imageWire.size(); ++i) imageWire[i] = std::byte(i + 1);
    input(imageWire); DB_PushStreamPos(0);
    auto* image = reinterpret_cast<GfxImageLoadDef*>(DB_AllocStreamPos(3));
    DB_LoadTypedStream(true, reinterpret_cast<uint8_t*>(image), 16, record_GfxImageLoadDef);
    CHECK(image->resourceSize == 5 && filePosition == 16);
    CHECK(DB_GetStreamPos() == image->data);
    Load_Stream(true, image->data, image->resourceSize);
    CHECK(filePosition == imageWire.size() && DB_GetStreamPos() == image->data + 5);
    CHECK(std::memcmp(image->data, imageWire.data() + 16, 5) == 0);
    DB_PopStreamPos();

    // Runtime pointers reserve native slots without consuming any file bytes.
    DB_PushStreamPos(1);
    auto* runtime = reinterpret_cast<uintptr_t*>(DB_AllocStreamPos(3));
    input(std::array<std::uint32_t, 2>{13, 19});
    DB_LoadPointerStream(true, reinterpret_cast<std::uint8_t*>(runtime), 8);
    CHECK(runtime[0] == 0 && runtime[1] == 0 && filePosition == 0);
    DB_PopStreamPos();

    // Delayed streams consume the file only in the original delayed-read phase.
    DB_PushStreamPos(2);
    auto* delayed = reinterpret_cast<uintptr_t*>(DB_AllocStreamPos(3));
    DB_LoadPointerStream(true, reinterpret_cast<std::uint8_t*>(delayed), 8);
    CHECK(filePosition == 0); DB_PopStreamPos();
#if defined(_WIN64)
    uintptr_t forward = offset(2, 0);
    DB_ConvertOffsetToPointer(&forward);
#endif
    Load_DelayStream();
    CHECK(filePosition == 8 && delayed[0] == 13 && delayed[1] == 19);
#if defined(_WIN64)
    DB_FinishNativeReferences(); CHECK(forward == reinterpret_cast<uintptr_t>(delayed));
#endif

    // Cloning a loaded sound must preserve the next native pool entry. The
    // original table used a GameWorldSp sizeof alias that diverges on x64.
    XAssetPool<LoadedSound, 3> soundPool{};
    soundPool.entries[1].next = &soundPool.entries[2];
    alignas(LoadedSound) std::array<std::byte, sizeof(XAssetSize)> soundSource{};
    auto* sourceSound = reinterpret_cast<LoadedSound*>(soundSource.data());
    sourceSound->name = reinterpret_cast<const char*>(zone.blocks[3].data);
    sourceSound->sound.info.samples = 193; sourceSound->sound.info.channels = 2;
    sourceSound->sound.data = zone.blocks[5].data;
    std::memcpy(&soundPool.entries[0].entry, sourceSound, DB_GetXAssetTypeSize(ASSET_TYPE_LOADED_SOUND));
    CHECK(soundPool.entries[1].next == &soundPool.entries[2]);
    CHECK(soundPool.entries[0].entry.sound.data == zone.blocks[5].data);
    CHECK(soundPool.entries[0].entry.sound.info.samples == 193);
    CHECK(DB_GetXAssetTypeSize(ASSET_TYPE_PHYSPRESET) == sizeof(PhysPreset));
    CHECK(DB_GetXAssetTypeSize(ASSET_TYPE_SOUND) == sizeof(snd_alias_list_t));
    CHECK(DB_GetXAssetTypeSize(ASSET_TYPE_CLIPMAP) == sizeof(clipMap_t));
    CHECK(DB_GetXAssetTypeSize(ASSET_TYPE_COMWORLD) == sizeof(ComWorld));
    CHECK(DB_GetXAssetTypeSize(ASSET_TYPE_MAP_ENTS) == sizeof(MapEnts));
    CHECK(DB_GetXAssetTypeSize(ASSET_TYPE_LIGHT_DEF) == sizeof(GfxLightDef));
    CHECK(DB_GetXAssetTypeSize(ASSET_TYPE_MENULIST) == sizeof(MenuList));
    CHECK(DB_GetXAssetTypeSize(ASSET_TYPE_LOCALIZE_ENTRY) == sizeof(LocalizeEntry));

    // Actual generated physics records retain fixed retail strides and native
    // body slots; the compact effect pool retains the original handle mapping.
    struct PhysicsWire { int32_t body; uint16_t flags, lighting; int32_t health; };
    static_assert(sizeof(PhysicsWire) == 12);
    input(std::array<PhysicsWire, 2>{{{-1, 3, 17, 91}, {0, 4, 19, -73}}});
    DB_PushStreamPos(6);
    auto* clients = reinterpret_cast<DynEntityClient*>(DB_AllocStreamPos(3));
    DB_LoadTypedStream(true, reinterpret_cast<uint8_t*>(clients), 24, record_DynEntityClient);
    CHECK(clients[0].physObjId == -1 && clients[0].flags == 3 && clients[0].health == 91);
    CHECK(clients[1].physObjId == 0 && clients[1].lightingHandle == 19 && clients[1].health == -73);
    clients[0].physObjId = reinterpret_cast<intptr_t>(zone.blocks[3].data);
    CHECK(reinterpret_cast<void*>(clients[0].physObjId) == zone.blocks[3].data);
    DB_PopStreamPos();
    auto* effects = static_cast<FxPool<FxElem>*>(VirtualAlloc(nullptr, sizeof(FxPool<FxElem>) * 2048,
        MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE)); CHECK(effects);
    for (const auto index : {0, 31, 2047}) {
        auto* effect = &effects[index].item;
        const auto handle = FX_PoolToHandle_Generic<FxElem, 2048>(effects, effect);
        CHECK((FX_PoolFromHandle_Generic<FxElem, 2048>(effects, handle) == &effects[index]));
        effect->physObjId = reinterpret_cast<intptr_t>(clients);
        effect->u.lightingHandle = 109;
        CHECK(reinterpret_cast<void*>(effect->physObjId) == clients && effect->u.lightingHandle == 109);
    }
    CHECK(VirtualFree(effects, 0, MEM_RELEASE));

    // Geometry block bytes and device-buffer offsets do not expand.
    std::array<std::uint8_t, 256> vertexMirror; vertexMirror.fill(0xcc);
    zone.lockedVertexData = vertexMirror.data();
    const std::array<std::uint8_t, 13> vertices{1,2,3,5,8,13,21,34,55,89,144,233,17}; input(vertices);
    DB_PushStreamPos(7); auto* geometry = DB_AllocStreamPos(0);
    Load_Stream(true, geometry, vertices.size()); DB_PopStreamPos();
    CHECK(std::memcmp(vertexMirror.data(), vertices.data(), vertices.size()) == 0);
    CHECK(vertexMirror[vertices.size()] == 0xcc);
#if defined(_WIN64)
    CHECK(g_assetRelocations.resolve(offset(7, 12)) == reinterpret_cast<std::byte*>(geometry + 12));
    rejects([&] { DB_PopStreamPos(); });
#endif
    for (auto& block : zone.blocks) CHECK(VirtualFree(block.data, 0, MEM_RELEASE));
    std::printf("Production asset streams: %zu-bit typed arrays, aliases, temporary rewinds, delayed reads and geometry passed\n", sizeof(void*) * 8);
}
