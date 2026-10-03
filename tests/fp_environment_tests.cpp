#include "fp_environment.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::exit(1); } } while (0)

namespace {
void changeEnvironment(unsigned precisionAndRounding, unsigned sseRounding) {
#if defined(_MSC_VER) && defined(_M_IX86)
    unsigned short fpControlWord = 0;
    __asm fnstcw fpControlWord
    fpControlWord = static_cast<unsigned short>((fpControlWord & ~0x0f00) | precisionAndRounding);
    __asm fldcw fpControlWord
#elif defined(__i386__) || defined(__x86_64__)
    unsigned short fpControlWord = 0;
    __asm__ __volatile__("fnstcw %0" : "=m"(fpControlWord));
    fpControlWord = static_cast<unsigned short>((fpControlWord & ~0x0f00) | precisionAndRounding);
    __asm__ __volatile__("fldcw %0" : : "m"(fpControlWord));
#else
    (void)precisionAndRounding;
#endif
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | sseRounding | 0x8040u);
#else
    (void)sseRounding;
#endif
}

struct Replay {
    std::uint64_t positionBits = 0;
    std::uint32_t velocityBits = 0;
    khz::FpEnvironment configured;
};

Replay runReplay(unsigned precisionAndRounding, unsigned sseRounding) {
    changeEnvironment(precisionAndRounding, sseRounding);
    CHECK(!khz::isGameplayFpEnvironment(khz::gameplayFpEnvironment()));
    Replay result;
    result.configured = khz::configureGameplayFpEnvironment();
    CHECK(khz::isGameplayFpEnvironment(result.configured));
    // This trace performs actual float/double arithmetic under the two thread
    // environments. Input generation uses integers and never depends on FP.
    volatile double position = 0.125;
    volatile float velocity = 0.0f;
    std::uint32_t random = 0x68bc21ebu;
    for (unsigned tick = 0; tick < 20000; ++tick) {
        random = random * 1664525u + 1013904223u;
        const float acceleration = static_cast<int>((random >> 16) & 255) - 128.0f;
        velocity = velocity + acceleration * 0.001f;
        velocity = velocity * 0.99975f;
        position = position + static_cast<double>(velocity) * 0.001;
    }
    const double finalPosition = position;
    const float finalVelocity = velocity;
    std::memcpy(&result.positionBits, &finalPosition, sizeof(finalPosition));
    std::memcpy(&result.velocityBits, &finalVelocity, sizeof(finalVelocity));
    // Nearest rounding and gradual underflow must survive real arithmetic.
    CHECK(std::nearbyint(1.5) == 2.0 && std::nearbyint(2.5) == 2.0);
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
    const __m128 small = _mm_set_ss(std::numeric_limits<float>::min());
    const __m128 half = _mm_mul_ss(small, _mm_set_ss(0.5f));
    const __m128 restored = _mm_mul_ss(half, _mm_set_ss(2.0f));
    CHECK(_mm_cvtss_f32(half) > 0.0f);
    CHECK(_mm_cvtss_f32(restored) == std::numeric_limits<float>::min());
#endif
    CHECK(khz::isGameplayFpEnvironment(khz::gameplayFpEnvironment()));
    return result;
}
}

int main() {
    const auto presentationBefore = khz::gameplayFpEnvironment();
    Replay client, server;
    std::thread prediction([&] { client = runReplay(0x0400, 0x4000); });
    std::thread authority([&] { server = runReplay(0x0b00, 0x2000); });
    prediction.join(); authority.join();
    CHECK(client.positionBits == server.positionBits);
    CHECK(client.velocityBits == server.velocityBits);
    CHECK(client.configured.x87Control == server.configured.x87Control);
    CHECK(client.configured.mxcsr == server.configured.mxcsr);
    const auto presentationAfter = khz::gameplayFpEnvironment();
    CHECK(presentationBefore.x87Control == presentationAfter.x87Control);
    CHECK(presentationBefore.mxcsr == presentationAfter.mxcsr);
    std::puts("Gameplay FP environments and replay match; presentation unchanged.");
}
