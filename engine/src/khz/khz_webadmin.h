#pragma once
#include "khz_admin.h"
#include <array>
#include <cstdint>
#include <memory>

namespace khz::web {
struct Player {
    admin::Admission admission{};
    bool connected = false, bot = false, undercover = false;
    int power = 0, score = 0, ping = 0, team = 0;
    char name[64]{};
};
struct View {
    std::uint64_t serverSession = 0;
    int serverId = 0;
    char hostname[256]{}, map[64]{}, gametype[64]{};
    std::array<Player, 16> players{};
    std::array<admin::AdminRecord, admin::MaxAdmins> administrators{};
};
struct Config { char bind[16] = "127.0.0.1"; unsigned short port = 28980; wchar_t staticRoot[512]{}; };
enum class Action : unsigned { Page, Login, Logout, Command, Kick, Ban, OfflineBan };
struct Request {
    unsigned slot = 0;
    std::uint64_t generation = 0, epoch = 0, deadline = 0, serverSession = 0;
    int serverId = 0, targetSlot = -1;
    Action action = Action::Page;
    admin::Admission target{};
    admin::AdminRecord credential{};
    bool passwordVerified = false, targetBot = false;
    char session[65]{}, csrf[65]{}, text[1024]{};
};
struct Reply {
    int status = 200, power = 0;
    bool authorized = false, clearCookie = false;
    char session[65]{}, csrf[65]{}, name[32]{}, text[4096]{};
};
}
// Engine calls only from owner service/lifecycle, never a gameplay tick.
bool KHZ_WebadminStart(const khz::web::Config& config);
void KHZ_WebadminPublish(std::shared_ptr<const khz::web::View> view);
bool KHZ_WebadminPoll(khz::web::Request* request);
void KHZ_WebadminComplete(const khz::web::Request& request, const khz::web::Reply& reply);
void KHZ_WebadminShutdown();
void KHZ_WebadminErrorCleanup();
bool KHZ_WebadminRunning();
const char* KHZ_WebadminStatsJSON();
void KHZ_WebadminRecordOwnerWork(std::uint64_t microseconds);
