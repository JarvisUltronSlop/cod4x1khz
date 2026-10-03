#include <qcommon/msg_mp.h>
#include <array>
#include <cassert>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

void MyAssertHandler(const char*, int, int, const char*, ...) { std::abort(); }
static_assert(sizeof(msg_t) == 40, "Compile the actual Win32 native message layout");

// Compile both the production source-owned definitions and the pinned fallback
// with the real native msg_t, rather than a reduced mock message layout.
#undef COD4X1KHZ
#define MSG_GetByte ReferenceGetByte
#define MSG_WriteBits ReferenceWriteBits
#define MSG_ReadBits ReferenceReadBits
#include <qcommon/msg_bits_mp.inc>
#undef MSG_GetByte
#undef MSG_WriteBits
#undef MSG_ReadBits
#define COD4X1KHZ 1
#include <qcommon/msg_bits_mp.inc>

static unsigned comparisons;
static std::uint32_t randomState = 0xa751980bu;
static std::uint32_t randomValue() {
    randomState ^= randomState << 13; randomState ^= randomState >> 17;
    randomState ^= randomState << 5; return randomState;
}
struct Buffer {
    std::array<unsigned char, 96> bytes;
    msg_t message{};
    Buffer() { for (auto& byte : bytes) byte = static_cast<unsigned char>(randomValue()); message.data = bytes.data() + 8; }
    void copy(const Buffer& other) {
        bytes = other.bytes; message = other.message; message.data = bytes.data() + 8;
        if (other.message.splitData) message.splitData = message.data + message.cursize;
    }
};
static void equal(const Buffer& a, const Buffer& b) {
    ++comparisons;
    assert(a.bytes == b.bytes);
    assert(a.message.overflowed == b.message.overflowed);
    assert(a.message.readOnly == b.message.readOnly);
    assert(a.message.maxsize == b.message.maxsize && a.message.cursize == b.message.cursize);
    assert(a.message.splitSize == b.message.splitSize && a.message.readcount == b.message.readcount);
    assert(a.message.bit == b.message.bit && a.message.lastEntityRef == b.message.lastEntityRef);
}
static void writeCase(int cursor, int bytes, int headroom, unsigned width, int value) {
    Buffer a, b;
    a.message.bit = cursor; a.message.cursize = bytes; a.message.maxsize = bytes + headroom;
    a.message.overflowed = headroom & 1; a.message.lastEntityRef = 73;
    b.copy(a);
    ReferenceWriteBits(&a.message, value, width); MSG_WriteBits(&b.message, value, width);
    equal(a, b);
}
static void readCase(int cursor, int readcount, int bytes, int split, unsigned width) {
    Buffer a, b;
    a.message.bit = cursor; a.message.readcount = readcount;
    a.message.cursize = bytes; a.message.splitSize = split;
    a.message.splitData = a.message.data + bytes; a.message.readOnly = 1;
    a.message.overflowed = readcount & 1; a.message.lastEntityRef = 74;
    b.copy(a);
    const int expected = ReferenceReadBits(&a.message, width);
    const int actual = MSG_ReadBits(&b.message, width);
    assert(actual == expected); equal(a, b);
}
static void mixedWrites() {
    for (unsigned run = 0; run < 4000; ++run) {
        Buffer a, b;
        a.message.maxsize = 72; b.copy(a);
        for (unsigned step = 0; step < 45; ++step) {
            if ((randomValue() & 3) == 0) {
                // Stock MSG_WriteByte appends whole bytes without changing bit.
                const unsigned char byte = static_cast<unsigned char>(randomValue());
                if (a.message.cursize == a.message.maxsize) a.message.overflowed = b.message.overflowed = 1;
                else { a.message.data[a.message.cursize++] = byte; b.message.data[b.message.cursize++] = byte; }
            } else {
                const unsigned width = randomValue() % 33;
                const int value = static_cast<int>(randomValue());
                ReferenceWriteBits(&a.message, value, width); MSG_WriteBits(&b.message, value, width);
            }
            equal(a, b);
        }
    }
}
static void mixedReads() {
    for (unsigned run = 0; run < 4000; ++run) {
        Buffer a, b;
        a.message.cursize = 36; a.message.splitSize = 36;
        a.message.splitData = a.message.data + 36; a.message.readOnly = 1; b.copy(a);
        for (unsigned step = 0; step < 45; ++step) {
            if ((randomValue() & 3) == 0) {
                // Stock byte reads advance readcount without clearing bit.
                if (a.message.readcount >= 72) a.message.overflowed = b.message.overflowed = 1;
                else {
                    assert(ReferenceGetByte(&a.message, a.message.readcount++) == MSG_GetByte(&b.message, b.message.readcount++));
                }
            } else {
                const unsigned width = randomValue() % 33;
                assert(ReferenceReadBits(&a.message, width) == MSG_ReadBits(&b.message, width));
            }
            equal(a, b);
        }
    }
}
template<bool Fast> static long long measure(bool read, std::uint64_t& checksum) {
    constexpr unsigned fields = 16000;
    std::vector<unsigned char> bytes(fields * 4 + 8, 0xa5);
    const auto started = std::chrono::steady_clock::now();
    for (unsigned run = 0; run < 32; ++run) {
        msg_t message{}; message.data = bytes.data(); message.maxsize = static_cast<int>(bytes.size());
        if (read) message.cursize = static_cast<int>(bytes.size());
        for (unsigned i = 0; i < fields; ++i) {
            const unsigned width = 1 + i % 32;
            if (read) checksum += static_cast<std::uint32_t>(Fast ? MSG_ReadBits(&message, width) : ReferenceReadBits(&message, width));
            else if constexpr (Fast) MSG_WriteBits(&message, static_cast<int>(0xac59013du + i), width);
            else ReferenceWriteBits(&message, static_cast<int>(0xac59013du + i), width);
        }
        checksum += static_cast<unsigned>(message.bit) + static_cast<unsigned>(bytes[run]);
    }
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
}
int main(int argc, char** argv) {
    constexpr int values[] = {0, 1, -1, INT_MIN, INT_MAX, 0x12345678, static_cast<int>(0xa55aa55au)};
    for (int offset = 0; offset < 8; ++offset)
        for (int gap = 0; gap < 6; ++gap)
            for (int headroom = 0; headroom < 10; ++headroom)
                for (unsigned width = 0; width <= 32; ++width)
                    for (int value : values) writeCase(offset, offset ? 1 + gap : gap, headroom, width, value);
    // Every split seam and partial-read failure, including all 32-bit values.
    for (int total = 0; total <= 12; ++total)
        for (int split = 0; split <= total; ++split)
            for (int consumed = 0; consumed <= total; ++consumed)
                for (int offset = 0; offset < 8; ++offset) {
                    if (offset && !consumed) continue;
                    for (unsigned width = 0; width <= 32; ++width)
                        readCase(offset ? 8 * (consumed - 1) + offset : 8 * consumed,
                                 consumed, total - split, split, width);
                }
    mixedWrites(); mixedReads();
    std::uint64_t a = 0, b = 0;
    const auto refWrite = measure<false>(false, a), fastWrite = measure<true>(false, b); assert(a == b);
    a = b = 0;
    const auto refRead = measure<false>(true, a), fastRead = measure<true>(true, b); assert(a == b);
    if (argc > 1) {
        std::ofstream output(argv[1]); assert(output);
        output << "{\"passed\":true,\"native_msg_t\":true,\"comparisons\":" << comparisons
               << ",\"reference_write_us\":" << refWrite << ",\"packed_write_us\":" << fastWrite
               << ",\"reference_read_us\":" << refRead << ",\"packed_read_us\":" << fastRead
               << ",\"scope\":\"Exact bytes and cursor/failure compatibility; isolated microbenchmark\",\"match_timing_gate\":false}\n";
    }
    std::printf("Native bit fields: %u exact comparisons; writes %lld/%lld us; reads %lld/%lld us\n", comparisons, refWrite, fastWrite, refRead, fastRead);
}
