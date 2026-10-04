#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <iterator>
#include <span>
#include <stdexcept>
#include <vector>
#include <utility>

namespace khz::asset_wire {
enum class FieldKind : std::uint8_t { Bytes, Pointer32, RelativeRecord32 };
struct Field {
    std::uint32_t wireOffset{}, nativeOffset{}, bytes{};
    FieldKind kind = FieldKind::Bytes;
};
struct Layout {
    std::uint32_t wireBytes{}, nativeBytes{};
    std::span<const Field> fields;
};
inline std::uint32_t read32(const std::byte* bytes) noexcept {
    return std::uint32_t(std::to_integer<unsigned char>(bytes[0])) |
        (std::uint32_t(std::to_integer<unsigned char>(bytes[1])) << 8) |
        (std::uint32_t(std::to_integer<unsigned char>(bytes[2])) << 16) |
        (std::uint32_t(std::to_integer<unsigned char>(bytes[3])) << 24);
}
inline std::uintptr_t pointerBits(std::uint32_t wire) noexcept {
    // These two values denote inline and shared-inline allocations, rather than
    // actual zone offsets. Preserve the engine's native pointer comparisons.
    if (wire == UINT32_MAX) return (std::numeric_limits<std::uintptr_t>::max)();
    if (wire == UINT32_MAX - 1) return (std::numeric_limits<std::uintptr_t>::max)() - 1;
    return wire;
}
inline void validate(const Layout& layout) {
    if (!layout.wireBytes || !layout.nativeBytes || layout.fields.empty())
        throw std::runtime_error("Empty asset record layout");
    std::uint32_t wireEnd{}, nativeEnd{};
    for (const auto& field : layout.fields) {
        const auto nativeBytes = field.kind == FieldKind::Pointer32 ? sizeof(std::uintptr_t) : field.bytes;
        if (!field.bytes || (field.kind != FieldKind::Bytes && field.bytes != 4) ||
            field.wireOffset < wireEnd || field.nativeOffset < nativeEnd ||
            field.wireOffset > layout.wireBytes || field.bytes > layout.wireBytes - field.wireOffset ||
            field.nativeOffset > layout.nativeBytes || nativeBytes > layout.nativeBytes - field.nativeOffset)
            throw std::runtime_error("Invalid or overlapping asset record fields");
        wireEnd = field.wireOffset + field.bytes;
        nativeEnd = field.nativeOffset + static_cast<std::uint32_t>(nativeBytes);
    }
}
inline void decode(const Layout& layout, std::span<const std::byte> source,
    std::span<std::byte> destination, std::size_t count = 1) {
    validate(layout);
    if (count > source.size() / layout.wireBytes || source.size() != count * layout.wireBytes ||
        count > destination.size() / layout.nativeBytes)
        throw std::runtime_error("Asset record span does not match its layout");
    if (!count) return;
    const auto input = reinterpret_cast<std::uintptr_t>(source.data());
    const auto output = reinterpret_cast<std::uintptr_t>(destination.data());
    const auto outputBytes = count * layout.nativeBytes;
    if (input <= output ? output - input < source.size() : input - output < outputBytes)
        throw std::runtime_error("Asset record decoding requires distinct storage");
    std::memset(destination.data(), 0, outputBytes);
    for (std::size_t i = 0; i < count; ++i) {
        const auto* wire = source.data() + i * layout.wireBytes;
        auto* native = destination.data() + i * layout.nativeBytes;
        for (const auto& field : layout.fields) {
            if (field.kind == FieldKind::Pointer32) {
                const auto value = pointerBits(read32(wire + field.wireOffset));
                std::memcpy(native + field.nativeOffset, &value, sizeof(value));
            } else if (field.kind == FieldKind::RelativeRecord32) {
                std::int32_t wireOffset{};
                std::memcpy(&wireOffset, wire + field.wireOffset, sizeof(wireOffset));
                if (wireOffset % static_cast<std::int32_t>(layout.wireBytes))
                    throw std::runtime_error("Relative asset child offset splits a record");
                const auto scaled = std::int64_t(wireOffset) / layout.wireBytes * layout.nativeBytes;
                if (scaled < INT32_MIN || scaled > INT32_MAX)
                    throw std::runtime_error("Relative asset child offset exceeds native range");
                const auto nativeOffset = static_cast<std::int32_t>(scaled);
                std::memcpy(native + field.nativeOffset, &nativeOffset, sizeof(nativeOffset));
            } else std::memcpy(native + field.nativeOffset, wire + field.wireOffset, field.bytes);
        }
    }
}
inline std::uint32_t offset(std::uint32_t block, std::uint32_t byte) {
    if (block >= 9 || byte >= 0x0fffffffu) throw std::runtime_error("Asset zone offset is out of range");
    return (block << 28) + byte + 1;
}

// Load-time metadata only. The engine receives actual native pointers after
// resolution; render/gameplay code never performs table lookups for asset use.
class Relocations {
    struct Range {
        std::uint32_t begin{}, end{};
        std::byte* native{};
        std::uint32_t wireStride{}, nativeStride{};
        std::vector<Field> fields;
    };
    struct Block { std::uint32_t bytes{}; std::vector<Range> ranges; };
    std::array<Block, 9> blocks_;
public:
    void rewind(std::uint32_t block, std::uint32_t position) {
        if (block >= 9 || position > blocks_[block].bytes)
            throw std::runtime_error("Invalid asset relocation rewind");
        auto& ranges = blocks_[block].ranges;
        auto first = std::lower_bound(ranges.begin(), ranges.end(), position,
            [](const auto& entry, auto value) { return entry.begin < value; });
        if (first != ranges.begin() && std::prev(first)->end > position)
            throw std::runtime_error("Asset rewind splits a decoded record");
        ranges.erase(first, ranges.end());
    }
    void reset(std::span<const std::uint32_t, 9> sizes) {
        for (unsigned i = 0; i < 9; ++i) {
            if (sizes[i] > 0x0fffffffu) throw std::runtime_error("Asset zone block is too large");
            blocks_[i].bytes = sizes[i]; blocks_[i].ranges.clear();
        }
    }
    void add(std::uint32_t block, std::uint32_t begin, const Layout& layout,
        std::span<std::byte> native, std::size_t count = 1) {
        validate(layout);
        if (block >= 9 || begin > blocks_[block].bytes ||
            count > (blocks_[block].bytes - begin) / layout.wireBytes ||
            count > native.size() / layout.nativeBytes)
            throw std::runtime_error("Asset relocation range exceeds zone storage");
        if (!count) return;
        auto& ranges = blocks_[block].ranges;
        const auto end = begin + static_cast<std::uint32_t>(count * layout.wireBytes);
        const auto place = std::lower_bound(ranges.begin(), ranges.end(), begin,
            [](const auto& entry, auto value) { return entry.begin < value; });
        if ((place != ranges.end() && end > place->begin) ||
            (place != ranges.begin() && std::prev(place)->end > begin))
            throw std::runtime_error("Overlapping asset relocation ranges");
        Range entry{begin, end, native.data(), layout.wireBytes, layout.nativeBytes,
            {layout.fields.begin(), layout.fields.end()}};
        ranges.insert(place, std::move(entry));
    }
    void addBytes(std::uint32_t block, std::uint32_t begin, std::span<std::byte> native) {
        static constexpr Field byte{0, 0, 1};
        add(block, begin, Layout{1, 1, {&byte, 1}}, native, native.size());
    }
    bool registered(std::uint32_t encoded) const {
        if (!encoded) return true;
        if (encoded >= UINT32_MAX - 1) throw std::runtime_error("Inline asset sentinel cannot be relocated");
        const auto raw = encoded - 1, block = raw >> 28, byte = raw & 0x0fffffffu;
        if (block >= 9 || byte >= blocks_[block].bytes) throw std::runtime_error("Asset reference exceeds its zone block");
        const auto& ranges = blocks_[block].ranges;
        auto place = std::upper_bound(ranges.begin(), ranges.end(), byte,
            [](auto value, const auto& entry) { return value < entry.begin; });
        return place != ranges.begin() && byte < std::prev(place)->end;
    }
    std::byte* resolve(std::uint32_t encoded) const {
        if (!encoded) return nullptr;
        if (encoded >= UINT32_MAX - 1) throw std::runtime_error("Inline asset sentinel cannot be relocated");
        const auto raw = encoded - 1, block = raw >> 28, byte = raw & 0x0fffffffu;
        if (block >= 9 || byte >= blocks_[block].bytes) throw std::runtime_error("Asset reference exceeds its zone block");
        const auto& ranges = blocks_[block].ranges;
        auto place = std::upper_bound(ranges.begin(), ranges.end(), byte,
            [](auto value, const auto& entry) { return value < entry.begin; });
        if (place == ranges.begin()) throw std::runtime_error("Unregistered asset reference");
        --place;
        if (byte >= place->end) throw std::runtime_error("Unregistered asset reference");
        const auto relative = byte - place->begin;
        const auto element = relative / place->wireStride, inside = relative % place->wireStride;
        for (const auto& field : place->fields) {
            if (inside < field.wireOffset) break;
            if (inside - field.wireOffset < field.bytes) {
                const auto delta = inside - field.wireOffset;
                if (field.kind == FieldKind::Pointer32 && delta)
                    throw std::runtime_error("Asset reference points inside an encoded pointer");
                return place->native + std::size_t(element) * place->nativeStride + field.nativeOffset + delta;
            }
        }
        // A reference to an object points to its start, even if its first member
        // is preceded by ABI padding. Other padding is never a serialized value.
        if (!inside) return place->native + std::size_t(element) * place->nativeStride;
        throw std::runtime_error("Asset reference points to record padding");
    }
    std::uintptr_t alias(std::uint32_t encoded) const {
        if (!encoded) return 0;
        const auto* location = resolve(encoded);
        // Require an original four-byte pointer slot, excluding scalar/raw data.
        // Registered layouts guarantee that its complete native slot exists.
        const auto raw = encoded - 1, block = raw >> 28, byte = raw & 0x0fffffffu;
        const auto& ranges = blocks_[block].ranges;
        auto place = std::upper_bound(ranges.begin(), ranges.end(), byte,
            [](auto value, const auto& entry) { return value < entry.begin; });
        --place;
        const auto inside = (byte - place->begin) % place->wireStride;
        for (const auto& field : place->fields) if (field.wireOffset == inside && field.kind == FieldKind::Pointer32) {
            std::uintptr_t value{}; std::memcpy(&value, location, sizeof(value)); return value;
        }
        throw std::runtime_error("Asset alias does not refer to a native pointer slot");
    }
};
}
