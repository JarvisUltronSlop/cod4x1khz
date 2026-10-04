#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>

struct gentity_s;
struct useList_t {
    gentity_s* ent;
    float score;
};
static_assert(sizeof(useList_t) == (sizeof(void*) == 8 ? 16 : 8));

// Retain the original integer score comparison on both architectures.
inline int __cdecl compare_use(const void* first, const void* second) {
    const auto* left = static_cast<const useList_t*>(first);
    const auto* right = static_cast<const useList_t*>(second);
    return static_cast<int>(left->score - right->score);
}
inline void Player_SortUseList(useList_t* entries, std::size_t count) {
    std::qsort(entries, count, sizeof(*entries), compare_use);
}
