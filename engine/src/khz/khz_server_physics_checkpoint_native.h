#pragma once
#include "khz_server_physics_checkpoint.h"
#include "khz_server_checkpoint_native.h"
#include <universal/pool_allocator.h>
#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace khz::server_physics_checkpoint {
using namespace khz::server_native_checkpoint;
constexpr unsigned RequiredParts = 0xff;
inline bool zero(const void* p, std::size_t bytes) {
    const auto* data = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < bytes; ++i) if (data[i]) return false;
    return true;
}
inline bool finite(float f) { return std::isfinite(f); }
// Walk only proven slot addresses, without Pool_FreeCount's unbounded loop.
// Native BSS pools legitimately have no free chain before ODE/Phys_Init.
inline bool emptyPool(const pooldata_t& pool, const void* storage,
    unsigned stride, unsigned count, bool allowDormant) {
    if (!storage || stride < sizeof(void*) || !count || count > 2048 || pool.activeCount) return false;
    if (!pool.firstFree) return allowDormant && zero(storage, std::size_t(stride) * count);
    unsigned char seen[256]{};
    auto* entry = pool.firstFree;
    unsigned visited = 0;
    const auto base = reinterpret_cast<std::uintptr_t>(storage);
    while (entry) {
        if (visited == count || !inside(entry, storage, std::size_t(stride) * count, sizeof(void*))) return false;
        const auto offset = reinterpret_cast<std::uintptr_t>(entry) - base;
        if (offset % stride) return false;
        const unsigned slot = unsigned(offset / stride);
        const unsigned char bit = static_cast<unsigned char>(1u << (slot & 7));
        if (seen[slot >> 3] & bit) return false;
        seen[slot >> 3] |= bit; ++visited;
        void* next;
        std::memcpy(&next, entry, sizeof(next)); entry = next;
    }
    return visited == count;
}
template<class T, int N> inline bool emptyIndices(int first, const T (&entries)[N], bool allowDormant) {
    if (allowDormant && first == 0 && zero(entries, sizeof(entries))) return true;
    unsigned char seen[(N + 7) / 8]{};
    unsigned visited = 0;
    while (first != -1) {
        if (first < 0 || first >= N || visited == N) return false;
        const unsigned char bit = static_cast<unsigned char>(1u << (first & 7));
        if (seen[first >> 3] & bit) return false;
        seen[first >> 3] |= bit; ++visited;
        int next; std::memcpy(&next, &entries[first], sizeof(next)); first = next;
    }
    return visited == N;
}
// An original native vtable/code pointer must remain in this executable's
// mapped image. This is an identity guard, never a pointer restoration file.
inline bool executablePointer(const void* p) {
    MEMORY_BASIC_INFORMATION m;
    if (!p || !VirtualQuery(p, &m, sizeof(m)) || m.State != MEM_COMMIT ||
        m.Type != MEM_IMAGE || m.AllocationBase != GetModuleHandleA(nullptr) ||
        (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const unsigned protect = m.Protect & 0xff;
    return protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
        protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}
inline bool nativeVtable(const void* object, std::uintptr_t& result) {
    std::memcpy(&result, object, sizeof(result));
    MEMORY_BASIC_INFORMATION m;
    if (!result || !VirtualQuery(reinterpret_cast<const void*>(result), &m, sizeof(m)) ||
        m.State != MEM_COMMIT || m.Type != MEM_IMAGE || m.AllocationBase != GetModuleHandleA(nullptr) ||
        (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const unsigned protect = m.Protect & 0xff;
    if (protect != PAGE_READONLY && protect != PAGE_READWRITE && protect != PAGE_WRITECOPY &&
        protect != PAGE_EXECUTE_READ && protect != PAGE_EXECUTE_READWRITE && protect != PAGE_EXECUTE_WRITECOPY) return false;
    const void* fn; std::memcpy(&fn, reinterpret_cast<const void*>(result), sizeof(fn));
    return executablePointer(fn);
}
inline bool registerPart(const KHZ_ServerCheckpointProvider& provider) {
    return KHZ_ServerCheckpointRequireParts(provider.domain, RequiredParts) &&
        KHZ_ServerCheckpointRegister(provider);
}
}
