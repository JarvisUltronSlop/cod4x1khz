// CoD4x public identifier conversion behavior and GUID salt are adapted from
// upstream/cod4x_server/src/sapi.c (ddd65fc). Preserve upstream COPYING and
// license notices when redistributing this combined project.
#include "khz_identity.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
#include <qcommon/cmd.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <server_mp/server_mp.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace khz::identity {
namespace {
struct Algorithm {
    BCRYPT_ALG_HANDLE handle = nullptr;
    ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
};
struct Key {
    BCRYPT_KEY_HANDLE handle = nullptr;
    ~Key() { if (handle) BCryptDestroyKey(handle); }
};
bool openP256(Algorithm& algorithm) {
    return BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) >= 0;
}
bool importPrivate(const PrivateKey& key, Algorithm& algorithm, Key& imported) {
    BCRYPT_ECCKEY_BLOB header{}; std::memcpy(&header, key.blob.data(), sizeof(header));
    return header.dwMagic == BCRYPT_ECDSA_PRIVATE_P256_MAGIC && header.cbKey == 32 &&
        openP256(algorithm) && BCryptImportKeyPair(algorithm.handle, nullptr, BCRYPT_ECCPRIVATE_BLOB,
            &imported.handle, const_cast<PUCHAR>(key.blob.data()), ULONG(key.blob.size()), 0) >= 0;
}
bool importPublic(const PublicKey& key, Algorithm& algorithm, Key& imported) {
    BCRYPT_ECCKEY_BLOB header{}; std::memcpy(&header, key.data(), sizeof(header));
    return header.dwMagic == BCRYPT_ECDSA_PUBLIC_P256_MAGIC && header.cbKey == 32 &&
        openP256(algorithm) && BCryptImportKeyPair(algorithm.handle, nullptr, BCRYPT_ECCPUBLIC_BLOB,
            &imported.handle, const_cast<PUCHAR>(key.data()), ULONG(key.size()), 0) >= 0;
}
bool proofHash(const Challenge& challenge, Fingerprint& result) {
    // Explicit little-endian encoding: no compiler packing or uninitialized
    // padding enters the signature. The domain prevents reuse in other APIs.
    std::array<std::uint8_t, 90> bytes{};
    constexpr char domain[] = "cod4x1khz-proof1";
    static_assert(sizeof(domain) - 1 == 16);
    std::memcpy(bytes.data(), domain, 16);
    std::memcpy(bytes.data() + 16, challenge.boot.data(), 32);
    std::memcpy(bytes.data() + 48, challenge.nonce.data(), 32);
    for (unsigned i = 0; i != 4; ++i) {
        bytes[80 + i] = std::uint8_t(challenge.challenge >> (i * 8));
        bytes[84 + i] = std::uint8_t(challenge.protocol >> (i * 8));
    }
    bytes[88] = std::uint8_t(challenge.qport); bytes[89] = std::uint8_t(challenge.qport >> 8);
    return Sha256(bytes.data(), bytes.size(), result);
}
int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool readKey(const wchar_t* filename, PrivateKey& key) {
    const HANDLE file = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::array<std::uint8_t, 4096> protectedBytes{};
    DWORD length = GetFileSize(file, nullptr), read = 0;
    const bool complete = length >= 16 && length <= protectedBytes.size() &&
        ReadFile(file, protectedBytes.data(), length, &read, nullptr) && read == length;
    CloseHandle(file);
    if (!complete) return false;
    DATA_BLOB input{length, protectedBytes.data()}, output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
    constexpr std::uint32_t magic = 0x31494b48; // HKI1
    std::uint32_t fileMagic = 0;
    if (output.cbData >= sizeof(fileMagic)) std::memcpy(&fileMagic, output.pbData, sizeof(fileMagic));
    bool valid = output.cbData == 8 + key.blob.size() && fileMagic == magic;
    std::uint32_t version = 0;
    if (valid) {
        std::memcpy(&version, output.pbData + 4, 4);
        valid = version == 1;
        if (valid) std::memcpy(key.blob.data(), output.pbData + 8, key.blob.size());
    }
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData);
    Algorithm algorithm; Key imported;
    return valid && importPrivate(key, algorithm, imported);
}
bool saveNewKey(const wchar_t* filename, const PrivateKey& key) {
    std::array<std::uint8_t, 112> plain{};
    const std::uint32_t magic = 0x31494b48, version = 1;
    std::memcpy(plain.data(), &magic, 4); std::memcpy(plain.data() + 4, &version, 4);
    std::memcpy(plain.data() + 8, key.blob.data(), key.blob.size());
    DATA_BLOB input{DWORD(plain.size()), plain.data()}, output{};
    const bool protectedOk = CryptProtectData(&input, L"cod4x1khz local identity", nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &output) != FALSE;
    SecureZeroMemory(plain.data(), plain.size());
    if (!protectedOk) return false;
    wchar_t temporary[32768]{};
    static LONG serial = 0;
    const int pathLength = _snwprintf_s(temporary, _countof(temporary), _TRUNCATE, L"%s.tmp.%lu.%ld",
        filename, GetCurrentProcessId(), InterlockedIncrement(&serial));
    bool written = false;
    if (pathLength > 0) {
        HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD count = 0;
            written = WriteFile(file, output.pbData, output.cbData, &count, nullptr) && count == output.cbData && FlushFileBuffers(file);
            CloseHandle(file);
            // No replacement: a concurrent creator wins, and its key is loaded.
            if (written) written = MoveFileExW(temporary, filename, MOVEFILE_WRITE_THROUGH) != FALSE;
            if (!written) DeleteFileW(temporary);
        }
    }
    LocalFree(output.pbData);
    return written;
}
std::string_view trim(std::string_view text) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    return text;
}
bool number(std::string_view text, std::uint64_t maximum, std::uint64_t& value) {
    value = 0; if (text.empty()) return false;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const auto digit = unsigned(c - '0');
        if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10)) return false;
        value = value * 10 + unsigned(c - '0');
    }
    return value <= maximum;
}
// Only legacy/Steam3 numeric fields accept the pinned scanf-leading grammar.
// The existing strict number() helper and unrelated protocol parsers stay intact.
bool fieldNumber(std::string_view text, std::uint64_t maximum, std::uint64_t& value) {
    while (!text.empty()) {
        const char c = text.front();
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\v' && c != '\f') break;
        text.remove_prefix(1);
    }
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    return number(text, maximum, value);
}

