#pragma once
#include <cstdint>

namespace khz {
struct GameplayRandomState { std::uint32_t crt, hold; };
// MSVC's Win32 rand() transition/output, made inspectable for deterministic
// restore. Unsigned arithmetic preserves its native 32-bit wrap semantics.
inline constexpr std::uint32_t advanceRandom(std::uint32_t& state) {
    state = state * 214013u + 2531011u; return state;
}
inline constexpr int msvcRandom(std::uint32_t& state) { return int((advanceRandom(state) >> 16) & 0x7fffu); }
// The stock flrand/irand stream takes the upper 15 bits at a different shift
// from CRT rand(). Sharing the transition must not change that native output.
inline constexpr std::uint32_t stockRandomBits(std::uint32_t& state) { return advanceRandom(state) >> 17; }
}
