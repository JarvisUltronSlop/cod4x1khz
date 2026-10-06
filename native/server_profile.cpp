#include "server_profile.hpp"
#include "host_transport.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstring>
#include <charconv>
#include <cmath>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace khz::hosting {
namespace {
std::vector<std::string> Tokens(std::string_view line) {
    std::vector<std::string> out;
    std::string token;
    bool quote = false, have = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char ch = line[i];
        if (!quote && ch == '/' && i + 1 < line.size() && line[i + 1] == '/')
            break;
        if (ch == '"') {
            quote = !quote;
            have = true;
        } else if (!quote && (ch == ' ' || ch == '\t' || ch == '\r')) {
            if (have) {
                out.push_back(token);
                token.clear();
                have = false;
            }
        } else {
            token += ch;
            have = true;
        }
    }
    if (quote)
        return {};
    if (have)
        out.push_back(token);
    return out;
}
std::string lower(std::string text) {
    for (auto &ch : text)
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}
bool protectedKey(std::string key) {
    key = lower(key);
    for (auto prefix : {"khz_host", "khz_benchmark", "khz_test", "khz_serverprofile"})
        if (key.starts_with(prefix))
            return true;
    for (auto name : {"sv_fps", "timescale", "com_fixedtime", "com_timescale", "protocol", "net_ip",
                      "net_port", "dedicated", "fs_homepath", "fs_basepath", "fs_game", "khz_bots"})
        if (key == name)
            return true;
    return false;
}
bool value(std::string_view text) {
    return text.size() <= 256 && text.find_first_of("\r\n\";\\") == text.npos &&
           text.find('\0') == text.npos;
}
bool number(std::string_view text, unsigned maximum) {
    if (text.empty() || text.size() > 9)
        return false;
    unsigned n = 0;
    for (auto ch : text) {
        if (ch < '0' || ch > '9')
            return false;
        n = n * 10 + ch - '0';
        if (n > maximum)
            return false;
    }
    return true;
}
bool decimal(std::string_view text, double maximum) {
    if (text.empty() || text.size() > 16 || text.find_first_not_of("0123456789.") != text.npos)
        return false;
    double result = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
           std::isfinite(result) && result >= 0 && result <= maximum;
}
bool atomMode(const std::string &text) {
    for (auto &mode : Modes())
        if (mode.first == text)
            return true;
    return false;
}
void write(std::ostream &out, const char *key, const std::string &text) {
    out << key << '=' << text << '\n';
}
bool atomicWrite(const std::filesystem::path &path, const std::string &text, std::string &error) {
    auto temporary = path;
    temporary += L".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (!out || !MoveFileExW(temporary.c_str(), path.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Cannot save profile/configuration";
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}
} // namespace
const std::vector<std::pair<std::string, std::string>> &Modes() {
    static const std::vector<std::pair<std::string, std::string>> modes{
        {"dm", "Free-for-all"}, {"war", "Team Deathmatch"}, {"sd", "Search and Destroy"},
        {"sab", "Sabotage"},    {"dom", "Domination"},      {"koth", "Headquarters"}};
    return modes;
}
std::vector<Setting> Settings() {
    std::vector<Setting> result{
        {"g_allowvote", "Allow voting", "1"},
        {"sv_voice", "Voice chat", "1"},
        {"sv_minPing", "Minimum ping", "0"},
        {"sv_maxPing", "Maximum ping (0 = unlimited)", "0"},
        {"scr_teambalance", "Team balance", "1"},
        {"scr_oldschool", "Old school", "0"},
        {"scr_hardcore", "Hardcore", "0"},
        {"scr_team_fftype", "Friendly fire (0 off, 1 on, 2 reflect, 3 shared)", "0"},
        {"scr_game_spectatetype", "Spectator rules", "1"},
        {"scr_game_allowkillcam", "Killcam", "1"},
        {"scr_game_onlyheadshots", "Headshots only", "0"},
        {"scr_player_maxhealth", "Maximum health", "100"},
        {"scr_player_healthregentime", "Health regeneration delay", "5"},
        {"scr_player_suicidespawndelay", "Suicide respawn delay", "0"},
        {"scr_player_forcerespawn", "Force respawn", "1"},
        {"scr_player_sprinttime", "Sprint time", "4"},
        {"scr_team_teamkillspawndelay", "Teamkill spawn delay", "20"},
        {"scr_team_kickteamkillers", "Kick teamkillers", "0"},
        {"scr_team_teamkillpointloss", "Teamkill point loss", "1"},
        {"scr_team_teamkillpunish", "Punish teamkill", "0"},
        {"sv_pure", "Pure server", "1"},
        {"rcon_password", "RCON password (empty disables)", ""},
        {"sv_privateClients", "Reserved slots", "0"},
        {"sv_privatePassword", "Reserved-slot password", ""},
        {"sv_webadmin", "Private web admin enabled", "0"},
        {"khz_webadmin_bind", "Private web admin bind", "127.0.0.1"},
        {"khz_webadmin_port", "Private web admin TCP port", "28980"},
        {"sv_banappealurl", "Ban appeal URL", ""},
        {"banlist_maxipbantime", "Maximum transient IP ban minutes", "240"}};
    for (auto &mode : Modes()) {
        auto add = [&](const char *suffix, const char *label, const char *initial) {
            result.push_back(
                {"scr_" + mode.first + "_" + suffix, mode.second + ": " + label, initial});
        };
        add("timelimit", "Time limit (minutes)", mode.first == "sd" ? "2.5" : "10");
        const char *score = mode.first == "dm"     ? "150"
                            : mode.first == "war"  ? "750"
                            : mode.first == "dom"  ? "200"
                            : mode.first == "koth" ? "250"
                                                   : "1";
        add("scorelimit", "Score limit", score);
        add("playerrespawndelay", "Player respawn delay", "0");
        add("waverespawndelay", "Wave respawn delay", "0");
        if (mode.first == "sd" || mode.first == "sab") {
            add("bombtimer", "Bomb timer", "45");
            add("planttime", "Plant time", "5");
            add("defusetime", "Defuse time", "5");
        }
        if (mode.first == "sd") {
            add("roundlimit", "Round limit", "0");
            add("winlimit", "Win limit", "4");
            add("roundswitch", "Rounds before team switch", "3");
            add("numlives", "Lives", "1");
        }
        if (mode.first == "dom")
            add("capturelimit", "Capture limit", "200");
    }
    return result;
}
Profile::Profile() {
    for (auto &rule : Settings())
        rules[rule.key] = rule.value;
}
bool Identifier(std::string_view text) {
    return !text.empty() && text.size() < 64 &&
           text.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == text.npos;
}
bool PublicIPv4(std::string_view text) {
    IN_ADDR address{};
    if (InetPtonA(AF_INET, std::string(text).c_str(), &address) != 1)
        return false;
    auto a = address.S_un.S_un_b.s_b1, b = address.S_un.S_un_b.s_b2;
    return a != 0 && a != 10 && a != 127 && a < 224 && !(a == 169 && b == 254) &&
           !(a == 172 && b >= 16 && b <= 31) && !(a == 192 && b == 168) &&
           !(a == 100 && b >= 64 && b <= 127);
}
bool ValidateExtra(std::string_view text, std::string &error) {
    if (text.size() > 65536 || text.find('\0') != text.npos) {
        error = "Extra config is too large or contains NUL";
        return false;
    }
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
        auto trimmed = line.substr(line.find_first_not_of(" \t\r") == line.npos
                                       ? line.size()
                                       : line.find_first_not_of(" \t\r"));
        if (trimmed.empty() || trimmed.starts_with("//") || trimmed.starts_with("#"))
            continue;
        auto words = Tokens(trimmed);
        if (words.empty() || line.find(';') != line.npos || line.find('\\') != line.npos) {
            error = "Extra config requires one quoted command per line";
            return false;
        }
        auto verb = lower(words[0]);
        if (verb == "set" || verb == "seta" || verb == "sets") {
            if (words.size() != 3 || !Identifier(words[1]) || protectedKey(words[1]) ||
                !value(words[2])) {
                error =
                    "Invalid or protected config setting: " + (words.size() > 1 ? words[1] : line);
                return false;
            }
        } else if (verb == "loadplugin") {
            if (words.size() != 2 || !Identifier(words[1])) {
                error = "Use loadPlugin with an installed plugin name";
                return false;
            }
        } else {
            error = "Unsupported extra-config command: " + words[0] +
                    " (exec and startup overrides are blocked)";
            return false;
        }
    }
    return true;
}
bool Validate(const Profile &p, std::string &error) {
    if (p.id.size() != 32 || p.id.find_first_not_of("0123456789abcdef") != p.id.npos) {
        error = "Invalid stable profile ID";
        return false;
    }
    if (!value(p.name) || p.name.empty() || !value(p.hostname) || !value(p.password) ||
        !value(p.advertised)) {
        error = "Names/passwords cannot contain quotes, separators or newlines";
        return false;
    }
    IN_ADDR address{};
    if (InetPtonA(AF_INET, p.bind.c_str(), &address) != 1 || p.port < 1024 || p.port > 65535 ||
        unsigned(p.network) > 2 || p.maxClients < 1 || p.maxClients > 16 ||
        p.bots >= p.maxClients || p.bots > 15 || p.difficulty > 3) {
        error = "Check network address, port, player limit and bot count";
        return false;
    }
    if (p.network == NetworkMode::Localhost && p.bind != "127.0.0.1") {
        error = "Localhost must bind to 127.0.0.1";
        return false;
    }
    if (p.network != NetworkMode::Localhost &&
        (p.bind.starts_with("127.") || p.bind == "0.0.0.0" || p.bind.starts_with("169.254."))) {
        error = "Select an active LAN adapter";
        return false;
    }
    if (!p.advertised.empty() && !PublicIPv4(p.advertised)) {
        error = "Advertised override must be a public IPv4 address";
        return false;
    }
    if (p.rotation.empty() || p.rotation.size() > 64) {
        error = "Select 1 to 64 map/mode rows";
        return false;
    }
    for (auto &row : p.rotation)
        if (!Identifier(row.map) || !atomMode(row.mode)) {
            error = "Invalid map or unsupported game mode";
            return false;
        }
    auto pool = Tokens(p.modePool);
    if (p.mixed && pool.empty()) {
        error = "Select at least one mixed game mode";
        return false;
    }
    if (pool.size() > 6) {
        error = "Mixed mode pool supports at most six modes";
        return false;
    }
    for (auto &mode : pool)
        if (!atomMode(mode)) {
            error = "Unsupported mixed game mode: " + mode;
            return false;
        }
    for (auto &[key, text] : p.rules) {
        if (!Identifier(key) || protectedKey(key) || !value(text)) {
            error = "Invalid/protected rule: " + key;
            return false;
        }
        if (key.starts_with("scr_") && key.ends_with("_timelimit")) {
            if (!decimal(text, 10000)) {
                error = "Time limit requires a nonnegative number: " + key;
                return false;
            }
        } else if ((key.starts_with("scr_") || key == "sv_minPing" || key == "sv_maxPing" ||
                    key == "sv_privateClients") &&
                   !number(text, 1000000)) {
            error = "Rule requires a nonnegative integer: " + key;
            return false;
        }
        unsigned maximum = 1000000;
        for (auto boolean :
             {"g_allowvote", "sv_voice", "scr_teambalance", "scr_oldschool", "scr_hardcore",
              "scr_game_allowkillcam", "scr_game_onlyheadshots", "scr_player_forcerespawn",
              "scr_team_kickteamkillers", "scr_team_teamkillpointloss", "scr_team_teamkillpunish",
              "sv_pure", "sv_webadmin"})
            if (key == boolean)
                maximum = 1;
        if (key == "scr_team_fftype" || key == "scr_game_spectatetype")
            maximum = 3;
        if (key == "sv_privateClients")
            maximum = p.maxClients;
        if (key == "khz_webadmin_port")
            maximum = 65535;
        if (key == "banlist_maxipbantime")
            maximum = 240;
        if (key == "sv_minPing" || key == "sv_maxPing")
            maximum = 999;
        if (maximum != 1000000 &&
            (!number(text, maximum) || (key == "khz_webadmin_port" && std::stoul(text) < 1024))) {
            error = "Rule is outside its supported range: " + key;
            return false;
        }
        if (key == "scr_player_maxhealth" && text == "0") {
            error = "Player health must be positive";
            return false;
        }
        if (key == "khz_webadmin_bind" && InetPtonA(AF_INET, text.c_str(), &address) != 1) {
            error = "Web-admin bind requires an IPv4 address";
            return false;
        }
    }
    auto rcon = p.rules.find("rcon_password");
    if (rcon != p.rules.end() && !rcon->second.empty() &&
        (rcon->second.size() < 8 || rcon->second.size() > 128)) {
        error = "RCON password requires 8 to 128 characters, or empty to disable";
        return false;
    }
    return ValidateExtra(p.extra, error);
}
std::string Configuration(const Profile &p) {
    std::ostringstream out;
    out << "// Generated by cod4x1khz Server Manager. Edit the profile in the manager.\n"
        << p.extra << "\n";
    auto set = [&](const std::string &key, const std::string &text) {
        out << "set " << key << " \"" << text << "\"\n";
    };
    for (auto &[key, text] : p.rules)
        set(key, text);
    set("sv_hostname", p.hostname);
    set("g_password", p.password);
    set("sv_maxclients", std::to_string(p.maxClients));
    set("khz_bot_count", std::to_string(p.bots));
    set("khz_bot_difficulty", std::to_string(p.difficulty));
    std::string rotation;
    for (auto &row : p.rotation)
        rotation += "gametype " + row.mode + " map " + row.map + " ";
    set("sv_mapRotation", rotation);
    set("sv_mapRotationCurrent", "");
    set("sv_randomMapRotation", p.shuffle ? "1" : "0");
    set("khz_rotationGametypePool", p.mixed ? p.modePool : "");
    set("g_gametype", p.rotation.front().mode);
    set("sv_fps", "1000");
    set("sv_maxRate", "2000000");
    set("net_lanauthorize", "0");
    set("sv_punkbuster", "0");
    set("logfile", "2");
    return out.str();
}
bool ImportConfiguration(Profile &p, std::string_view text, std::string &error) {
    if (text.size() > 65536) {
        error = "Imported config is too large";
        return false;
    }
    Profile proposed = p;
    std::string extra;
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
        auto words = Tokens(line);
        if (words.size() == 3 && (words[0] == "set" || words[0] == "seta" || words[0] == "sets")) {
            const auto &key = words[1];
            const auto &v = words[2];
            if (protectedKey(key)) {
                if (key == "sv_fps" && v == "1000")
                    continue;
                error = "Imported config overrides protected setting: " + key;
                return false;
            }
            if (key == "sv_hostname")
                proposed.hostname = v;
            else if (key == "g_password")
                proposed.password = v;
            else if (key == "sv_maxclients" || key == "khz_bot_count" ||
                     key == "khz_bot_difficulty" || key == "sv_randomMapRotation") {
                unsigned maximum = key == "sv_maxclients"        ? 16
                                   : key == "khz_bot_count"      ? 15
                                   : key == "khz_bot_difficulty" ? 3
                                                                 : 1;
                if (!number(v, maximum)) {
                    error = "Invalid imported setting: " + key;
                    return false;
                }
                auto n = std::stoul(v);
                if (key == "sv_maxclients")
                    proposed.maxClients = n;
                else if (key == "khz_bot_count")
                    proposed.bots = n;
                else if (key == "khz_bot_difficulty")
                    proposed.difficulty = n;
                else
                    proposed.shuffle = n != 0;
            } else if (key == "khz_rotationGametypePool") {
                proposed.modePool = v;
                proposed.mixed = !v.empty();
            } else if (key == "sv_mapRotation") {
                auto tokens = Tokens(v);
                std::string mode = "dm";
                proposed.rotation.clear();
                if (tokens.size() % 2) {
                    error = "Incomplete map rotation pair";
                    return false;
                }
                for (size_t i = 0; i + 1 < tokens.size(); i += 2) {
                    if (tokens[i] == "gametype")
                        mode = tokens[i + 1];
                    else if (tokens[i] == "map")
                        proposed.rotation.push_back({tokens[i + 1], mode});
                    else {
                        error = "Invalid rotation";
                        return false;
                    }
                }
            } else if (proposed.rules.contains(key))
                proposed.rules[key] = v;
            else
                extra += line + '\n';
        } else
            extra += line + '\n';
    }
    proposed.extra = extra;
    if (!Validate(proposed, error))
        return false;
    p = std::move(proposed);
    return true;
}
std::filesystem::path ProfileHome(const std::filesystem::path &root, const Profile &p) {
    return root / "runtime/servers" / p.id;
}
bool SaveProfile(const std::filesystem::path &root, const Profile &p, std::string &error) {
    if (!Validate(p, error))
        return false;
    try {
        auto home = ProfileHome(root, p);
        std::filesystem::create_directories(home / "main");
        std::ostringstream out;
        out << "version=1\n";
        write(out, "id", p.id);
        write(out, "name", p.name);
        write(out, "hostname", p.hostname);
        write(out, "password", p.password);
        write(out, "network", std::to_string(unsigned(p.network)));
        write(out, "bind", p.bind);
        write(out, "advertised", p.advertised);
        write(out, "port", std::to_string(p.port));
        write(out, "maxClients", std::to_string(p.maxClients));
        write(out, "bots", std::to_string(p.bots));
        write(out, "difficulty", std::to_string(p.difficulty));
        write(out, "shuffle", p.shuffle ? "1" : "0");
        write(out, "mixed", p.mixed ? "1" : "0");
        write(out, "automaticNetwork", p.automaticNetwork ? "1" : "0");
        write(out, "modePool", p.modePool);
        for (auto &row : p.rotation)
            write(out, "map", row.map + " " + row.mode);
        for (auto &[key, text] : p.rules)
            write(out, ("rule." + key).c_str(), text);
        if (!atomicWrite(home / "profile.ini", out.str(), error) ||
            !atomicWrite(home / "extra.cfg", p.extra, error))
            return false;
        return atomicWrite(home / "main/manager.cfg", Configuration(p), error);
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool LoadProfile(const std::filesystem::path &file, Profile &p, std::string &error) {
    try {
        if (std::filesystem::file_size(file) > 131072) {
            error = "Profile is too large";
            return false;
        }
        std::ifstream in(file);
        Profile result;
        result.rotation.clear();
        std::string line;
        bool version = false;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            auto at = line.find('=');
            if (at == line.npos)
                continue;
            auto key = line.substr(0, at), v = line.substr(at + 1);
            if (key == "version")
                version = v == "1";
            else if (key == "id")
                result.id = v;
            else if (key == "name")
                result.name = v;
            else if (key == "hostname")
                result.hostname = v;
            else if (key == "password")
                result.password = v;
            else if (key == "bind")
                result.bind = v;
            else if (key == "advertised")
                result.advertised = v;
            else if (key == "modePool")
                result.modePool = v;
            else if (key == "map") {
                auto row = Tokens(v);
                if (row.size() != 2) {
                    error = "Invalid map row";
                    return false;
                }
                result.rotation.push_back({row[0], row[1]});
            } else if (key.starts_with("rule."))
                result.rules[key.substr(5)] = v;
            else if (key == "network" || key == "port" || key == "maxClients" || key == "bots" ||
                     key == "difficulty" || key == "shuffle" || key == "mixed" ||
                     key == "automaticNetwork") {
                if (!number(v, 65535)) {
                    error = "Invalid profile number";
                    return false;
                }
                auto n = std::stoul(v);
                if (key == "network")
                    result.network = NetworkMode(n);
                else if (key == "port")
                    result.port = n;
                else if (key == "maxClients")
                    result.maxClients = n;
                else if (key == "bots")
                    result.bots = n;
                else if (key == "difficulty")
                    result.difficulty = n;
                else if (key == "shuffle")
                    result.shuffle = n != 0;
                else if (key == "mixed")
                    result.mixed = n != 0;
                else
                    result.automaticNetwork = n != 0;
            }
        }
        auto extraPath = file.parent_path() / "extra.cfg";
        if (std::filesystem::exists(extraPath) && std::filesystem::file_size(extraPath) > 65536) {
            error = "Extra configuration is too large";
            return false;
        }
        std::ifstream extra(extraPath, std::ios::binary);
        result.extra.assign(std::istreambuf_iterator<char>(extra), {});
        if (!version || !Validate(result, error))
            return false;
        p = std::move(result);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
std::vector<std::string> InstalledMaps(const std::filesystem::path &root) {
    std::vector<std::string> result;
    const auto zone = root / "runtime/zone";
    if (std::filesystem::is_directory(zone))
        for (auto &entry : std::filesystem::recursive_directory_iterator(zone)) {
            auto name = entry.path().stem().string();
            if (entry.is_regular_file() && entry.path().extension() == ".ff" &&
                name.starts_with("mp_") && !name.ends_with("_load") && Identifier(name))
                result.push_back(name);
        }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
std::wstring StartArguments(const Profile &p) {
    auto wide = [](const std::string &s) { return std::wstring(s.begin(), s.end()); };
    return L"+set khz_serverProfile " + wide(p.id) + L" +set net_ip " + wide(p.bind) +
           L" +set net_port " + std::to_wstring(p.port) + L" +set khz_bots 1 +set khz_bot_count " +
           std::to_wstring(p.bots) + L" +set khz_bot_difficulty " + std::to_wstring(p.difficulty) +
           L" +set sv_maxclients " + std::to_wstring(p.maxClients) + L" +exec manager.cfg";
}
Packet ConfigurationPacket(const Profile &p) {
    Packet value;
    value.message = HostMessage::Configure;
    value.manager = 1;
    value.port = p.port;
    value.maxClients = p.maxClients;
    value.dedicated = unsigned(p.network);
    value.bots = p.bots;
    value.difficulty = p.difficulty;
    strcpy_s(value.map, p.rotation.front().map.c_str());
    strcpy_s(value.gametype, p.rotation.front().mode.c_str());
    return value;
}
} // namespace khz::hosting