unsigned accountType(char c) {
    switch (c) {
    case 'U': return 1; case 'M': return 2; case 'G': return 3; case 'A': return 4;
    case 'P': return 5; case 'C': return 6; case 'g': return 7;
    case 'T': case 'c': case 'L': return 8; case 'a': return 10; default: return 0;
    }
}
bool validId(std::uint64_t id) {
    const unsigned universe = unsigned(id >> 56), type = unsigned((id >> 52) & 15);
    return (universe >= 1 && (universe <= 4 || (universe >= 32 && universe < 40))) && type >= 1 && type <= 10;
}
bool fields(std::string_view text, std::array<std::string_view, 4>& result, unsigned& count) {
    count = 0;
    while (true) {
        if (count == result.size()) return false;
        auto split = text.find(':'); result[count++] = text.substr(0, split);
        if (split == std::string_view::npos) return true;
        text.remove_prefix(split + 1);
    }
}
constexpr std::uint8_t guidSalt[] = {
    0x83,0xf3,0x65,0x90,0x6c,0x75,0x33,0xe1,0x81,0xaf,0xc1,0x44,0xe1,0x3e,0xf0,0x66,
    0x77,0xec,0x0e,0x62,0x6a,0xe2,0xf3,0xba,0x58,0x1a,0x72,0x62,0x2a,0xc4,0xcc,0x61,
    0x49,0x4b,0xf5,0xb9,0x49,0xea,0x2b,0xdf,0x0f,0x99,0xe2,0x8e,0xb8,0x7e,0x50,0x63,
    0xc6,0xd5,0x44,0xdd,0x0b,0x56,0x96,0x19,0x94,0x76,0x98,0x90,0x17,0xb2,0x66,0xfa,
    0x23,0x7c,0xcd,0x31,0x49,0x4b,0x85,0x31,0xc2,0xfe,0x86,0x3d,0x80,0xcd,0xe5,0x3a,
    0xe9,0x43,0xc3,0x7c,0x19,0xc3,0x9c,0xbe,0x19,0x33,0x5c,0x22,0x34,0x16,0xc9,0xc2,
    0xce,0xd2,0x25,0x2d,0x5f,0x2e,0x32,0x81,0x97,0xcf,0x14,0x96,0x6b,0x15,0x59,0xce,
    0xbd,0x36,0xdb,0xbc,0x23,0x16,0x74,0x68,0xa1,0x4f,0x0c,0x46,0xbc,0x1e,0x19,0x12,
    0x8a,0x86,0x16,0x2d,0xe9,0x3f,0x22,0x49,0x0d,0xaa,0x6e,0x15,0x47,0xe8,0x19,0x17,
    0xa2,0xf1,0xcc,0xca,0x49,0x38,0xa0,0xa7,0xb3,0xcc,0x92,0xb5,0x7c,0x0c,0xd4,0x25,
    0x6a,0x3e,0x55,0xc4,0x72,0x39,0x81,0x22,0x0d,0x1a,0xc7,0x1e,0xf4,0x96,0xe4,0xc6,
    0x6d,0x6c,0x43,0x81,0xcf,0x64,0x49,0xa1,0x10,0x73,0x46,0x7a,0x05,0xdb,0xdb,0xd4,
    0x94,0x3f,0x04,0x52,0xd8,0x23,0x9d,0x85,0x59,0x3a,0x29,0xfc,0xfc,0xd5,0x06,0xc9,
    0xdf,0x58,0xc3,0x7e,0x49,0xe4,0xbe,0x79,0x37,0x97,0x51,0xc2,0xf0,0x0c,0x38,0x6f,
    0x96,0x58,0xaf,0xb0,0x43,0x10,0x41,0x57,0xef,0xc6,0xfa,0x40,0x6f,0xa4,0xc5,0xc6,
    0x67,0x08,0xd3,0x65,0x0f,0xb0,0xa4,0x4d,0x54,0xe4,0x87,0xc7,0x0f,0x52,0xe2,0xc6
};
// GUID-local CNG adapter. Shared Algorithm/P256/SHA256 callers do not use it.
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
enum class GuidFault { None, Open, Derive };
struct GuidProbeState {
    GuidFault fault = GuidFault::None;
    unsigned openAttempts = 0, opened = 0, deriveAttempts = 0, derived = 0;
    unsigned closeAttempts = 0, closed = 0, closeErrors = 0;
};
thread_local GuidProbeState* guidProbeState = nullptr;
struct GuidProbeScope {
    GuidProbeState* previous;
    explicit GuidProbeScope(GuidProbeState& state) : previous(guidProbeState) { guidProbeState = &state; }
    ~GuidProbeScope() { guidProbeState = previous; }
    GuidProbeScope(const GuidProbeScope&) = delete;
    GuidProbeScope& operator=(const GuidProbeScope&) = delete;
};
#endif

