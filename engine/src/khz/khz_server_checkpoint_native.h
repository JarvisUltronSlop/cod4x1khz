#pragma once
#include "khz_server_checkpoint.h"
#include <cstdio>
#include <cstring>

namespace khz::server_native_checkpoint {
inline bool reject(char* text, unsigned capacity, const char* reason) {
    if (text && capacity) std::snprintf(text, capacity, "%s", reason); return false;
}
inline bool context(char* text, unsigned capacity) {
    return KHZ_ServerCheckpointContext() ? true : reject(text, capacity, "native_server_checkpoint_not_paused_owner_callback");
}
inline bool inside(const void* pointer, const void* base, std::size_t size, std::size_t length = 1) {
    const auto p = reinterpret_cast<std::uintptr_t>(pointer), b = reinterpret_cast<std::uintptr_t>(base);
    return p && b && length && length <= size && p >= b && p - b <= size - length;
}
// Raw BSS images deliberately have no aggregate/default initialization. Native
// VariableUnion constructors must never generate multi-megabyte CRT temporaries.
template<std::size_t N> struct alignas(128) Bytes {
    unsigned char value[N];
    void capture(const void* pointer) { std::memcpy(value, pointer, N); }
    void restore(void* pointer) const { std::memcpy(pointer, value, N); }
};
}
