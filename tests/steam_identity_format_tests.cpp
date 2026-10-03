#include <steam_identity_format.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

// Requires native/ include path and ordinary C++17; no engine, Windows,
// provider or network dependency.
namespace format = khz::identity_format;
static unsigned assertions;
#define CHECK(value) do { ++assertions; if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::exit(1); } } while (0)

struct Vector { std::uint64_t id; const char* decimal; const char* steam3; };
static constexpr Vector vectors[] = {
    {0, "0", "[I:0:0]"},
    {(1ULL << 56) | (1ULL << 52) | (1ULL << 32) | 12345ULL, "76561197960278073", "[U:1:12345]"},
    {(32ULL << 56) | (1ULL << 52) | (1ULL << 32) | 12345ULL, "2310346613136044089", "[U:32:12345]"},
    {9999999999999999999ULL, "9999999999999999999", "[I:0:0]"},
    {10000000000000000000ULL, "10000000000000000000", "[I:0:0]"},
    {std::numeric_limits<std::uint64_t>::max(), "18446744073709551615", "[I:0:0]"}
};

static void checkVector(const Vector& vector, bool decimal, int capacity) {
    std::array<unsigned char, 80> bytes; bytes.fill(0xa5);
    char* output = reinterpret_cast<char*>(bytes.data() + 8);
    const bool complete = format::Selected(vector.id, decimal, output, capacity);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i < 8 || capacity <= 0 || i >= 8 + std::size_t(capacity)) CHECK(bytes[i] == 0xa5);
    }
    if (capacity <= 0) { CHECK(!complete); return; }
    const char* expected = decimal ? vector.decimal : vector.steam3;
    const std::size_t length = std::strlen(expected);
    if (decimal && (capacity < format::Steam64MinimumCapacity || length >= std::size_t(capacity))) {
        CHECK(!complete); CHECK(output[0] == '\0');
        // Refusal changes only the first byte, including capacity-20 overflow.
        for (int i = 1; i < capacity; ++i) CHECK(static_cast<unsigned char>(output[i]) == 0xa5);
        return;
    }
    const std::size_t copied = length < std::size_t(capacity) ? length : std::size_t(capacity - 1);
    CHECK(std::memcmp(output, expected, copied) == 0); CHECK(output[copied] == '\0');
    CHECK(complete == (copied == length));
}

int main() {
    for (const auto& vector : vectors) {
        for (int capacity = -2; capacity <= 64; ++capacity) {
            checkVector(vector, false, capacity);
            checkVector(vector, true, capacity);
            CHECK(!format::Selected(vector.id, false, nullptr, capacity));
            CHECK(!format::Selected(vector.id, true, nullptr, capacity));
        }
    }
    // Independent golden table covers each pinned printable account type.
    constexpr char types[] = {'U', 'M', 'G', 'A', 'P', 'C', 'g', 'T', ' ', 'a'};
    for (unsigned type = 1; type <= 10; ++type) {
        char expected[48], output[48];
        std::snprintf(expected, sizeof(expected), "[%c:39:4294967295]", types[type - 1]);
        CHECK(format::Steam3((39ULL << 56) | (std::uint64_t(type) << 52) | UINT32_MAX, output, sizeof(output)));
        CHECK(std::strcmp(expected, output) == 0);
    }
    for (unsigned universe : {0u, 5u, 31u, 40u, 255u}) {
        char output[48]; CHECK(format::Steam3((std::uint64_t(universe) << 56) | (1ULL << 52), output, sizeof(output)));
        CHECK(std::strcmp(output, "[I:0:0]") == 0);
    }
    for (unsigned type : {0u, 11u, 15u}) {
        char output[48]; CHECK(format::Steam3((1ULL << 56) | (std::uint64_t(type) << 52), output, sizeof(output)));
        CHECK(std::strcmp(output, "[I:0:0]") == 0);
    }
    // Dvar values 0/1 are supplied by the native adapter, never hard-coded here.
    char output[48]; CHECK(format::Selected(vectors[1].id, false, output, sizeof(output)));
    CHECK(std::strcmp(output, "[U:1:12345]") == 0);
    CHECK(format::Selected(vectors[1].id, true, output, sizeof(output)));
    CHECK(std::strcmp(output, "76561197960278073") == 0);
    std::printf("Steam identity formatter contracts: %u assertions, no provider authentication claim\n", assertions);
    return 0;
}
