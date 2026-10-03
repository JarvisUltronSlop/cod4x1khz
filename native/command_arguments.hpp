#pragma once
#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace khz::command {
inline constexpr unsigned MaxArguments = 512, MaxBytes = 8190;
// Offsets keep copied argument storage valid when this object itself is copied.
struct Arguments {
    std::array<char, MaxBytes> text{};
    std::array<unsigned, MaxArguments> offsets{}, lengths{};
    unsigned count = 0, bytes = 0;
    std::string_view At(unsigned i) const { return i < count ? std::string_view(text.data() + offsets[i], lengths[i]) : std::string_view{}; }
    bool Capture(int argc, const char* const* argv, unsigned byteLimit = MaxBytes) {
        if (argc < 1 || unsigned(argc) > MaxArguments || !argv || !byteLimit || byteLimit > MaxBytes) return false;
        std::array<unsigned, MaxArguments> measured{};
        unsigned used = 0;
        for (unsigned i = 0; i < unsigned(argc); ++i) {
            if (!argv[i]) return false;
            const auto remaining = byteLimit - used;
            unsigned length = 0;
            while (length < remaining && argv[i][length]) ++length;
            if (length == remaining) return false;
            measured[i] = length; used += measured[i] + 1;
            if (i + 1 < unsigned(argc) && used == byteLimit) return false;
        }
        // The dispatch name is one literal token, never an encoded command stream.
        if (!measured[0] || measured[0] >= 128) return false;
        for (unsigned i = 0; i < measured[0]; ++i) {
            const unsigned char c = static_cast<unsigned char>(argv[0][i]);
            if (c <= 32 || c == 127 || c == '"' || c == '\\' || c == ';' || c == '/') return false;
        }
        unsigned cursor = 0;
        for (unsigned i = 0; i < unsigned(argc); ++i) {
            offsets[i] = cursor; lengths[i] = measured[i];
            std::memcpy(text.data() + cursor, argv[i], measured[i] + 1); cursor += measured[i] + 1;
        }
        count = unsigned(argc); bytes = used; return true;
    }
};
}
