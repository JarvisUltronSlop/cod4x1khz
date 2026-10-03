#include "khz_asset_paths.h"
#include "khz_content.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace khz::assets {
namespace {
bool select(const std::filesystem::path& home, const std::filesystem::path& base,
    const std::string& relative, Fastfile* result) {
    if (!result || !content::SafeAssetPath(relative)) return false;
    for (const auto& root : {home, base}) {
        if (root.empty()) continue;
        std::filesystem::path physical;
        if (!content::ConfinedPath(root, relative, &physical)) continue;
        const DWORD flags = GetFileAttributesW(physical.c_str());
        if (flags == INVALID_FILE_ATTRIBUTES || (flags & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
        *result = {std::move(physical), relative};
        return true;
    }
    return false;
}
}
bool ModFastfile(const std::filesystem::path& home, const std::filesystem::path& base,
    std::string_view gameDirectory, std::string_view zone, Fastfile* result) {
    if (zone != "mod") return false;
    return select(home, base, std::string(gameDirectory) + "/mod.ff", result);
}
bool MapFastfile(const std::filesystem::path& home, const std::filesystem::path& base,
    std::string_view activeMap, std::string_view zone, Fastfile* result) {
    if (activeMap.empty() || activeMap.size() >= 64 ||
        (zone != activeMap && zone != std::string(activeMap) + "_load")) return false;
    return select(home, base, "usermaps/" + std::string(activeMap) + "/" + std::string(zone) + ".ff", result);
}
bool CustomReference(std::string_view reference, std::string_view gameDirectory, std::string_view activeMap) {
    const std::string relative = std::string(reference) + ".ff";
    if (!content::SafeAssetPath(relative)) return false;
    if (reference == std::string(gameDirectory) + "/mod") return true;
    if (activeMap.empty() || activeMap.size() >= 64) return false;
    const auto prefix = "usermaps/" + std::string(activeMap) + "/" + std::string(activeMap);
    return reference == prefix || reference == prefix + "_load";
}
bool ReferencedFastfile(const std::filesystem::path& home, const std::filesystem::path& base,
    std::string_view gameDirectory, std::string_view activeMap, std::string_view reference, Fastfile* result) {
    return CustomReference(reference, gameDirectory, activeMap) && select(home, base, std::string(reference) + ".ff", result);
}
}
