#include "gfx_hash.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <stdexcept>
namespace gfx {
std::string sha256(std::span<const std::byte> data) {
    if(data.size()>ULONG_MAX) throw std::runtime_error("Graphics cache key input too large");
    std::array<unsigned char,32> bytes{};
    if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data.data())),ULONG(data.size()),bytes.data(),ULONG(bytes.size()))<0) throw std::runtime_error("Graphics SHA256 failed");
    std::string result; result.reserve(64); const char* hex="0123456789abcdef";
    for(auto b:bytes) { result+=hex[b>>4]; result+=hex[b&15]; } return result;
}
}
