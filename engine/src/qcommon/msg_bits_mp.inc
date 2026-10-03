// Source-owned definitions, included by msg_mp.cpp. The fallback bodies below
// preserve the pinned 8aadf94 native implementation for compatibility testing.
#if defined(COD4X1KHZ)
#include <msg_bits.hpp>
#endif

int __cdecl MSG_GetByte(msg_t *msg, int where)
{
    if (where < msg->cursize)
        return msg->data[where];
    iassert(msg->splitData);
    return msg->splitData[where - msg->cursize];
}

void __cdecl MSG_WriteBits(msg_t *msg, int value, uint32_t bits)
{
    iassert((unsigned)bits <= 32);
    iassert(!msg->readOnly);
#if defined(COD4X1KHZ)
    khz::msg_bits::write(msg, value, bits);
#else
    int bit;
    if (msg->maxsize - msg->cursize >= 4)
    {
        while (bits)
        {
            --bits;
            bit = msg->bit & 7;
            if (!bit)
            {
                msg->bit = 8 * msg->cursize;
                msg->data[msg->cursize++] = 0;
            }
            if ((value & 1) != 0)
                msg->data[msg->bit >> 3] |= 1 << bit;
            ++msg->bit;
            value >>= 1;
        }
    }
    else
        msg->overflowed = 1;
#endif
}

int __cdecl MSG_ReadBits(msg_t *msg, uint32_t bits)
{
    iassert((unsigned)bits <= 32);
#if defined(COD4X1KHZ)
    return khz::msg_bits::read(msg, bits, MSG_GetByte);
#else
    int value = 0;
    for (int i = 0; i < (int)bits; ++i)
    {
        int bit = msg->bit & 7;
        if (!bit)
        {
            if (msg->readcount >= msg->splitSize + msg->cursize)
            {
                msg->overflowed = 1;
                return -1;
            }
            msg->bit = 8 * msg->readcount++;
        }
        value |= ((MSG_GetByte(msg, msg->bit >> 3) >> bit) & 1) << i;
        ++msg->bit;
    }
    return value;
#endif
}