struct GuidAlgorithm {
    BCRYPT_ALG_HANDLE handle = nullptr;
    ~GuidAlgorithm() {
        if (!handle) return;
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
        if (guidProbeState) ++guidProbeState->closeAttempts;
#endif
        const NTSTATUS status = BCryptCloseAlgorithmProvider(handle, 0);
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
        if (guidProbeState) {
            if (status >= 0) ++guidProbeState->closed;
            else ++guidProbeState->closeErrors;
        }
#else
        (void)status;
#endif
    }
};

NTSTATUS guidOpenAlgorithm(GuidAlgorithm& algorithm) {
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
    if (guidProbeState) {
        ++guidProbeState->openAttempts;
        if (guidProbeState->fault == GuidFault::Open) return static_cast<NTSTATUS>(0xc0000001UL);
    }
#endif
    const NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm.handle,
        BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
    if (guidProbeState && status >= 0) ++guidProbeState->opened;
#endif
    return status;
}

NTSTATUS guidDeriveKey(GuidAlgorithm& algorithm, std::array<std::uint8_t, 16>& decoded,
                       std::array<std::uint8_t, 4>& derived) {
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
    if (guidProbeState) {
        ++guidProbeState->deriveAttempts;
        if (guidProbeState->fault == GuidFault::Derive) return static_cast<NTSTATUS>(0xc0000001UL);
    }
#endif
    const NTSTATUS status = BCryptDeriveKeyPBKDF2(algorithm.handle, decoded.data(),
        ULONG(decoded.size()), const_cast<PUCHAR>(guidSalt), sizeof(guidSalt), 100,
        derived.data(), ULONG(derived.size()), 0);
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
    if (guidProbeState && status >= 0) ++guidProbeState->derived;
#endif
    return status;
}

}
PrivateKey::~PrivateKey() { SecureZeroMemory(blob.data(), blob.size()); }
bool RandomBytes(void* bytes, std::size_t length) {
    return bytes && length && length <= ULONG_MAX && BCryptGenRandom(nullptr, static_cast<PUCHAR>(bytes), ULONG(length), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}
bool Sha256(const void* bytes, std::size_t length, Fingerprint& result) {
    result = {};
    if ((!bytes && length) || length > ULONG_MAX) return false;
    Algorithm algorithm;
    if (BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    std::array<std::uint8_t, 512> object{}; DWORD objectLength = 0, received = 0;
    if (BCryptGetProperty(algorithm.handle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &received, 0) < 0 || objectLength > object.size()) return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm.handle, &hash, object.data(), objectLength, nullptr, 0, 0) < 0) return false;
    const bool valid = BCryptHashData(hash, const_cast<PUCHAR>(static_cast<const std::uint8_t*>(bytes)), ULONG(length), 0) >= 0 &&
        BCryptFinishHash(hash, result.data(), ULONG(result.size()), 0) >= 0;
    BCryptDestroyHash(hash); return valid;
}
bool GenerateKey(PrivateKey& key) {
    key.blob = {}; Algorithm algorithm; Key generated; ULONG length = 0;
    return openP256(algorithm) && BCryptGenerateKeyPair(algorithm.handle, &generated.handle, 256, 0) >= 0 &&
        BCryptFinalizeKeyPair(generated.handle, 0) >= 0 &&
        BCryptExportKey(generated.handle, nullptr, BCRYPT_ECCPRIVATE_BLOB, key.blob.data(), ULONG(key.blob.size()), &length, 0) >= 0 && length == key.blob.size();
}
bool ExportPublicKey(const PrivateKey& key, PublicKey& result) {
    result = {}; Algorithm algorithm; Key imported; ULONG length = 0;
    return importPrivate(key, algorithm, imported) &&
        BCryptExportKey(imported.handle, nullptr, BCRYPT_ECCPUBLIC_BLOB, result.data(), ULONG(result.size()), &length, 0) >= 0 && length == result.size();
}
bool PublicKeyFingerprint(const PublicKey& key, Fingerprint& result) {
    Algorithm algorithm; Key imported;
    return importPublic(key, algorithm, imported) && Sha256(key.data(), key.size(), result);
}
bool Sign(const PrivateKey& key, const Challenge& challenge, Proof& proof) {
    proof = {}; Algorithm algorithm; Key imported; Fingerprint digest{}; ULONG length = 0;
    return importPrivate(key, algorithm, imported) && ExportPublicKey(key, proof.publicKey) && proofHash(challenge, digest) &&
        BCryptSignHash(imported.handle, nullptr, digest.data(), ULONG(digest.size()), proof.signature.data(), ULONG(proof.signature.size()), &length, 0) >= 0 && length == proof.signature.size();
}
bool Verify(const Proof& proof, const Challenge& challenge, Fingerprint& result) {
    result = {}; Algorithm algorithm; Key imported; Fingerprint digest{};
    return importPublic(proof.publicKey, algorithm, imported) && proofHash(challenge, digest) &&
        BCryptVerifySignature(imported.handle, nullptr, digest.data(), ULONG(digest.size()), const_cast<PUCHAR>(proof.signature.data()), ULONG(proof.signature.size()), 0) >= 0 &&
        Sha256(proof.publicKey.data(), proof.publicKey.size(), result);
}
bool EncodeHex(const void* bytes, std::size_t length, char* text, std::size_t capacity) {
    if (!bytes || !text || length > (SIZE_MAX - 1) / 2 || capacity < length * 2 + 1) return false;
    constexpr char digits[] = "0123456789abcdef"; auto* data = static_cast<const std::uint8_t*>(bytes);
    for (std::size_t i = 0; i != length; ++i) { text[i * 2] = digits[data[i] >> 4]; text[i * 2 + 1] = digits[data[i] & 15]; }
    text[length * 2] = 0; return true;
}
bool DecodeHex(std::string_view text, void* bytes, std::size_t length) {
    if (!bytes || length > SIZE_MAX / 2 || text.size() != length * 2) return false;
    auto* data = static_cast<std::uint8_t*>(bytes);
    for (std::size_t i = 0; i != length; ++i) {
        const int high = hexValue(text[i * 2]), low = hexValue(text[i * 2 + 1]);
        if (high < 0 || low < 0) { std::memset(bytes, 0, length); return false; }
        data[i] = std::uint8_t(high * 16 + low);
    }
    return true;
}
bool EncodeProof(const Proof& proof, char* text, std::size_t capacity) {
    if (!text || capacity <= ProofHexLength) return false;
    return EncodeHex(proof.publicKey.data(), proof.publicKey.size(), text, capacity) &&
        EncodeHex(proof.signature.data(), proof.signature.size(), text + proof.publicKey.size() * 2, capacity - proof.publicKey.size() * 2);
}
bool DecodeProof(std::string_view text, Proof& proof) {
    proof = {};
    return text.size() == ProofHexLength && DecodeHex(text.substr(0, 144), proof.publicKey.data(), proof.publicKey.size()) &&
        DecodeHex(text.substr(144), proof.signature.data(), proof.signature.size());
}
bool IsEmpty(const Fingerprint& value) { return std::all_of(value.begin(), value.end(), [](auto byte) { return byte == 0; }); }
std::uint64_t LocalPlayerId(const Fingerprint& fingerprint) {
    std::uint64_t result = 0; for (unsigned i = 0; i != 8; ++i) result |= std::uint64_t(fingerprint[i]) << (i * 8);
    return result ? result : 1; // Display alias only; permissions use all 256 bits.
}
bool LoadOrCreateKey(const wchar_t* filename, PrivateKey& key) {
    key.blob = {}; if (!filename || !*filename) return false;
    const DWORD attributes = GetFileAttributesW(filename);
    if (attributes != INVALID_FILE_ATTRIBUTES) return !(attributes & FILE_ATTRIBUTE_DIRECTORY) && readKey(filename, key);
    if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    if (!GenerateKey(key)) return false;
    if (saveNewKey(filename, key)) return true;
    key.blob = {}; return readKey(filename, key);
}
// Reviewed proposal: substitute StringToSteamId only after guarded source review.
// No official provider authentication follows from textual conversion.
std::uint64_t StringToSteamId(std::string_view text) {
    // Bound original input before removing spaces, avoiding pinned truncation.
    if (text.empty() || text.size() >= 128) return 0;
    text = trim(text); if (text.empty()) return 0;
    std::array<std::string_view, 4> part{}; unsigned count = 0;
    if (text.substr(0, 6) == "STEAM_") {
        if (!fields(text.substr(6), part, count) || count != 3) return 0;
        std::uint64_t instance, low, high;
        if (!fieldNumber(part[0], 1, instance) || !fieldNumber(part[1], 1, low) ||
            !fieldNumber(part[2], UINT32_MAX / 2, high)) return 0;
        return (1ULL << 56) | (1ULL << 52) | (instance << 32) | (high * 2 + low);
    }
    const bool bracketed = text.front() == '[';
    if (bracketed) {
        if (text.back() != ']') return 0;
        text.remove_prefix(1); text.remove_suffix(1);
    }
    if (text.find(':') != std::string_view::npos) {
        if (!fields(text, part, count) || (count != 3 && count != 4) || part[0].size() != 1) return 0;
        const unsigned type = accountType(part[0][0]);
        std::uint64_t universe, account, instance = (type == 1 || type == 3) ? 1 : 0;
        if (!type || !fieldNumber(part[1], 255, universe) ||
            !fieldNumber(part[2], UINT32_MAX, account) ||
            (count == 4 && !fieldNumber(part[3], 0xfffff, instance))) return 0;
        const auto id = (universe << 56) | (std::uint64_t(type) << 52) | (instance << 32) | account;
        return validId(id) ? id : 0;
    }
    // Pinned optional brackets describe Steam3 only, not decimal Steam64.
    if (bracketed) return 0;
    std::uint64_t id;
    return number(text, UINT64_MAX, id) && validId(id) ? id : 0;
}

