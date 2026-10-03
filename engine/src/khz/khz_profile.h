#pragma once

#include <cstdint>

// Profile features execute on the existing client owner, outside steady ticks.
void KHZ_InitProfileFeatures();
void KHZ_ProfileReset();
void KHZ_ProfileReloadEntitlements();
bool KHZ_ProfileHasAllUnlocks();
std::uint32_t KHZ_ProfileUnlockMask(int statIndex);
bool KHZ_ProfileSelfTest();
const char* KHZ_ProfileStatusJSON();
// The connected handler owns transport/quiescing; the transaction owns backup,
// validation and persistence. A successful commit changes no combat counters.
bool KHZ_ProfileCommitAllUnlocks();
void KHZ_ProfileSetConnectedUnlockHandler(bool (*handler)());

// The same projection bound is used for the camera and its hip-fire sensitivity
// reference. ADS still blends the stock weapon FOV before applying this scale.
inline float KHZ_ScaledFov(float degrees, float scale, float minimum)
{
    const float scaled = degrees * scale;
    const float boundedMin = minimum < 1.0f ? 1.0f : (minimum > 160.0f ? 160.0f : minimum);
    return scaled < boundedMin ? boundedMin : (scaled > 160.0f ? 160.0f : scaled);
}
