#pragma once
#include <cstddef>

// Connection proof is created outside the active gameplay clock. It proves
// possession of this project's key, without claiming an external identity.
bool KHZ_ClientAcceptIdentityChallenge(const char* challenge, const char* boot,
    const char* nonce, unsigned qport);
bool KHZ_ClientIdentityProof(char* output, std::size_t capacity);
void KHZ_ClientResetIdentityChallenge();
