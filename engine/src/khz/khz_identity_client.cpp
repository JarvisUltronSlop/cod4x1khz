#include "khz_identity_client.h"
#include "khz_identity.h"
#include "khz_paths.h"
#include <core.hpp>
#include <charconv>
#include <cstring>
#include <filesystem>

namespace {
khz::identity::PrivateKey clientKey;
char encodedProof[khz::identity::ProofHexLength + 1]{};
bool keyReady = false;
bool loadKey() {
    if (keyReady) return true;
    std::error_code error;
    const auto directory = std::filesystem::path(KHZ_HomeRoot()) / "identity";
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    keyReady = khz::identity::LoadOrCreateKey((directory / "client-key.dpapi").c_str(), clientKey);
    return keyReady;
}
}
void KHZ_ClientResetIdentityChallenge() { std::memset(encodedProof, 0, sizeof(encodedProof)); }
bool KHZ_ClientAcceptIdentityChallenge(const char* text, const char* boot, const char* nonce, unsigned qport) {
    KHZ_ClientResetIdentityChallenge();
    if (!text || !*text || !boot || !nonce || !qport || qport > 65535) return false;
    khz::identity::Challenge challenge;
    const auto end = text + std::strlen(text);
    const auto converted = std::from_chars(text, end, challenge.challenge);
    if (converted.ec != std::errc{} || converted.ptr != end || !challenge.challenge ||
        !khz::identity::DecodeHex(boot, challenge.boot.data(), challenge.boot.size()) ||
        !khz::identity::DecodeHex(nonce, challenge.nonce.data(), challenge.nonce.size()) || !loadKey()) return false;
    challenge.protocol = khz::Protocol; challenge.qport = static_cast<unsigned short>(qport);
    khz::identity::Proof proof;
    return khz::identity::Sign(clientKey, challenge, proof) &&
        khz::identity::EncodeProof(proof, encodedProof, sizeof(encodedProof));
}
bool KHZ_ClientIdentityProof(char* output, std::size_t capacity) {
    if (!output || capacity < sizeof(encodedProof) || !encodedProof[0]) return false;
    std::memcpy(output, encodedProof, sizeof(encodedProof)); return true;
}