bool SteamIdToString(std::uint64_t id, bool steam64, char* text, std::size_t capacity) {
    if (!text || !capacity) return false;
    int length;
    if (steam64) length = std::snprintf(text, capacity, "%llu", id);
    else if (!validId(id)) length = std::snprintf(text, capacity, "[I:0:0]");
    else {
        constexpr char types[] = "IUMGAPCgT a";
        length = std::snprintf(text, capacity, "[%c:%u:%u]", types[(id >> 52) & 15], unsigned(id >> 56), unsigned(id & UINT32_MAX));
    }
    return length >= 0 && std::size_t(length) < capacity;
}
bool SteamIdIndividual(std::uint64_t id, bool steamOnly) {
    const unsigned universe = unsigned(id >> 56);
    return ((universe == 1) || (!steamOnly && universe >= 32 && universe < 40)) && ((id >> 52) & 15) == 1 && ((id >> 32) & 0xfffff) == 1;
}
std::uint64_t GuidToPlayerId(std::string_view guid) {
    std::array<std::uint8_t, 16> decoded{};
    if (!DecodeHex(guid, decoded.data(), decoded.size())) return 0;
    GuidAlgorithm algorithm; std::array<std::uint8_t, 4> derived{};
    if (guidOpenAlgorithm(algorithm) < 0 || guidDeriveKey(algorithm, decoded, derived) < 0) return 0;
    std::uint32_t account = 0; for (unsigned i = 0; i != 4; ++i) account |= std::uint32_t(derived[i]) << (8 * i);
    return (32ULL << 56) | (1ULL << 52) | (1ULL << 32) | account;
}
}

