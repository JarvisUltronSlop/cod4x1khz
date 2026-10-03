#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace khz::canonical {
// Diagnostic replay identity, not an authentication or content-signature hash.
// Two independently mixed 64-bit accumulators make per-domain divergence easy
// to identify. Callers serialize reviewed fields, never native object padding,
// process addresses, wall clocks or inactive storage.
struct Digest {
    std::uint64_t first, second, bytes;
    bool valid;
};
class Hash {
public:
    void octet(std::uint8_t value) {
        first_ = (first_ ^ value) * 1099511628211ULL;
        second_ = ((second_ << 9) | (second_ >> 55)) ^ (std::uint64_t(value) + 0x9E3779B97F4A7C15ULL);
        second_ *= 0xD6E8FEB86659FD93ULL;
        ++bytes_;
    }
    void boolean(bool value) { octet(value ? 1 : 0); }
    void u16(std::uint16_t value) { for (unsigned i = 0; i < 2; ++i) octet(static_cast<std::uint8_t>(value >> (8 * i))); }
    void u32(std::uint32_t value) { for (unsigned i = 0; i < 4; ++i) octet(static_cast<std::uint8_t>(value >> (8 * i))); }
    void u64(std::uint64_t value) { for (unsigned i = 0; i < 8; ++i) octet(static_cast<std::uint8_t>(value >> (8 * i))); }
    void f32(float value) {
        static_assert(sizeof(float) == sizeof(std::uint32_t));
        std::uint32_t bits; std::memcpy(&bits, &value, sizeof(bits));
        if ((bits & 0x7F800000u) == 0x7F800000u) valid_ = false;
        u32(bits); // exact IEEE representation, including the sign of zero
    }
    template<class Value> void scalar(Value value) {
        static_assert(std::is_integral_v<Value> || std::is_enum_v<Value>, "serialize a reviewed arithmetic field, not a pointer/object");
        if constexpr (std::is_same_v<Value, bool>) boolean(value);
        else if constexpr (sizeof(Value) == 1) octet(static_cast<std::uint8_t>(value));
        else if constexpr (sizeof(Value) == 2) u16(static_cast<std::uint16_t>(value));
        else if constexpr (sizeof(Value) == 4) u32(static_cast<std::uint32_t>(value));
        else { static_assert(sizeof(Value) == 8); u64(static_cast<std::uint64_t>(value)); }
    }
    void scalar(float value) { f32(value); }
    template<class Value, std::size_t Count> void array(const Value (&values)[Count]) {
        u32(static_cast<std::uint32_t>(Count));
        for (const auto& value : values) {
            if constexpr (std::is_array_v<Value>) array(value);
            else scalar(value);
        }
    }
    // Bounded asset identifiers are names, not addresses or process-local
    // interned-string IDs. Null and empty are deliberately distinct.
    void text(const char* value, std::size_t maximum = 4096) {
        boolean(value != nullptr);
        if (!value) return;
        std::size_t length = 0;
        while (length < maximum && value[length]) ++length;
        if (length == maximum) { reject(); return; }
        u32(static_cast<std::uint32_t>(length));
        for (std::size_t i = 0; i < length; ++i) octet(static_cast<std::uint8_t>(value[i]));
    }
    void digest(const Digest& value) { u64(value.first); u64(value.second); u64(value.bytes); if (!value.valid) reject(); }
    void reject() { valid_ = false; }
    Digest finish() const { return {first_, second_, bytes_, valid_}; }
private:
    std::uint64_t first_ = 14695981039346656037ULL;
    std::uint64_t second_ = 0x6A09E667F3BCC909ULL;
    std::uint64_t bytes_ = 0;
    bool valid_ = true;
};
inline bool equal(const Digest& a, const Digest& b) {
    return a.valid && b.valid && a.first == b.first && a.second == b.second && a.bytes == b.bytes;
}
} // namespace khz::canonical
