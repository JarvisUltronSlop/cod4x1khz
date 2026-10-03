#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Local proof of key ownership does not assert a Steam, CoD4x, HWID or
// PunkBuster identity. Provider identifiers must be supplied by verified
// providers separately; parsing an identifier never authenticates it.
namespace khz::identity {
// The native game stores/transmits challenges as signed int while the proof
// codec uses uint32. Keep their wire intersection nonzero and positive.
constexpr std::uint32_t GameChallengeNumber(std::uint32_t random) {
    return (random & 0x7fffffffu) ? (random & 0x7fffffffu) : 1u;
}
using Fingerprint = std::array<std::uint8_t, 32>;
using PublicKey = std::array<std::uint8_t, 72>; // CNG P-256 public blob.
struct PrivateKey {
    std::array<std::uint8_t, 104> blob{}; // CNG P-256 private blob.
    ~PrivateKey();
};
struct Challenge {
    std::array<std::uint8_t, 32> boot{};
    std::array<std::uint8_t, 32> nonce{};
    std::uint32_t challenge = 0;
    std::uint32_t protocol = 0;
    std::uint16_t qport = 0;
};
struct Proof {
    PublicKey publicKey{};
    std::array<std::uint8_t, 64> signature{}; // CNG ECDSA r || s.
};
inline constexpr unsigned ProofHexLength = (72 + 64) * 2;

bool RandomBytes(void* bytes, std::size_t length);
bool Sha256(const void* bytes, std::size_t length, Fingerprint& result);
bool GenerateKey(PrivateKey& key);
bool ExportPublicKey(const PrivateKey& key, PublicKey& result);
bool PublicKeyFingerprint(const PublicKey& key, Fingerprint& result);
bool Sign(const PrivateKey& key, const Challenge& challenge, Proof& proof);
bool Verify(const Proof& proof, const Challenge& challenge, Fingerprint& result);
bool EncodeHex(const void* bytes, std::size_t length, char* text, std::size_t capacity);
bool DecodeHex(std::string_view text, void* bytes, std::size_t length);
bool EncodeProof(const Proof& proof, char* text, std::size_t capacity);
bool DecodeProof(std::string_view text, Proof& proof);
bool IsEmpty(const Fingerprint& value);
std::uint64_t LocalPlayerId(const Fingerprint& fingerprint);

// Caller supplies a path beneath the project home. Corrupt existing keys fail
// closed instead of silently replacing the player's persistent identity.
// Persisted private blobs are protected with current-user Windows DPAPI.
bool LoadOrCreateKey(const wchar_t* filename, PrivateKey& key);

// Public CoD4x identifier conversions, based on pinned src/sapi.c. These are
// conversions only, including its legacy Steam universe/instance convention.
std::uint64_t StringToSteamId(std::string_view text);
bool SteamIdToString(std::uint64_t id, bool steam64, char* text, std::size_t capacity);
bool SteamIdIndividual(std::uint64_t id, bool steamOnly = false);
std::uint64_t GuidToPlayerId(std::string_view guid);
}