#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(COD4X1KHZ_IDENTITY_PROBE)
namespace khz::identity {
namespace {
cmd_function_s guidProbeCommand{};

bool guidProbeUnrelatedCrypto() {
    constexpr std::array<std::uint8_t, 32> expected = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    Fingerprint digest{};
    if (!Sha256("abc", 3, digest) || digest != expected) return false;
    PrivateKey key; PublicKey publicKey{}; Fingerprint fingerprint{}, verified{};
    Challenge challenge{}; Proof proof{};
    challenge.challenge = 1; challenge.protocol = 10002; challenge.qport = 28982;
    challenge.boot.fill(0x31); challenge.nonce.fill(0x72);
    if (!GenerateKey(key) || !ExportPublicKey(key, publicKey) ||
        !PublicKeyFingerprint(publicKey, fingerprint) || !Sign(key, challenge, proof) ||
        proof.publicKey != publicKey || !Verify(proof, challenge, verified) || verified != fingerprint) return false;
    ++challenge.nonce[0];
    return !Verify(proof, challenge, verified) && IsEmpty(verified);
}

void guidProbeRun() {
    // Gate before installing TLS or invoking any cryptographic operation.
    if (!Sys_IsMainThread() || com_inServerFrame || Cmd_Argc() != 1 ||
        !Dvar_GetBool("khz_benchmark") || !Dvar_GetBool("khz_test_controls") || guidProbeState) {
        Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_IDENTITY_GUID_PROVIDER rejected=1\n");
        return;
    }
    unsigned cases = 0, failures = 0, cryptoIsolationCases = 0;
    unsigned openAttempts = 0, opened = 0, deriveAttempts = 0, derived = 0;
    unsigned closeAttempts = 0, closed = 0, closeErrors = 0;
    constexpr char input[] = "0123456789abcdef0123456789abcdef";
    constexpr std::uint64_t golden = 2310346614158149229ULL;
    // All cryptographic calls and their RAII cleanup finish before printing.
    for (unsigned phase = 0; phase != 4; ++phase) {
        GuidProbeState state{};
        state.fault = phase == 1 ? GuidFault::Open : phase == 2 ? GuidFault::Derive : GuidFault::None;
        std::uint64_t converted = 0;
        bool isolated = true;
        {
            GuidProbeScope scope(state);
            if (phase == 1 || phase == 2) {
                isolated = guidProbeUnrelatedCrypto();
                ++cryptoIsolationCases;
                // The unrelated real crypto may not consume GUID counters.
                isolated = isolated && !state.openAttempts && !state.deriveAttempts &&
                    !state.opened && !state.derived && !state.closeAttempts && !state.closed;
            }
            converted = GuidToPlayerId(input);
        }
        const bool openFailure = phase == 1, deriveFailure = phase == 2;
        ++cases;
        if (!isolated || converted != (openFailure || deriveFailure ? 0 : golden) ||
            state.openAttempts != 1 || state.opened != (openFailure ? 0u : 1u) ||
            state.deriveAttempts != (openFailure ? 0u : 1u) ||
            state.derived != (openFailure || deriveFailure ? 0u : 1u) ||
            state.closeAttempts != state.opened || state.closed != state.opened ||
            state.closeErrors || guidProbeState) ++failures;
        openAttempts += state.openAttempts; opened += state.opened;
        deriveAttempts += state.deriveAttempts; derived += state.derived;
        closeAttempts += state.closeAttempts; closed += state.closed; closeErrors += state.closeErrors;
    }
    Com_Printf(CON_CHANNEL_DONT_FILTER,
        "KHZ_IDENTITY_GUID_PROVIDER cases=%u failures=%u open_failure=1 derive_failure=1 "
        "normal_before=1 normal_after=1 isolation_cases=%u open_attempts=%u opened=%u "
        "derive_attempts=%u derived=%u close_attempts=%u closed=%u close_errors=%u "
        "live_handles=%u scope_reset=%u provider_authentication=0\n",
        cases, failures, cryptoIsolationCases, openAttempts, opened, deriveAttempts,
        derived, closeAttempts, closed, closeErrors, opened - closed, guidProbeState ? 0u : 1u);
}
}
}

bool KHZ_InitIdentityProbe() {
    if (!Sys_IsMainThread()) return false;
    static bool installed = false;
    if (!installed) {
        Cmd_AddCommandInternal("khz_identity_guid_provider", khz::identity::guidProbeRun,
            &khz::identity::guidProbeCommand);
        installed = true;
    }
    return true;
}
#endif
