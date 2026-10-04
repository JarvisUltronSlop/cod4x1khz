#include <asset_bootstrap_layout.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::abort(); } } while (0)
using namespace khz::asset_wire;
template<class F> void rejects(F function) {
    bool caught{}; try { function(); } catch (const std::runtime_error&) { caught = true; }
    CHECK(caught);
}
template<class T> std::span<std::byte> bytes(T& value) {
    return {reinterpret_cast<std::byte*>(&value), sizeof(value)};
}
template<class T> std::span<const std::byte> input(const T& value) {
    return {reinterpret_cast<const std::byte*>(&value), sizeof(value)};
}
struct NativeAsset { std::uint32_t type; void* header; };

int main() {
    CHECK(sizeof(void*) == 4 || sizeof(void*) == 8);
    StringTable32 disk{UINT32_MAX, 7, 11, UINT32_MAX - 1};
    StringTable native{};
    decode(stringTableLayout(), input(disk), bytes(native));
    CHECK(reinterpret_cast<std::uintptr_t>(native.name) == pointerBits(UINT32_MAX));
    CHECK(reinterpret_cast<std::uintptr_t>(native.values) == pointerBits(UINT32_MAX - 1));
    CHECK(native.columnCount == 7 && native.rowCount == 11);
    rejects([&] { decode(stringTableLayout(), input(disk).first(15), bytes(native)); });
    rejects([&] { decode(stringTableLayout(), input(disk), bytes(native).first(sizeof(native) - 1)); });
    rejects([&] { decode(stringTableLayout(), input(native).first(16), bytes(native)); });
    const Field overlap[] = {{0, 0, 8}, {4, 8, 4}};
    rejects([&] { validate({16, 24, overlap}); });

    const XAssetList32 listDisk{{3, UINT32_MAX}, 5, UINT32_MAX - 1};
    XAssetList list{};
    decode(assetListLayout(), input(listDisk), bytes(list));
    CHECK(list.stringList.count == 3 && list.assetCount == 5);
    CHECK(reinterpret_cast<std::uintptr_t>(list.stringList.strings) == pointerBits(UINT32_MAX));
    CHECK(reinterpret_cast<std::uintptr_t>(list.assets) == pointerBits(UINT32_MAX - 1));
    const std::array<std::uint32_t, 4> assetDisk{2, UINT32_MAX, 17, offset(8, 13)};
    std::array<NativeAsset, 2> assets{};
    decode(assetLayout<NativeAsset>(), input(assetDisk), bytes(assets), 2);
    CHECK(assets[0].type == 2 && reinterpret_cast<std::uintptr_t>(assets[0].header) == pointerBits(UINT32_MAX));
    CHECK(assets[1].type == 17 && reinterpret_cast<std::uintptr_t>(assets[1].header) == offset(8, 13));

    Relocations map;
    std::array<std::uint32_t, 9> sizes; sizes.fill(256); map.reset(sizes);
    std::array<std::array<std::byte, 32>, 9> storage{};
    for (unsigned block = 0; block < 9; ++block) {
        map.addBytes(block, 19, storage[block]);
        for (unsigned at = 0; at < storage[block].size(); ++at)
            CHECK(map.resolve(offset(block, 19 + at)) == storage[block].data() + at);
    }
    std::array<StringTable32, 2> tableDisk{{{offset(8, 19), 3, 2, 0}, {offset(7, 20), 5, 4, 0}}};
    std::array<StringTable, 2> tables{};
    decode(stringTableLayout(), input(tableDisk), bytes(tables), 2);
    map.add(4, 64, stringTableLayout(), bytes(tables), 2);
    CHECK(map.resolve(offset(4, 64)) == bytes(tables).data());
    CHECK(map.resolve(offset(4, 64 + 16)) == bytes(tables[1]).data());
    CHECK(map.resolve(offset(4, 64 + 16 + 4)) == reinterpret_cast<std::byte*>(&tables[1].columnCount));
    tables[0].name = reinterpret_cast<const char*>(map.resolve(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(tables[0].name))));
    tables[1].name = reinterpret_cast<const char*>(map.resolve(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(tables[1].name))));
    CHECK(tables[0].name == reinterpret_cast<const char*>(storage[8].data()));
    CHECK(tables[1].name == reinterpret_cast<const char*>(storage[7].data() + 1));
    CHECK(map.alias(offset(4, 64)) == reinterpret_cast<std::uintptr_t>(tables[0].name));
    CHECK(map.alias(offset(4, 80)) == reinterpret_cast<std::uintptr_t>(tables[1].name));
    if constexpr (sizeof(void*) == 8) CHECK(map.alias(offset(4, 64)) > UINT32_MAX);
    rejects([&] { map.alias(offset(4, 68)); });
    rejects([&] { map.alias(offset(8, 19)); });
    rejects([&] { map.resolve(offset(4, 65)); });
    rejects([&] { map.resolve(offset(4, 128)); });
    rejects([&] { map.resolve(offset(4, 256)); });
    rejects([&] { map.resolve(UINT32_MAX); });
    rejects([&] { map.resolve(UINT32_MAX - 1); });
    rejects([&] { offset(9, 0); });
    rejects([&] { offset(0, 0x0fffffff); });
    rejects([&] { map.addBytes(4, 65, storage[0]); });
    CHECK(map.resolve(0) == nullptr && map.alias(0) == 0);
    map.reset(sizes);
    rejects([&] { map.resolve(offset(4, 64)); });
    std::printf("Asset wire conversion and all nine zone blocks: %zu-bit pointers, sentinels, array strides, native aliases and malformed ranges passed\n", sizeof(void*) * 8);
}
