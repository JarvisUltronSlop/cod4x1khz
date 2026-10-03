#include "gameplay_rng.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

int main() {
    constexpr int expected[] = {41,18467,6334,26500,19169,15724,11478,29358,26962,24464};
    std::uint32_t sequence = 1;
    for (const auto value : expected) CHECK(khz::msvcRandom(sequence) == value);
    std::uint32_t zero = 0, wrap = 0xFFFFFFFFu;
    CHECK(khz::msvcRandom(zero) == 38 && zero == 2531011u);
    CHECK(khz::msvcRandom(wrap) == 35 && wrap == 2316998u);

#ifdef _MSC_VER
    // Compare the installed Win32 CRT itself, including unsigned seed and
    // wrap semantics; a duplicate local formula cannot establish compatibility.
    constexpr unsigned seeds[] = {0,1,7,0x80000000u,0xFFFFFFFFu,0x937AD51Bu};
    for (const auto seed : seeds) {
        std::srand(seed); std::uint32_t native = seed;
        for (unsigned i = 0; i < 50000; ++i) CHECK(khz::msvcRandom(native) == std::rand());
    }
#endif

    khz::GameplayRandomState gameplay{0x71234567u,0x89ABCDEFu};
    auto independent = gameplay;
    std::uint32_t presentation = 0x6D2B79F5u;
    for (unsigned tick = 0; tick < 10000; ++tick) {
        // Superseded frames can consume any amount of presentation randomness.
        for (unsigned frame = 0; frame < tick % 17; ++frame) khz::msvcRandom(presentation);
        CHECK(khz::msvcRandom(gameplay.crt) == khz::msvcRandom(independent.crt));
        CHECK(khz::stockRandomBits(gameplay.hold) == khz::stockRandomBits(independent.hold));
        CHECK(gameplay.crt == independent.crt && gameplay.hold == independent.hold);
    }
    const auto checkpoint = gameplay;
    int first[256]; std::uint32_t held[256];
    for (unsigned i = 0; i < 256; ++i) { first[i] = khz::msvcRandom(gameplay.crt); held[i] = khz::stockRandomBits(gameplay.hold); }
    gameplay = checkpoint;
    for (unsigned i = 0; i < 256; ++i) { CHECK(first[i] == khz::msvcRandom(gameplay.crt)); CHECK(held[i] == khz::stockRandomBits(gameplay.hold)); }
    CHECK(gameplay.crt != checkpoint.crt && gameplay.hold != checkpoint.hold);
    std::puts("MSVC gameplay RNG compatibility and independent restored streams passed");
}
