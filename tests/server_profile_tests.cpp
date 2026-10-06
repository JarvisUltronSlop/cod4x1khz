#include "server_profile.hpp"
#include "host_transport.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
int main() {
    using namespace khz::hosting;
    Profile profile;
    profile.id = "00112233445566778899aabbccddeeff";
    std::string error;
    assert(Validate(profile, error));
    assert(profile.rules.at("rcon_password").empty());
    auto fractional = profile;
    fractional.rules["scr_dm_timelimit"] = "2.5";
    assert(Validate(fractional, error));
    fractional.rules["scr_dm_timelimit"] = "nan";
    assert(!Validate(fractional, error));
    assert(PublicIPv4("172.32.1.2") && !PublicIPv4("172.16.1.2") && !PublicIPv4("100.64.1.2") &&
           !PublicIPv4("127.0.0.1"));
    auto changed = profile;
    changed.port = 0;
    assert(!Validate(changed, error));
    changed = profile;
    changed.bots = 16;
    assert(!Validate(changed, error));
    changed = profile;
    changed.network = NetworkMode::Lan;
    assert(!Validate(changed, error));
    changed.bind = "192.168.1.2";
    assert(Validate(changed, error));
    changed = profile;
    changed.extra = "set sv_fps 20\n";
    assert(!Validate(changed, error));
    changed.extra = "exec bypass.cfg\n";
    assert(!Validate(changed, error));
    changed.extra = "set custom_mod_option \"kept value\"\n";
    assert(Validate(changed, error));
    assert(ImportConfiguration(
        changed,
        "// saved\nset sv_hostname \"Imported server\"\nset custom_thing 42\nset sv_mapRotation "
        "\"gametype war map mp_crash gametype sd map mp_backlot\"\n",
        error));
    assert(changed.hostname == "Imported server" && changed.rotation.size() == 2 &&
           changed.rotation[1].mode == "sd" &&
           changed.extra.find("custom_thing") != std::string::npos);
    auto original = changed;
    assert(!ImportConfiguration(changed, "set timescale 2\n", error));
    assert(changed.hostname == original.hostname);
    assert(!ImportConfiguration(changed, "set sv_mapRotation \"map mp_crash gametype\"\n", error));
    assert(!ImportConfiguration(changed, "set khz_bot_difficulty 5\n", error));
    auto invalidRule = profile;
    invalidRule.rules["scr_team_fftype"] = "4";
    assert(!Validate(invalidRule, error));
    invalidRule = profile;
    invalidRule.rules["khz_webadmin_port"] = "70000";
    assert(!Validate(invalidRule, error));
    changed.mixed = true;
    changed.modePool = "dm invalid";
    assert(!Validate(changed, error));
    changed.modePool = "dm war";
    assert(Validate(changed, error));
    auto root = std::filesystem::temp_directory_path() /
                ("khz-profile-test-" + std::to_string(GetCurrentProcessId()));
    assert(SaveProfile(root, changed, error));
    Profile loaded;
    assert(LoadProfile(ProfileHome(root, changed) / "profile.ini", loaded, error));
    assert(loaded.id == changed.id && loaded.extra == changed.extra &&
           loaded.rotation.size() == 2 && loaded.mixed && loaded.modePool == "dm war");
    auto oldHome = ProfileHome(root, loaded);
    loaded.port = 29999;
    assert(ProfileHome(root, loaded) == oldHome);
    assert(SaveProfile(root, loaded, error));
    auto config = Configuration(loaded);
    assert(config.find("set sv_fps \"1000\"") != config.npos &&
           config.find("local-khz-benchmark") == config.npos);
    auto imported = profile;
    assert(ImportConfiguration(imported, config, error));
    assert(imported.rotation.size() == loaded.rotation.size() && imported.mixed &&
           imported.modePool == loaded.modePool && imported.bots == loaded.bots &&
           imported.extra.find("custom_thing") != imported.extra.npos);
    auto packet = ConfigurationPacket(loaded);
    assert(packet.manager == 1 && packet.port == 29999 && packet.maxClients == 16);
    assert(Quote(L"a b\\") == L"\"a b\\\\\"");
    std::filesystem::remove_all(root);
    std::cout
        << "Profile validation, config preservation, stable homes, startup and quoting passed\n";
}
