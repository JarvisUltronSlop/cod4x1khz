#include "canonical.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
using khz::canonical::Hash;
using khz::canonical::equal;

int main() {
    Hash golden;
    for (const auto value : "hello") if (value) golden.octet(value);
    CHECK(golden.finish().first == 0xA430D84680AABD0BULL);
    CHECK(golden.finish().bytes == 5 && golden.finish().valid);

    Hash integer, explicitBytes;
    integer.u16(0xA1B2); integer.u32(0xC3D4E5F6); integer.u64(0x1234567890ABCDEFULL);
    const std::uint8_t bytes[] = {0xB2,0xA1,0xF6,0xE5,0xD4,0xC3,0xEF,0xCD,0xAB,0x90,0x78,0x56,0x34,0x12};
    for (auto value : bytes) explicitBytes.octet(value);
    CHECK(equal(integer.finish(), explicitBytes.finish()));
    Hash signedValue, unsignedValue;
    signedValue.scalar(std::int32_t(-1)); unsignedValue.u32(0xFFFFFFFF);
    CHECK(equal(signedValue.finish(), unsignedValue.finish()));

    Hash positiveZero, negativeZero;
    positiveZero.f32(0.f); negativeZero.f32(-0.f);
    CHECK(!equal(positiveZero.finish(), negativeZero.finish()));
    Hash nan, infinity;
    nan.f32((std::numeric_limits<float>::quiet_NaN)()); infinity.f32((std::numeric_limits<float>::infinity)());
    CHECK(!nan.finish().valid && !infinity.finish().valid);
    Hash invalidParent; invalidParent.digest(nan.finish()); CHECK(!invalidParent.finish().valid);

    Hash splitA, splitB, nullText, emptyText, unterminated;
    splitA.text("ab"); splitA.text("c"); splitB.text("a"); splitB.text("bc");
    CHECK(!equal(splitA.finish(), splitB.finish()));
    nullText.text(nullptr); emptyText.text(""); CHECK(!equal(nullText.finish(), emptyText.finish()));
    const char noTerminator[] = {'a','b'}; unterminated.text(noTerminator, sizeof(noTerminator)); CHECK(!unterminated.finish().valid);
    const float matrix[2][2] = {{1,2},{3,4}};
    Hash array, flattened; array.array(matrix);
    flattened.u32(2); flattened.u32(2); flattened.f32(1); flattened.f32(2); flattened.u32(2); flattened.f32(3); flattened.f32(4);
    CHECK(equal(array.finish(), flattened.finish()));

    // Native padding and process addresses cannot influence a reviewed record.
    struct Native { char kind; float phase; const char* name; } left, right;
    std::memset(&left, 0xAA, sizeof(left)); std::memset(&right, 0x55, sizeof(right));
    char firstName[] = "same_asset", secondName[] = "same_asset";
    left.kind = right.kind = 7; left.phase = right.phase = .125f; left.name = firstName; right.name = secondName;
    const auto serialize = [](const Native& value) { Hash hash; hash.scalar(value.kind); hash.f32(value.phase); hash.text(value.name); return hash.finish(); };
    CHECK(equal(serialize(left), serialize(right)));
    right.phase = .25f; CHECK(!equal(serialize(left), serialize(right)));
    std::puts("canonical field serialization passed");
}
