// Standalone diagnostic backing for the engine's real minizip implementation.
#include <cstdio>
#include <cstdint>
#include <cstring>
FILE* FS_FileOpenReadBinary(const char* name) { return std::fopen(name, "rb"); }
void FS_FileClose(FILE* file) { std::fclose(file); }
int FS_FileSeek(FILE* file, int offset, int origin) { return std::fseek(file, offset, origin == 0 ? SEEK_CUR : origin == 1 ? SEEK_END : SEEK_SET); }
std::uint32_t FS_FileRead(void* output, std::uint32_t length, FILE* file) { return static_cast<std::uint32_t>(std::fread(output, 1, length, file)); }
int FS_FileGetFileSize(FILE* file) { const auto old = std::ftell(file); std::fseek(file, 0, SEEK_END); const auto length = std::ftell(file); std::fseek(file, old, SEEK_SET); return static_cast<int>(length); }
void Com_Memcpy(void* output, const void* input, const std::size_t length) { std::memcpy(output, input, length); }
