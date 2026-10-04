#include <qcommon/huffman.h>
#include <huffman_codes.hpp>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

void Com_Memset(void* destination, const int value, const std::size_t bytes) { std::memset(destination, value, bytes); }
void MyAssertHandler(const char*, int, int, const char*, ...) { std::abort(); }
const int frequencies[256] = {
#include <qcommon/msg_huffman_data.inc>
};

int reference(huff_t& tree, const unsigned char* input, unsigned char* output, int size, int capacity) {
    int bit = 0;
    for (int i = 0; i < size; ++i) {
        if (((bit + 7) >> 3) + 4 > capacity) return -1;
        Huff_offsetTransmit(&tree, input[i], output, &bit);
    }
    return (bit + 7) >> 3;
}

int main() {
    huffman_t native{};
    Huff_Init(&native); Huff_BuildFromData(&native.compressDecompress, frequencies);
    khz::huffman::Codes table;
    assert(table.build(native.compressDecompress.tree, native.compressDecompress.loc));
    unsigned maximum = 0;
    for (unsigned symbol = 0; symbol != 256; ++symbol) {
        maximum = (std::max)(maximum, table[symbol].length);
        assert(table[symbol].length == unsigned(Huff_bitCount(&native.compressDecompress, symbol)));
    }
    unsigned comparisons = 0;
    auto compare = [&](const std::vector<unsigned char>& input, int capacity) {
        std::vector<unsigned char> expected(capacity + 16, 0xa5), actual = expected;
        const auto old = reference(native.compressDecompress, input.data(), expected.data(), int(input.size()), capacity);
        const auto fast = table.encode(input.data(), actual.data(), int(input.size()), capacity);
        assert(old == fast && expected == actual); ++comparisons;
    };
    for (unsigned a = 0; a != 256; ++a)
        for (unsigned b = 0; b != 256; ++b) compare({static_cast<unsigned char>(a), static_cast<unsigned char>(b)}, 8);
    for (unsigned symbol = 0; symbol != 256; ++symbol)
        for (int length = 0; length <= 40; ++length)
            for (int capacity = 0; capacity <= 16; ++capacity)
                compare(std::vector<unsigned char>(length, static_cast<unsigned char>(symbol)), capacity);
    std::uint32_t random = 7;
    std::vector<unsigned char> packet(32768);
    for (auto& byte : packet) { random = random * 1664525 + 1013904223; byte = std::uint8_t(random >> 24); }
    compare(packet, 65536);
    for (int size : {0, 1, 3, 17, 257, 2048, 32767})
        compare(std::vector<unsigned char>(packet.begin(), packet.begin() + size), size * 2 + 4);
    assert(table.encode(nullptr, nullptr, 0, 0) == 0);
    assert(table.encode(nullptr, packet.data(), 1, 4) == -1);
    assert(table.encode(packet.data(), nullptr, 1, 4) == -1);
    assert(table.encode(packet.data(), packet.data(), -1, 4) == -1);
    assert(table.encode(packet.data(), packet.data(), 1, -1) == -1);
    // Independent packed callers share only the immutable startup table.
    std::array<unsigned long long, 4> sums{};
    std::array<std::thread, 4> workers;
    for (unsigned worker = 0; worker != workers.size(); ++worker)
        workers[worker] = std::thread([&, worker] {
            std::vector<unsigned char> output(65536);
            for (unsigned pass = 0; pass != 64; ++pass) {
                const auto count = table.encode(packet.data(), output.data(), int(packet.size()), int(output.size()));
                assert(count > 0);
                for (int i = 0; i < count; ++i) sums[worker] += output[i];
            }
        });
    for (auto& worker : workers) worker.join();
    for (const auto sum : sums) assert(sum == sums[0] && sum != 0);
    auto malformed = native.compressDecompress.loc[1]->parent;
    native.compressDecompress.loc[1]->parent = native.compressDecompress.loc[1];
    khz::huffman::Codes rejected;
    assert(!rejected.build(native.compressDecompress.tree, native.compressDecompress.loc) && !rejected.ready());
    native.compressDecompress.loc[1]->parent = malformed;
    std::vector<unsigned char> output(65536);
    // Architecture-independent fingerprint of every native code and a complete
    // deterministic packet. This is compared against the original x86 encoder.
    std::uint64_t wireDigest = 14695981039346656037ull;
    const auto digestByte = [&](unsigned char byte) { wireDigest = (wireDigest ^ byte) * 1099511628211ull; };
    for (unsigned symbol = 0; symbol != 256; ++symbol) {
        digestByte(static_cast<unsigned char>(table[symbol].length));
        for (unsigned shift = 0; shift != 32; shift += 8)
            digestByte(static_cast<unsigned char>(table[symbol].bits >> shift));
    }
    const auto wireSize = reference(native.compressDecompress, packet.data(), output.data(), int(packet.size()), int(output.size()));
    assert(wireSize > 0);
    for (int i = 0; i < wireSize; ++i) digestByte(output[i]);
    // Captured with HEAD's original x86 huffman.cpp and the same frequencies,
    // code table serialization and deterministic input on 2026-10-04.
    assert(wireDigest == 0x31442bde5f36413full);
    const auto measure = [&](bool packed) {
        const auto started = std::chrono::steady_clock::now();
        for (unsigned pass = 0; pass != 64; ++pass)
            assert((packed ? table.encode(packet.data(), output.data(), int(packet.size()), int(output.size())) :
                reference(native.compressDecompress, packet.data(), output.data(), int(packet.size()), int(output.size()))) > 0);
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    };
    const auto referenceUs = measure(false), packedUs = measure(true);
    std::printf("{\"process_bits\":%zu,\"wire_digest\":\"%016llx\",\"native_comparisons\":%u,\"max_code_bits\":%u,\"reference_us\":%lld,\"packed_us\":%lld,\"same_wire_bytes\":true,\"match_timing_gate\":false}\n",
        sizeof(void*) * 8, static_cast<unsigned long long>(wireDigest), comparisons, maximum,
        static_cast<long long>(referenceUs), static_cast<long long>(packedUs));
}
