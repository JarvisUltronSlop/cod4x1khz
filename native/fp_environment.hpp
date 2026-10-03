#pragma once

#include <cstdint>
#include <cfenv>

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif
#if defined(_MSC_VER)
#include <float.h>
#endif

namespace khz {

struct FpEnvironment {
    std::uint32_t x87Control = 0;
    std::uint32_t mxcsr = 0;
    bool x87Available = false;
    bool sse2Available = false;
};

// Raw hardware control words make the replay gate check the environment that
// actually performs arithmetic, independently of presentation/D3D settings.
inline FpEnvironment gameplayFpEnvironment() noexcept {
    FpEnvironment result;
#if defined(_MSC_VER) && defined(_M_IX86)
    unsigned short fpControlWord = 0;
    __asm fnstcw fpControlWord
    result.x87Control = fpControlWord;
    result.x87Available = true;
#elif defined(__i386__) || defined(__x86_64__)
    unsigned short fpControlWord = 0;
    __asm__ __volatile__("fnstcw %0" : "=m"(fpControlWord));
    result.x87Control = fpControlWord;
    result.x87Available = true;
#endif
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
    result.mxcsr = _mm_getcsr();
    result.sse2Available = true;
#endif
    return result;
}

inline bool isGameplayFpEnvironment(const FpEnvironment& state) noexcept {
    // x87: exceptions masked, 53-bit significand, round to nearest.
    // SSE2: exceptions masked, round to nearest, gradual underflow; both
    // denormals-are-zero and flush-to-zero are disabled. Sticky status flags
    // do not affect comparisons and are deliberately excluded from the gate.
    return (!state.x87Available || (state.x87Control & 0x0f3f) == 0x023f) &&
        (!state.sse2Available || (state.mxcsr & 0xffc0) == 0x1f80) &&
        (state.x87Available || state.sse2Available);
}

// Call on each gameplay owner before the first tick. This is intentionally not
// called on rendering threads: their native presentation precision is retained.
inline FpEnvironment configureGameplayFpEnvironment() noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    unsigned short fpControlWord = 0;
    __asm fnstcw fpControlWord
    fpControlWord = static_cast<unsigned short>((fpControlWord & ~0x0f3f) | 0x023f);
    __asm fnclex
    __asm fldcw fpControlWord
#elif defined(__i386__) || defined(__x86_64__)
    unsigned short fpControlWord = 0;
    __asm__ __volatile__("fnstcw %0" : "=m"(fpControlWord));
    fpControlWord = static_cast<unsigned short>((fpControlWord & ~0x0f3f) | 0x023f);
    __asm__ __volatile__("fnclex\n\tfldcw %0" : : "m"(fpControlWord));
#elif defined(_MSC_VER)
    unsigned ignored = 0;
    _controlfp_s(&ignored, _MCW_EM | _RC_NEAR, _MCW_EM | _MCW_RC);
#else
    std::fenv_t ignored;
    std::feholdexcept(&ignored);
    std::fesetround(FE_TONEAREST);
#endif
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
    const auto old = _mm_getcsr();
    _mm_setcsr((old & ~(0x6000u | 0x8000u | 0x0040u | 0x003fu)) | 0x1f80u);
#endif
    return gameplayFpEnvironment();
}

} // namespace khz
