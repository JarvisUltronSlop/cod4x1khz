#include <game_mp/player_use_list.h>
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); std::abort(); } } while (0)
int main() {
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(sizeof(void*) == 8 ? reinterpret_cast<void*>(UINT64_C(0x100010000)) : nullptr,
        65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(memory);
    if constexpr (sizeof(void*) == 8) CHECK(reinterpret_cast<uintptr_t>(memory) > UINT32_MAX);
    struct Guarded { std::array<unsigned char, 16> before; std::array<useList_t, 128> entries; std::array<unsigned char, 16> after; } data;
    std::memset(&data, 0xa5, sizeof(data));
    for (unsigned index = 0; index < data.entries.size(); ++index) {
        unsigned score = (index * 73) % 128;
        data.entries[index] = {reinterpret_cast<gentity_s*>(memory + score * 256), static_cast<float>(score)};
    }
    Player_SortUseList(data.entries.data(), data.entries.size());
    for (unsigned index = 0; index < data.entries.size(); ++index) {
        CHECK(data.entries[index].score == index);
        CHECK(data.entries[index].ent == reinterpret_cast<gentity_s*>(memory + index * 256));
    }
    for (auto byte : data.before) CHECK(byte == 0xa5);
    for (auto byte : data.after) CHECK(byte == 0xa5);
    useList_t fractions[] = {{nullptr, 1.25f}, {nullptr, 1.75f}, {nullptr, 3.25f}};
    CHECK(compare_use(fractions, fractions + 1) == 0);
    CHECK(compare_use(fractions, fractions + 2) == -2);
    CHECK(compare_use(fractions + 2, fractions) == 2);
    Player_SortUseList(data.entries.data(), 0);
    Player_SortUseList(data.entries.data(), 1);
    CHECK(VirtualFree(memory, 0, MEM_RELEASE));
    std::printf("Production usable-entity sorting: %zu-bit pointers, full records, legacy scores and guards passed\n", sizeof(void*) * 8);
}
