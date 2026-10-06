#pragma once
#include <filesystem>
#include <functional>
#include <atomic>
#include <string>
#include <vector>
namespace khz::setup {
struct Entry {
    std::filesystem::path source, relative;
    std::uint64_t bytes = 0;
};
struct Plan {
    std::filesystem::path game, destination;
    bool repair = false;
    std::uint64_t bytes = 0;
    std::vector<Entry> assets;
};
using Progress = std::function<void(unsigned, const std::string &)>;
bool Inspect(const std::filesystem::path &game, const std::filesystem::path &destination, Plan &,
             std::string &error);
bool Install(const Plan &, std::atomic_bool &cancel, Progress, std::string &error);
std::filesystem::path DetectSteam();
bool SafeRelative(std::string_view);
std::string HashFile(const std::filesystem::path &);
} // namespace khz::setup
