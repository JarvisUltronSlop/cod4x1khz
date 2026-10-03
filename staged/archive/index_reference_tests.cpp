// Includes generated exact native functions from source-owned index contract
// generator. Primitive dependencies are also native, not a mirrored algorithm.
#include "index_reference_extracted.inc"
#include <array>
#include <vector>
#include <cstdio>
#include <cstring>
#include <stdexcept>

unsigned cases = 0;
void require(bool value) { ++cases; if (!value) throw std::runtime_error("native index reference contract failed"); }
struct Encoded { std::array<std::uint8_t, 2048> data{}; int size = 0; };
Encoded encode(const std::vector<int>& values, int bits) {
    Encoded result;
    msg_t wire{};
    wire.data = result.data.data(); wire.maxsize = int(result.data.size());
    MSG_ClearLastReferencedEntity(&wire);
    SnapshotInfo_s info{};
    for (int value : values) {
        MSG_WriteBit1(&wire); // actual client-row presence prefix
        MSG_WriteEntityIndex(&info, &wire, value, bits);
    }
    require(!wire.overflowed);
    result.size = wire.cursize;
    return result;
}
void roundtrip(const std::vector<int>& values, int bits, int stale, int bitPrefix) {
    std::array<std::uint8_t, 2048> data{};
    msg_t wire{}; wire.data = data.data(); wire.maxsize = int(data.size());
    SnapshotInfo_s info{};
    for (int i = 0; i < bitPrefix; ++i) (i & 1 ? MSG_WriteBit1 : MSG_WriteBit0)(&wire);
    MSG_ClearLastReferencedEntity(&wire);
    for (int value : values) { MSG_WriteBit1(&wire); MSG_WriteEntityIndex(&info, &wire, value, bits); }
    require(!wire.overflowed);
    msg_t read{}; read.lastEntityRef = stale;
    MSG_InitReadOnly(&read, data.data(), wire.cursize);
    MSG_BeginReading(&read);
    require(read.lastEntityRef == stale); // explicit native init contract
    for (int i = 0; i < bitPrefix; ++i) require(MSG_ReadBit(&read) == (i & 1));
    MSG_ClearLastReferencedEntity(&read);
    require(read.lastEntityRef == -1);
    for (int value : values) {
        require(MSG_ReadBit(&read) == 1);
        require(MSG_ReadEntityIndex(&read, bits) == value);
        require(!read.overflowed);
    }
}
void branch_contract(int value, int bits, bool increment, bool smallDelta) {
    auto encoded = encode({value}, bits);
    msg_t read{};
    MSG_InitReadOnly(&read, encoded.data.data(), encoded.size); MSG_BeginReading(&read);
    require(MSG_ReadBit(&read) == 1); // client/entity fixture row marker
    require(MSG_ReadBit(&read) == int(increment));
    if (increment) return;
    if (bits == 10) require(MSG_ReadBit(&read) == int(!smallDelta));
    require(MSG_ReadBits(&read, smallDelta ? 4u : unsigned(bits)) == (smallDelta ? value + 1 : value));
    require(!read.overflowed);
}
void split_contract(const std::vector<int>& values, int bits) {
    auto encoded = encode(values, bits);
    for (int firstSize = 0; firstSize < encoded.size; ++firstSize) {
        msg_t read{}; read.lastEntityRef = 32767;
        MSG_InitReadOnlySplit(&read, encoded.data.data(), firstSize,
                             encoded.data.data() + firstSize, encoded.size - firstSize);
        MSG_BeginReading(&read);
        require(read.lastEntityRef == 32767);
        MSG_ClearLastReferencedEntity(&read);
        for (int value : values) {
            require(MSG_ReadBit(&read) == 1);
            require(MSG_ReadEntityIndex(&read, unsigned(bits)) == value);
            require(!read.overflowed);
        }
        require(read.readcount <= encoded.size);
    }
}
int main() {
    try {
        // This isolates the observed field: client0 is a one-bit increment from
        // -1. Native MSG_InitReadOnly+BeginReading retains stale0, yielding1.
        auto encoded = encode({0}, 6);
        msg_t stale{}; stale.lastEntityRef = 0;
        MSG_InitReadOnly(&stale, encoded.data.data(), encoded.size); MSG_BeginReading(&stale);
        require(stale.lastEntityRef == 0); require(MSG_ReadBit(&stale) == 1);
        require(MSG_ReadEntityIndex(&stale, 6) == 1); // actual uncorrected corruption
        std::printf("native stale0 client-index reproduction: expected=0 actual=1 (0x00000000 vs0x00000001)\n");
        // Check actual encoder discriminator boundaries independently from the
        // decoder's roundtrip: first14 has distance15; first15 has distance16.
        branch_contract(0, 6, true, false);
        branch_contract(63, 6, false, false);
        branch_contract(0, 10, true, false);
        branch_contract(14, 10, false, true);
        branch_contract(15, 10, false, false);
        branch_contract(1023, 10, false, false);
        std::array<int, 7> old{-1,0,1,17,63,1023,32767};
        for (int previous : old) for (int prefix = 0; prefix < 8; ++prefix) {
            roundtrip({0,1,2,7,63}, 6, previous, prefix);
            roundtrip({0,1,3,18,34,1022,1023}, 10, previous, prefix);
            roundtrip({16,17,31,47,511,1023}, 10, previous, prefix);
            roundtrip({63}, 6, previous, prefix); // full index at upper boundary
        }
        split_contract({0,1,2,7,63}, 6);
        split_contract({0,1,3,18,34,1022,1023}, 10);
        // Entity list starts independently after a preceding client list.
        auto clients = encode({0,1,63}, 6);
        msg_t reused{}; reused.lastEntityRef = 63;
        auto entities = encode({0,1,15,31,1023}, 10);
        MSG_InitReadOnly(&reused, entities.data.data(), entities.size); MSG_BeginReading(&reused);
        require(reused.lastEntityRef == 63); MSG_ClearLastReferencedEntity(&reused);
        for (int value : {0,1,15,31,1023}) { require(MSG_ReadBit(&reused) == 1); require(MSG_ReadEntityIndex(&reused,10) == value); }
        (void)clients;
        std::printf("native source-extracted index reference contracts passed: %u assertions; full/delta/increment, stale/zero initialization, byte/split boundaries and client/entity list reset\n", cases);
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
}
