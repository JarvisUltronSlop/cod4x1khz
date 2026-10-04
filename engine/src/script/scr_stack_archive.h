#pragma once
#include <universal/q_shared.h>
#include "scr_variable.h"
#include <cstddef>
#include <cstring>

// Suspended VM stacks are process-local. Their value payload must hold native
// pointers, while script integers, object IDs and function handles stay 32-bit.
inline constexpr int SCR_STACK_VALUE_BYTES = sizeof(VariableUnion);
inline constexpr int SCR_STACK_ENTRY_BYTES = 1 + SCR_STACK_VALUE_BYTES;
inline constexpr int SCR_STACK_HEADER_BYTES = offsetof(VariableStackBuffer, buf);

inline VariableUnion Scr_ReadStackValue(const char* payload) {
    VariableUnion value;
    std::memcpy(&value, payload, sizeof(value));
    return value;
}
inline void Scr_WriteStackValue(char* payload, const VariableUnion& value) {
    std::memcpy(payload, &value, sizeof(value));
}
