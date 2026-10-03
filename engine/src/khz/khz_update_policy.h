#pragma once
#include "khz_update.h"

namespace khz::update {
inline constexpr const char* PolicyRelativePath = "config/update-policy.txt";
inline constexpr unsigned MaximumPolicyBytes = 4096;
bool ParsePolicy(std::string_view text, bool allowPrivateFixture, Policy* result, std::string* reason = nullptr);
bool LoadPolicy(const std::filesystem::path& projectRoot, bool allowPrivateFixture,
    Policy* result, std::string* reason = nullptr);
bool FormatPolicy(const Policy& policy, std::string* result);
bool ReadInstalledVersion(const std::filesystem::path& projectRoot, std::uint64_t* result);
}
