#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Bounded public identifier formatting; this is not an authentication provider.
namespace khz::identity_format {
inline constexpr int Steam64MinimumCapacity = 20;
inline constexpr int Steam64MaximumCapacity = 21;

inline bool Steam64(std::uint64_t id, char* output, int capacity) {
    if (!output || capacity <= 0) return false;
    output[0] = '\0';
    // Pinned sapi.c clears positive buffers shorter than 20 regardless of id.
    if (capacity < Steam64MinimumCapacity) return false;
    char reversed[20];
    unsigned count = 0;
    do {
        reversed[count++] = char('0' + id % 10);
        id /= 10;
    } while (id);
    // A full uint64 may have 20 digits. Its terminator needs byte 21; never
    // reproduce pinned sprintf's capacity-20 overflow or return truncated id.
    if (count >= unsigned(capacity)) return false;
    for (unsigned i = 0; i < count; ++i) output[i] = reversed[count - 1 - i];
    output[count] = '\0';
    return true;
}

inline bool Steam3(std::uint64_t id, char* output, int capacity) {
    if (!output || capacity <= 0) return false;
    const unsigned universe = unsigned(id >> 56);
    const unsigned type = unsigned((id >> 52) & 15);
    const bool valid = universe >= 1 && (universe <= 4 || (universe >= 32 && universe < 40)) &&
        type >= 1 && type <= 10;
    char text[48];
    if (valid) {
        // Preserve pinned accounttypechars, including the type-9 space.
        constexpr char types[] = "IUMGAPCgT a";
        const int length = std::snprintf(text, sizeof(text), "[%c:%u:%u]", types[type], universe,
                                        unsigned(id & UINT32_MAX));
        if (length < 0 || std::size_t(length) >= sizeof(text)) { output[0] = '\0'; return false; }
    } else {
        std::memcpy(text, "[I:0:0]", sizeof("[I:0:0]"));
    }
    const std::size_t length = std::strlen(text);
    const std::size_t available = std::size_t(capacity - 1);
    const std::size_t copied = length < available ? length : available;
    if (copied) std::memcpy(output, text, copied);
    output[copied] = '\0';
    return copied == length;
}

// The production caller must read the configured dvar under KHZ_DvarScope.
// Passing the selected value keeps the pure formatter independent of globals.
inline bool Selected(std::uint64_t id, bool steam64, char* output, int capacity) {
    return steam64 ? Steam64(id, output, capacity) : Steam3(id, output, capacity);
}
}
