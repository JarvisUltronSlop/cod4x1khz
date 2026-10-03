#pragma once
#include <cstdint>

// Byte-chunk implementation of the native MP bit cursor contract. The cursor
// may address a partial byte preceding separately appended whole bytes. At the
// next byte boundary it jumps to cursize/readcount, exactly like the stock loop.
// No alignment, speculative word access, allocation or shared scratch state.
namespace khz::msg_bits {
template<class Message>
inline void write(Message* msg, int value, unsigned bits) {
    // Preserve the native four-byte headroom check even when bits == 0.
    if (msg->maxsize - msg->cursize < 4) { msg->overflowed = 1; return; }
    std::uint32_t remainingValue = static_cast<std::uint32_t>(value);
    while (bits) {
        const unsigned offset = static_cast<unsigned>(msg->bit) & 7u;
        if (!offset) {
            msg->bit = 8 * msg->cursize;
            msg->data[msg->cursize++] = 0;
        }
        const unsigned available = 8u - offset;
        const unsigned take = bits < available ? bits : available;
        const unsigned mask = (1u << take) - 1u;
        msg->data[msg->bit >> 3] |= static_cast<std::uint8_t>((remainingValue & mask) << offset);
        msg->bit += static_cast<int>(take);
        remainingValue >>= take; // take <= 8, including full-width values.
        bits -= take;
    }
}

template<class Message, class GetByte>
inline int read(Message* msg, unsigned bits, GetByte getByte) {
    std::uint32_t value = 0;
    unsigned written = 0;
    while (bits) {
        const unsigned offset = static_cast<unsigned>(msg->bit) & 7u;
        if (!offset) {
            if (msg->readcount >= msg->splitSize + msg->cursize) {
                msg->overflowed = 1;
                return -1; // Keep every preceding successful cursor advance.
            }
            msg->bit = 8 * msg->readcount++;
        }
        const unsigned available = 8u - offset;
        const unsigned take = bits < available ? bits : available;
        const unsigned mask = (1u << take) - 1u;
        value |= ((static_cast<unsigned>(getByte(msg, msg->bit >> 3)) >> offset) & mask) << written;
        msg->bit += static_cast<int>(take);
        written += take;
        bits -= take;
    }
    return static_cast<int>(value);
}
}
