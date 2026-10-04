#pragma once
#include <cstddef>
#include <cstdint>

// Runtime buffer state, shared by the production message code and its fixtures.
// Only the pointed-to bytes are transmitted; native pointers are never on wire.
struct msg_t
{
    int overflowed;
    int readOnly;
    uint8_t* data;
    uint8_t* splitData;
    int maxsize;
    int cursize;
    int splitSize;
    int readcount;
    int bit;
    int lastEntityRef;
};
static_assert(sizeof(int) == 4);
static_assert(sizeof(msg_t) == 32 + 2 * sizeof(void*));
static_assert(offsetof(msg_t, data) == 8);
static_assert(offsetof(msg_t, maxsize) == 8 + 2 * sizeof(void*));
