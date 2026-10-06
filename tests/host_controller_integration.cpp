#include "host_controller.hpp"
#include "server_profile.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <sstream>
#include <cstring>

using namespace khz::hosting;
namespace fs = std::filesystem;
static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class Predicate>
static void until(Controller &host, Predicate predicate, unsigned seconds = 120) {
    auto deadline = GetTickCount64() + seconds * 1000ull, lastQuery = 0ull;
    do {
        host.Poll();
        auto now = GetTickCount64();
        if (host.phase == Controller::Phase::Running && now - lastQuery >= 200) {
            Packet query;
            query.message = HostMessage::Status;
            host.Request(query);
            lastQuery = now;
        }
        if (predicate())
            return;
        if (host.phase == Controller::Phase::Failed)
            throw std::runtime_error(host.error);
        Sleep(10);
    } while (GetTickCount64() < deadline);
    throw std::runtime_error("Owned host condition timed out");
}
static void stop(Controller &host) {
    host.Stop();
    until(host, [&] { return host.phase == Controller::Phase::Idle; }, 10);
}
static void command(Controller &host, const char *text) {
    Packet p;
    p.message = HostMessage::Command;
    strcpy_s(p.command, text);
    require(host.Request(p), "Command queue rejected request");
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 2) {
        std::cerr << "Usage: khz_host_controller_integration <installation/project root>\n";
        return 2;
    }
    const auto root = fs::absolute(argv[1]);
    Controller host, conflict;
    Profile profile;
    char token[33];
    NewToken(token);
    profile.id = token;
    profile.name = "Integration fixture";
    profile.port = 29040;
    profile.bots = 15;
    profile.rules["scr_dm_scorelimit"] = "0";
    profile.rules["scr_dm_timelimit"] = "0";
    const auto home = ProfileHome(root, profile);
    auto output = root / "results/hosting-integration";
    fs::create_directories(output);
    std::ofstream report(output / "report.txt");
    report << std::unitbuf;
    std::string error, replyText;
    host.onReply = [&](const Packet &reply) {
        if (reply.message == HostMessage::CommandResult) {
            replyText = reply.output;
            report << "command: " << reply.output << '\n';
        }
    };
    host.initialReadyCheck = [&](const Packet &reply) {
        for (auto &row : profile.rotation)
            if (row.map == reply.map) {
                if (!profile.mixed)
                    return row.mode == reply.gametype;
                std::istringstream pool(profile.modePool);
                std::string mode;
                while (pool >> mode)
                    if (mode == reply.gametype)
                        return true;
            }
        return false;
    };
    try {
        require(SaveProfile(root, profile, error), error.c_str());
        require(host.Start(root, StartArguments(profile), ConfigurationPacket(profile)),
                "Initial spawn failed");
        until(host, [&] {
            return host.phase == Controller::Phase::Running && host.status.playerCount == 15 &&
                   host.status.fixture;
        });
        require(host.status.port == profile.port && host.status.maxClients == 16,
                "Bound port/player limit differs");
        bool fixed = false, loopback = false;
        for (unsigned i = 0; i < host.status.ruleCount; ++i) {
            auto &rule = host.status.rules[i];
            if (std::string(rule.name) == "sv_fps")
                fixed = std::string(rule.value) == "1000";
            if (std::string(rule.name) == "net_ip")
                loopback = std::string(rule.value) == "127.0.0.1";
        }
        require(fixed && loopback, "Startup fixed tick/bind settings not loaded");
        auto first = host.status;
        until(host, [&] { return host.status.fixture >= first.fixture + 60000; }, 75);
        auto last = host.status;
        double hz = double(last.tick - first.tick) * 1000 / (last.fixture - first.fixture);
        report << "steady_server_hz=" << hz
               << " seconds=" << (last.fixture - first.fixture) / 1000.0 << '\n';
        require(hz >= 999 && hz <= 1001, "Simulation outside 0.1% tolerance");
        auto player = host.status.players[0];
        Packet stale;
        stale.message = HostMessage::PlayerAction;
        stale.targetSlot = player.slot;
        stale.admission = player.generation + 1;
        stale.session = host.status.session;
        stale.action = PlayerAction::Kick;
        replyText.clear();
        require(host.Request(stale), "Stale admission request failed");
        until(host, [&] { return replyText.find("reused") != replyText.npos; }, 5);
        stale.admission = player.generation;
        stale.action = PlayerAction::PermanentBan;
        replyText.clear();
        host.Request(stale);
        until(host, [&] { return replyText.find("Bots cannot") != replyText.npos; }, 5);
        command(host, "set sv_fps 20");
        replyText.clear();
        until(host, [&] { return replyText.find("unsupported") != replyText.npos; }, 5);
        Packet bots;
        bots.message = HostMessage::Bots;
        bots.bots = 0;
        bots.difficulty = 3;
        host.Request(bots);
        until(
            host, [&] { return host.status.playerCount == 0 && host.status.difficulty == 3; }, 35);
        bots.bots = 2;
        host.Request(bots);
        until(host, [&] { return host.status.playerCount == 2; }, 15);
        report << "live_bots_and_admission_checks=passed\n";
        require(conflict.Start(root, StartArguments(profile), ConfigurationPacket(profile)),
                "Conflict child spawn failed");
        auto deadline = GetTickCount64() + 120000;
        while (conflict.phase != Controller::Phase::Failed && GetTickCount64() < deadline) {
            conflict.Poll();
            host.Poll();
            Sleep(10);
        }
        require(conflict.phase == Controller::Phase::Failed &&
                    host.phase == Controller::Phase::Running,
                "Occupied port did not isolate failure");
        report << "occupied_port=" << conflict.error << '\n';
        stop(host);
        auto stableHome = ProfileHome(root, profile);
        profile.port = 29041;
        profile.bots = 15;
        profile.rotation = {{"mp_killhouse", "dm"}, {"mp_crash", "war"}};
        profile.rules["scr_dm_scorelimit"] = "5";
        profile.rules["scr_dm_timelimit"] = "1";
        profile.rules["scr_war_scorelimit"] = "5";
        profile.rules["scr_war_timelimit"] = "1";
        require(ProfileHome(root, profile) == stableHome, "Changing port lost stable profile home");
        require(SaveProfile(root, profile, error), error.c_str());
        require(host.Start(root, StartArguments(profile), ConfigurationPacket(profile)),
                "Rotation spawn failed");
        until(host, [&] {
            return host.phase == Controller::Phase::Running &&
                   std::string(host.status.map) == "mp_killhouse";
        });
        until(
            host,
            [&] {
                return std::string(host.status.map) == "mp_crash" &&
                       std::string(host.status.gametype) == "war";
            },
            180);
        report << "natural_assigned_rotation=mp_killhouse/dm -> mp_crash/war\n";
        Packet restart;
        restart.message = HostMessage::Restart;
        host.Request(restart);
        until(host, [&] { return host.phase == Controller::Phase::Running; });
        stop(host);
        profile.mixed = true;
        profile.shuffle = true;
        profile.modePool = "dm war";
        require(SaveProfile(root, profile, error), error.c_str());
        require(host.Start(root, StartArguments(profile), ConfigurationPacket(profile)),
                "Mixed-mode spawn failed");
        std::set<std::string> modes;
        std::set<std::string> maps;
        std::uint64_t previous = 0;
        auto visits = 0;
        until(
            host,
            [&] {
                if (host.phase == Controller::Phase::Running && host.status.session &&
                    host.status.session != previous) {
                    previous = host.status.session;
                    ++visits;
                    require(std::string(host.status.gametype) == "dm" ||
                                std::string(host.status.gametype) == "war",
                            "Mixed mode escaped selected pool");
                    modes.insert(host.status.gametype);
                    maps.insert(host.status.map);
                    report << "natural_mixed_visit=" << host.status.map << '/'
                           << host.status.gametype << '\n';
                }
                return visits >= 4 && modes.size() == 2 && maps.size() == 2;
            },
            360);
        report << "natural_mixed_rotation=passed\n";
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, host.Pid());
        require(process != nullptr, "Cannot access exact owned crash fixture");
        TerminateProcess(process, 123);
        CloseHandle(process);
        deadline = GetTickCount64() + 5000;
        while (host.phase != Controller::Phase::Failed && GetTickCount64() < deadline) {
            host.Poll();
            Sleep(10);
        }
        require(host.phase == Controller::Phase::Failed, "Crash did not expose Failed state");
        require(fs::is_regular_file(home / "profile.ini"), "Crash lost saved profile");
        report << "crash=" << host.error << "\npassed=true\n";
        fs::rename(home, output / ("profile-data-" + profile.id));
        std::cout << "Owned hosting, 60-second simulation, live controls, port conflict, stable "
                     "profile, actual match rotation and crash recovery passed\n";
        return 0;
    } catch (const std::exception &e) {
        host.Close();
        conflict.Close();
        report << "passed=false\nerror=" << e.what() << '\n';
        std::cerr << e.what() << '\n';
        return 1;
    }
}
