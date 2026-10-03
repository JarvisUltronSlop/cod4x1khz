// Only external diagnostic/Huffman readiness dependencies are fixtures.
// Actual native msg_t layout is emitted from msg_mp.h by the source extractor.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <msg_bits.hpp>
struct SnapshotInfo_s {};
struct DebugDvar { struct { bool enabled; } current{false}; };
DebugDvar debugDvar;
DebugDvar* msg_printEntityNums = &debugDvar;
constexpr int CON_CHANNEL_SERVER=0,CON_CHANNEL_SYSTEM=0;
inline bool SV_IsPacketDataNetworkData() { return false; }
inline void Com_Printf(int,const char*,...) { std::abort(); }
inline void MyAssertHandler(const char*,int,int,const char*,...) { std::abort(); }
inline void MSG_InitHuffman() { std::abort(); } // msgInit1: never called
inline int msgInit=1;
#define iassert(value) assert(value)


struct msg_t // sizeof=0x28
{                                       // ...
    int overflowed;                     // ...
    int readOnly;                       // ...
    uint8_t* data;              // ...
    uint8_t* splitData;         // ...
    int maxsize;                        // ...
    int cursize;                        // ...
    int splitSize;                      // ...
    int readcount;                      // ...
    int bit;                            // ...
    int lastEntityRef;                  // ...
};

static_assert(sizeof(msg_t) == 0x28, "Native Win32 message layout changed");

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


void __cdecl MSG_InitReadOnly(msg_t *buf, uint8_t *data, int length)
{
    iassert( data );
    if (!msgInit)
        MSG_InitHuffman();
    buf->readOnly = 1;
    buf->data = data;
    buf->maxsize = length;
    buf->cursize = length;
    buf->splitData = 0;
    buf->splitSize = 0;
}

void __cdecl MSG_InitReadOnlySplit(msg_t *buf, uint8_t *data, int length, uint8_t *data2, int length2)
{
    iassert( data );
    iassert( data2 );
    if (!msgInit)
        MSG_InitHuffman();
    buf->readOnly = 1;
    buf->data = data;
    buf->maxsize = length2 + length;
    buf->cursize = length;
    buf->splitData = data2;
    buf->splitSize = length2;
}

void __cdecl MSG_BeginReading(msg_t *msg)
{
    msg->overflowed = 0;
    msg->readcount = 0;
    msg->bit = 0;
}

void __cdecl MSG_WriteBit0(msg_t *msg)
{
    iassert( !msg->readOnly );
    if ((msg->bit & 7) == 0)
    {
        if (msg->cursize >= msg->maxsize)
        {
            msg->overflowed = 1;
            return;
        }
        msg->bit = 8 * msg->cursize;
        msg->data[msg->cursize++] = 0;
    }
    ++msg->bit;
}

void __cdecl MSG_WriteBit1(msg_t *msg)
{
    int bit; // [esp+4h] [ebp-4h]

    iassert( !msg->readOnly );
    bit = msg->bit & 7;
    if (!bit)
    {
        if (msg->cursize >= msg->maxsize)
        {
            msg->overflowed = 1;
            return;
        }
        msg->bit = 8 * msg->cursize;
        msg->data[msg->cursize++] = 0;
    }
    msg->data[msg->bit++ >> 3] |= 1 << bit;
}

int __cdecl MSG_ReadBit(msg_t *msg)
{
    int Byte; // eax
    int bit; // [esp+0h] [ebp-8h]

    bit = msg->bit & 7;
    if (!bit)
    {
        if (msg->readcount >= msg->splitSize + msg->cursize)
        {
            msg->overflowed = 1;
            return -1;
        }
        msg->bit = 8 * msg->readcount++;
    }
    Byte = MSG_GetByte(msg, msg->bit >> 3);
    ++msg->bit;
    return (Byte >> bit) & 1;
}

void __cdecl MSG_ClearLastReferencedEntity(msg_t *msg)
{
    msg->lastEntityRef = -1;
}

int __cdecl MSG_ReadEntityIndex(msg_t *msg, uint32_t indexBits)
{
    if (MSG_ReadBit(msg))
    {
        if (msg_printEntityNums->current.enabled)
            Com_Printf(CON_CHANNEL_SYSTEM, "Entity num: 1 bit (inc)\n");
        ++msg->lastEntityRef;
    }
    else if (indexBits != 10 || MSG_ReadBit(msg))
    {
        if (msg_printEntityNums->current.enabled)
            Com_Printf(CON_CHANNEL_SYSTEM, "Entity num: %i bits (full)\n", indexBits + 2);
        msg->lastEntityRef = MSG_ReadBits(msg, indexBits);
    }
    else
    {
        if (msg_printEntityNums->current.enabled)
            Com_Printf(CON_CHANNEL_SYSTEM, "Entity num: %i bits (delta)\n", 6);
        msg->lastEntityRef += MSG_ReadBits(msg, 4u);
    }
    if (msg_printEntityNums->current.enabled)
        Com_Printf(CON_CHANNEL_SYSTEM, "Read entity num %i\n", msg->lastEntityRef);
    if (msg->lastEntityRef < 0)
        MyAssertHandler(
            ".\\qcommon\\msg_mp.cpp",
            1309,
            0,
            "%s\n\t(msg->lastEntityRef) = %i",
            "(msg->lastEntityRef >= 0)",
            msg->lastEntityRef);
    return msg->lastEntityRef;
}

void __cdecl MSG_WriteEntityIndex(SnapshotInfo_s *snapInfo, msg_t *msg, int index, int indexBits)
{
    iassert( !msg->readOnly );

    if (msg_printEntityNums->current.enabled && SV_IsPacketDataNetworkData())
        Com_Printf(CON_CHANNEL_SERVER, "Writing entity num %i\n", index);

    iassert(index - msg->lastEntityRef > 0);

    if (index - msg->lastEntityRef == 1)
    {
        if (msg_printEntityNums->current.enabled && SV_IsPacketDataNetworkData())
            Com_Printf(CON_CHANNEL_SYSTEM, "Wrote entity num: 1 bit (inc)\n");
        MSG_WriteBit1(msg);
    }
    else
    {
        MSG_WriteBit0(msg);
        if (indexBits == 10 && index - msg->lastEntityRef < 16)
        {
            if (msg_printEntityNums->current.enabled && SV_IsPacketDataNetworkData())
                Com_Printf(CON_CHANNEL_SYSTEM, "Wrote entity num: %i bits (delta)\n", 6);
            
            iassert(index - msg->lastEntityRef > 0);

            MSG_WriteBit0(msg);
            MSG_WriteBits(msg, index - msg->lastEntityRef, 4u);
        }
        else
        {
            if (msg_printEntityNums->current.enabled && SV_IsPacketDataNetworkData())
                Com_Printf(CON_CHANNEL_SYSTEM, "Wrote entity num: %i bits (full)\n", indexBits + 2);

            if (indexBits == 10)
                MSG_WriteBit1(msg);

            MSG_WriteBits(msg, index, indexBits);
        }
    }
    msg->lastEntityRef = index;
}
