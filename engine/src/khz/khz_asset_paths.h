#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace khz::assets {
struct Fastfile {
    std::filesystem::path physical;
    std::string relative;
};
// Lifecycle/DB work only. Native asset readers and advertised download names
// use this same selection; no mutable engine pointers enter service workers.
bool ModFastfile(const std::filesystem::path& home, const std::filesystem::path& base,
    std::string_view gameDirectory, std::string_view zone, Fastfile* result);
bool MapFastfile(const std::filesystem::path& home, const std::filesystem::path& base,
    std::string_view activeMap, std::string_view zone, Fastfile* result);
bool CustomReference(std::string_view reference, std::string_view gameDirectory, std::string_view activeMap);
bool ReferencedFastfile(const std::filesystem::path& home, const std::filesystem::path& base,
    std::string_view gameDirectory, std::string_view activeMap, std::string_view reference, Fastfile* result);
}
