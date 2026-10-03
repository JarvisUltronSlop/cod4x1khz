#pragma once
#include <array>
#include <cstdint>
#include <limits>

namespace khz::huffman {
struct Code { std::uint32_t bits = 0; unsigned length = 0; };
class Codes {
public:
    template<class Node> bool build(const Node* root, Node* const* leaves) {
        ready_ = false;
        if (!root || root->parent || !leaves) return false;
        for (unsigned symbol = 0; symbol != codes_.size(); ++symbol) {
            const auto* node = leaves[symbol]; Code code;
            if (!node || node->symbol != int(symbol)) return false;
            while (node != root) {
                if (!node->parent || code.length == 25) return false;
                const auto* parent = node->parent;
                if (parent->left != node && parent->right != node) return false;
                code.bits = (code.bits << 1) | unsigned(parent->right == node);
                ++code.length; node = parent;
            }
            if (!code.length) return false;
            codes_[symbol] = code;
        }
        ready_ = true; return true;
    }
    bool ready() const { return ready_; }
    const Code& operator[](unsigned symbol) const { return codes_[symbol]; }
    int encode(const std::uint8_t* input, std::uint8_t* output, int size, int capacity) const {
        if (!ready_ || size < 0 || capacity < 0 || size > (std::numeric_limits<int>::max)() / 25 ||
            (size && (!input || !output))) return -1;
        unsigned bit = 0;
        for (int i = 0; i < size; ++i) {
            // Keep the native compressor's conservative four-byte headroom
            // check, including its exact failure boundary and partial prefix.
            if (((bit + 7) >> 3) + 4 > unsigned(capacity)) return -1;
            const auto& code = codes_[input[i]];
            const unsigned byte = bit >> 3, offset = bit & 7;
            const std::uint32_t packed = code.bits << offset;
            output[byte] = std::uint8_t((offset ? output[byte] : 0) | packed);
            const unsigned written = offset + code.length;
            if (written > 8) output[byte + 1] = std::uint8_t(packed >> 8);
            if (written > 16) output[byte + 2] = std::uint8_t(packed >> 16);
            if (written > 24) output[byte + 3] = std::uint8_t(packed >> 24);
            bit += code.length;
        }
        return int((bit + 7) >> 3);
    }
private:
    std::array<Code, 256> codes_{};
    bool ready_ = false;
};
}
