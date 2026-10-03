#pragma once
#include "khz_server_collision_checkpoint.h"
#include "khz_server_checkpoint_native.h"
#include <Windows.h>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace khz::server_collision_checkpoint {
using namespace khz::server_native_checkpoint;
inline constexpr std::size_t SingleSpanLimit = 16u * 1024 * 1024;
inline constexpr std::size_t FingerprintLimit = 128u * 1024 * 1024;
inline constexpr unsigned MaxStaticModels = 65535;
inline constexpr unsigned MaxPartitions = 262144;
inline constexpr unsigned MaxSubmodels = 4095;
inline bool readable(const void* pointer, std::size_t bytes) {
    auto at = reinterpret_cast<std::uintptr_t>(pointer);
    if (!at || !bytes || bytes > UINTPTR_MAX - at) return false;
    const auto end = at + bytes;
    while (at < end) {
        MEMORY_BASIC_INFORMATION memory;
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &memory, sizeof(memory)) ||
            memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const unsigned protection = memory.Protect & 0xff;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
            return false;
        const auto start = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize > UINTPTR_MAX - start || start + memory.RegionSize <= at) return false;
        const auto next = start + memory.RegionSize;
        at = next < end ? next : end;
    }
    return true;
}
inline bool finite3(const float* value) {
    return value && std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}
inline bool bounds(const float* mins, const float* maxs) {
    return finite3(mins) && finite3(maxs) && mins[0] <= maxs[0] && mins[1] <= maxs[1] && mins[2] <= maxs[2];
}
struct Signature { std::uint64_t a, b; };
inline bool same(Signature a, Signature b) { return a.a == b.a && a.b == b.b; }
struct Fingerprint {
    Signature value;
    std::size_t bytes;
    void begin() { value = {14695981039346656037ull, 7809847782465536322ull}; bytes = 0; }
    bool append(const void* pointer, std::size_t count) {
        if (!count) return true;
        if (count > SingleSpanLimit || bytes > FingerprintLimit - count || !readable(pointer, count)) return false;
        const auto* data = static_cast<const unsigned char*>(pointer);
        for (std::size_t i = 0; i < count; ++i) {
            value.a = (value.a ^ data[i]) * 1099511628211ull;
            value.b = (value.b ^ (unsigned(data[i]) + 1)) * 14029467366897019727ull;
        }
        bytes += count; return true;
    }
    bool array(const void* pointer, std::size_t count, std::size_t stride) {
        return stride && count <= SingleSpanLimit / stride && append(pointer, count * stride);
    }
    bool string(const char* value) {
        if (!value) return false;
        for (unsigned i = 0; i < 256; ++i) {
            if (!append(value + i, 1)) return false;
            if (!value[i]) return true;
        }
        return false;
    }
};
inline void registerPart(const KHZ_ServerCheckpointProvider& provider) {
    if (!KHZ_ServerCheckpointRequireParts(provider.domain, KHZ_ServerCollisionRequiredParts) ||
        !KHZ_ServerCheckpointRegister(provider))
        Com_Error(ERR_FATAL, "Could not register complete native server Collision checkpoint part %u", provider.part);
}
template<class T> bool element(const T* pointer, const T* base, std::size_t count) {
    return count && pointer && base && count <= SIZE_MAX / sizeof(T) &&
        inside(pointer, base, count * sizeof(T), sizeof(T)) &&
        (reinterpret_cast<std::uintptr_t>(pointer) - reinterpret_cast<std::uintptr_t>(base)) % sizeof(T) == 0;
}
} // namespace khz::server_collision_checkpoint

