#pragma once
#include <asset_wire.hpp>

namespace khz::asset_wire {
inline std::uint32_t readScalar(std::span<const std::byte> wire, std::size_t offset, unsigned bytes) {
    if ((bytes != 1 && bytes != 2 && bytes != 4) || offset > wire.size() || bytes > wire.size() - offset)
        throw std::runtime_error("Asset discriminator exceeds its disk record");
    std::uint32_t value{};
    for (unsigned i = 0; i < bytes; ++i)
        value |= std::uint32_t(std::to_integer<unsigned char>(wire[offset + i])) << (8 * i);
    return value;
}
struct Schema {
    const char* name;
    std::uint32_t wireBytes, nativeBytes;
    Layout (*select)(std::span<const std::byte>, std::vector<Field>&);
    bool dynamic{};
};
}
