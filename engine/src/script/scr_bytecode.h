#pragma once
#include <cstdint>
#include <cstring>

// Bytecode integers remain 32-bit. Only embedded process code addresses grow.
inline constexpr size_t SCR_SWITCH_ENTRY_BYTES = sizeof(uint32_t) + sizeof(const char *);
template<typename T> inline T Scr_ReadOperand(const char **cursor) {
    T value;
    std::memcpy(&value, *cursor, sizeof(value));
    *cursor += sizeof(value);
    return value;
}
inline uint32_t Scr_ReadSwitchKey(const void *entry) {
    uint32_t value;
    std::memcpy(&value, entry, sizeof(value));
    return value;
}
void EmitCodepos(const char *pos);
void EmitString(uint32_t value);
void EmitFloat(float value);
void EmitUnsignedShort(unsigned short value);
void EmitShort(short value);
void EmitInteger(int value);
void EmitByte(uint8_t value);
int CompareCaseInfo(const void *first, const void *second);
void Scr_PatchJumpOffset(const char* operand, int32_t offset);
void ConnectBreakStatements();
void ConnectContinueStatements();
