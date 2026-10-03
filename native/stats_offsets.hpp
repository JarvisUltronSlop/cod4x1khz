#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace khz::stats {
inline constexpr std::size_t BufferBytes = 8192;
inline constexpr int ByteCount = 2000;
inline constexpr int Count = 3498;
struct Field { std::size_t offset = 0, width = 0; };
// Native MP stats contain a four-byte prefix, 2000 byte counters and
// 1498 little-endian uint32 counters. Tail bytes are outside public indices.
constexpr Field Locate(int index) {
    if (index < 0 || index >= Count) return {};
    return index < ByteCount ? Field{4 + std::size_t(index), 1} :
        Field{2004 + std::size_t(index - ByteCount) * 4, 4};
}
inline bool Fits(Field field, std::size_t bytes) {
    return field.width && field.offset <= bytes && field.width <= bytes - field.offset;
}
inline bool Read(const void* buffer, std::size_t bytes, int index, std::uint32_t* value) {
    const auto field = Locate(index);
    if (!buffer || !value || !Fits(field, bytes)) return false;
    const auto* source = static_cast<const std::uint8_t*>(buffer) + field.offset;
    *value = source[0];
    if (field.width == 4) *value |= std::uint32_t(source[1]) << 8 |
        std::uint32_t(source[2]) << 16 | std::uint32_t(source[3]) << 24;
    return true;
}
inline bool Write(void* buffer, std::size_t bytes, int index, std::uint32_t value, bool* changed = nullptr) {
    if (changed) *changed = false;
    const auto field = Locate(index);
    if (!buffer || !Fits(field, bytes) || (field.width == 1 && value > 255)) return false;
    std::uint32_t previous = 0;
    if (!Read(buffer, bytes, index, &previous)) return false;
    if (previous == value) return true;
    auto* destination = static_cast<std::uint8_t*>(buffer) + field.offset;
    destination[0] = std::uint8_t(value);
    if (field.width == 4) {
        destination[1] = std::uint8_t(value >> 8);
        destination[2] = std::uint8_t(value >> 16);
        destination[3] = std::uint8_t(value >> 24);
    }
    if (changed) *changed = true;
    return true;
}
}
