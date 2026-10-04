#pragma once
#include <asset_wire.hpp>

namespace khz::asset_wire {
// Each zone retains its original disk offsets while native records consume a
// separately checked arena. Geometry blocks retain their exact byte layout.
class Streams {
    struct Block { std::span<std::byte> native; std::uint32_t wireSize{}, wirePos{}; std::size_t nativePos{}; };
    struct Frame { unsigned previous{}; std::uint32_t wire{}; std::size_t native{}; };
    std::array<Block, 9> blocks_{};
    std::array<Frame, 64> stack_{};
    unsigned current_{}, depth_{};
public:
    void reset(std::span<const std::uint32_t, 9> wireSizes,
        const std::array<std::span<std::byte>, 9>& native) {
        current_ = depth_ = 0;
        for (unsigned i = 0; i < 9; ++i) {
            if (wireSizes[i] > 0x0fffffffu || native[i].size() < wireSizes[i] ||
                (i >= 7 && native[i].size() != wireSizes[i]))
                throw std::runtime_error("Asset stream arena has an invalid capacity");
            blocks_[i] = {native[i], wireSizes[i], 0, 0};
        }
    }
    unsigned index() const noexcept { return current_; }
    unsigned depth() const noexcept { return depth_; }
    std::uint32_t wirePosition() const noexcept { return blocks_[current_].wirePos; }
    std::uint32_t wireRemaining() const noexcept { return blocks_[current_].wireSize - blocks_[current_].wirePos; }
    std::byte* position() const noexcept {
        const auto& b = blocks_[current_];
        return b.native.empty() ? b.native.data() : b.native.data() + b.nativePos;
    }
    std::span<std::byte> remaining() const noexcept { return blocks_[current_].native.subspan(blocks_[current_].nativePos); }
    void align(unsigned mask) {
        if (mask > 4095 || (mask & (mask + 1))) throw std::runtime_error("Invalid asset alignment mask");
        auto& b = blocks_[current_];
        const auto wire = (std::uint64_t(b.wirePos) + mask) & ~std::uint64_t(mask);
        const auto nativeMask = current_ < 7 && mask >= 3 ? (std::max)(mask, unsigned(sizeof(void*) - 1)) : mask;
        const auto address = reinterpret_cast<std::uintptr_t>(position());
        if (address > UINTPTR_MAX - nativeMask) throw std::runtime_error("Asset alignment overflows native address space");
        const auto padding = ((address + nativeMask) & ~std::uintptr_t(nativeMask)) - address;
        if (wire > b.wireSize || padding > b.native.size() - b.nativePos)
            throw std::runtime_error("Asset alignment exceeds stream storage");
        b.wirePos = static_cast<std::uint32_t>(wire); b.nativePos += padding;
    }
    void check(std::size_t wire, std::size_t native) const {
        const auto& b = blocks_[current_];
        if (wire > b.wireSize - b.wirePos || native > b.native.size() - b.nativePos ||
            (current_ >= 7 && wire != native))
            throw std::runtime_error("Asset record exceeds stream storage");
    }
    void advance(std::size_t wire, std::size_t native) {
        check(wire, native);
        auto& b = blocks_[current_];
        b.wirePos += static_cast<std::uint32_t>(wire); b.nativePos += native;
    }
    void push(unsigned index) {
        if (index >= 9 || depth_ >= stack_.size()) throw std::runtime_error("Asset stream stack overflow");
        const auto previous = current_; current_ = index;
        stack_[depth_++] = {previous, blocks_[index].wirePos, blocks_[index].nativePos};
    }
    // The temporary block is rewound to the position captured after switching,
    // matching the retail loader. Persistent blocks retain their cursor.
    void pop(Relocations& relocations) {
        if (!depth_) throw std::runtime_error("Asset stream stack underflow");
        const auto frame = stack_[--depth_];
        if (!current_) {
            relocations.rewind(0, frame.wire);
            blocks_[0].wirePos = frame.wire; blocks_[0].nativePos = frame.native;
        }
        current_ = frame.previous;
    }
};
}
