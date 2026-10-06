#pragma once
#include "host_protocol.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace khz::hosting {
struct RotationEntry {
    std::string map, mode;
};
struct Setting {
    std::string key, label, value;
};
struct Profile {
    std::string id, name = "My server", hostname = "cod4x1khz server", password;
    NetworkMode network = NetworkMode::Localhost;
    std::string bind = "127.0.0.1", advertised;
    unsigned port = 28960, maxClients = 16, bots = 15, difficulty = 1;
    bool shuffle = false, mixed = false, automaticNetwork = false;
    std::string modePool = "dm war sd sab dom koth", extra;
    std::vector<RotationEntry> rotation{{"mp_killhouse", "dm"}};
    std::map<std::string, std::string> rules;
    Profile();
};
std::vector<Setting> Settings();
const std::vector<std::pair<std::string, std::string>> &Modes();
bool Identifier(std::string_view);
bool PublicIPv4(std::string_view);
bool Validate(const Profile &, std::string &error);
bool ValidateExtra(std::string_view, std::string &error);
std::string Configuration(const Profile &);
bool ImportConfiguration(Profile &, std::string_view, std::string &error);
bool SaveProfile(const std::filesystem::path &root, const Profile &, std::string &error);
bool LoadProfile(const std::filesystem::path &file, Profile &, std::string &error);
std::vector<std::string> InstalledMaps(const std::filesystem::path &root);
std::filesystem::path ProfileHome(const std::filesystem::path &root, const Profile &);
std::wstring StartArguments(const Profile &);
Packet ConfigurationPacket(const Profile &);
} // namespace khz::hosting
