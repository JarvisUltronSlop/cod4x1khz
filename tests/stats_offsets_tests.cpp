#include "stats_offsets.hpp"
#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
namespace stats_codec = khz::stats;
int main() {
    // Independent native-layout boundaries, including the actual XP index.
    constexpr struct { int index; unsigned offset, width; } fields[] = {
        {0, 4, 1}, {1999, 2003, 1}, {2000, 2004, 4}, {2301, 3208, 4}, {3497, 7992, 4}
    };
    for (const auto& expected : fields) {
        const auto actual = stats_codec::Locate(expected.index);
        CHECK(actual.offset == expected.offset && actual.width == expected.width);
    }
    std::array<unsigned char, stats_codec::BufferBytes + 2> guarded;
    guarded.fill(0xcd);
    auto* buffer = guarded.data() + 1; // Deliberately unaligned uint32 storage.
    for (int index = 0; index != stats_codec::Count; ++index) {
        const auto value = index < 2000 ? unsigned(index % 256) : 0x80000000u + unsigned(index);
        bool changed = false;
        CHECK(stats_codec::Write(buffer, stats_codec::BufferBytes, index, value, &changed)); CHECK(changed == (index >= 2000 || value != 0xcd));
        std::uint32_t retrieved = 0; CHECK(stats_codec::Read(buffer, stats_codec::BufferBytes, index, &retrieved)); CHECK(retrieved == value);
        CHECK(stats_codec::Write(buffer, stats_codec::BufferBytes, index, value, &changed)); CHECK(!changed);
    }
    CHECK(guarded.front() == 0xcd && guarded.back() == 0xcd);
    for (unsigned i = 0; i != 4; ++i) CHECK(buffer[i] == 0xcd); // header/checksum unchanged
    for (unsigned i = 7996; i != stats_codec::BufferBytes; ++i) CHECK(buffer[i] == 0xcd); // reserved tail unchanged
    const auto before = guarded;
    std::uint32_t value = 0x12345678;
    for (const int invalid : {-1, INT_MIN, 3498, INT_MAX}) {
        CHECK(!stats_codec::Read(buffer, stats_codec::BufferBytes, invalid, &value)); CHECK(value == 0x12345678);
        CHECK(!stats_codec::Write(buffer, stats_codec::BufferBytes, invalid, 123)); CHECK(before == guarded);
    }
    CHECK(!stats_codec::Write(buffer, stats_codec::BufferBytes, 1999, 256)); CHECK(before == guarded);
    CHECK(!stats_codec::Read(buffer, 7995, 3497, &value)); CHECK(!stats_codec::Write(buffer, 7995, 3497, 0)); CHECK(before == guarded);
    CHECK(stats_codec::Read(buffer, 7996, 3497, &value));
    CHECK(!stats_codec::Read(nullptr, stats_codec::BufferBytes, 0, &value)); CHECK(!stats_codec::Read(buffer, stats_codec::BufferBytes, 0, nullptr));
    CHECK(!stats_codec::Write(nullptr, stats_codec::BufferBytes, 0, 0));
    CHECK(stats_codec::Write(buffer, stats_codec::BufferBytes, 2000, 0x78563412));
    CHECK(buffer[2004] == 0x12 && buffer[2005] == 0x34 && buffer[2006] == 0x56 && buffer[2007] == 0x78);
    CHECK(stats_codec::Write(buffer, stats_codec::BufferBytes, 3497, UINT32_MAX));
    CHECK(stats_codec::Read(buffer, stats_codec::BufferBytes, 3497, &value) && value == UINT32_MAX);
    std::puts("bounded native MP stats codec tests passed");
}
