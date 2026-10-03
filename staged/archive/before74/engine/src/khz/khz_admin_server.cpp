#include <universal/q_shared.h>
#include "khz_admin_server.h"
#include "khz_admin.h"
#include "khz_bots.h"
#include "khz_paths.h"
#include "khz_runtime.h"
#include "khz_webadmin.h"
#include "khz_screenshot.h"
#include <webadmin_policy.hpp>
#include <server_mp/server_mp.h>
#include <server/sv_game.h>
#include <game_mp/g_public_mp.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/threads.h>
#include <script/scr_vm.h>
#include <core.hpp>
#include <Windows.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <iterator>

namespace {
namespace id = khz::identity;
namespace policy = khz::admin;
policy::Store store;
policy::RconLimiter rconLimiter;
policy::IpBanCache ipBanCache;
struct ClientIdentity {
    id::Fingerprint fingerprint{};
    std::uint64_t generation = 0, lastCommandTick = UINT64_MAX;
    std::array<std::uint64_t, 1000> commandTicks{};
    unsigned nextCommandTick = 0, commandCount = 0;
    int temporaryPower = -1, uid = 0;
    bool undercover = false;
    policy::CommandWhitelist whitelist{};
};
std::array<ClientIdentity, KHZ_MaxMatchClients> clients{};
struct PendingProof {
    netadr_t address{};
    id::Challenge challenge{};
    id::Fingerprint admitted{};
    id::Proof proof{};
    std::uint64_t issued = 0, admittedGeneration = 0;
    int admittedSlot = -1;
    bool used = false;
};
std::array<PendingProof, 1024> pendingProofs{};
std::array<std::uint8_t, 32> bootNonce{};
struct AdminRequest { int slot = -1; std::uint64_t generation = 0; char text[1024]{}; };
std::array<AdminRequest, 32> requests{};
unsigned requestHead = 0, requestCount = 0;
struct PluginBanRequest {
    policy::Subject subject{};
    policy::Admission admission{};
    policy::Invoker issuer{};
    std::int64_t submitted = 0, expires = 0;
    int slot = -1, invokerUid = 0, issuerPower = 0;
    char name[64]{}, reason[127]{}, issuerName[64]{}, plugin[64]{};
};
std::array<PluginBanRequest, 32> pluginBans{};
unsigned pluginBanHead = 0, pluginBanCount = 0;
struct AuditEntry { std::int64_t unixTime = 0; char text[512]{}; };
std::array<AuditEntry, 128> audit{};
unsigned auditHead = 0, auditCount = 0;
std::uint64_t auditDropped = 0, deniedCommands = 0, acceptedCommands = 0, proofAccepted = 0, proofRejected = 0;
std::uint64_t pluginBansAccepted = 0, pluginBansRejected = 0, pluginBansReusedSlot = 0;
std::uint64_t generationSerial = 0;
policy::Invoker currentInvoker{{}, true};
int redirectSlot = -1;
bool remoteConsoleContext = false;
bool initialized = false, storageReady = false, cryptoReady = false;
std::filesystem::path storagePath, logPath;
const dvar_t *reservedPower = nullptr, *steam64 = nullptr, *logEnabled = nullptr;
const dvar_t *ipBanMinutes = nullptr, *appealUrl = nullptr, *appealHours = nullptr, *randomRotation = nullptr;
const dvar_t *legacyAppealUrl = nullptr, *legacyAppealHours = nullptr;

struct Registration { const char* name; int power; void (*function)(); cmd_function_s command{}; };
void adminCommand(); void banCommand(); void kickCommand(); void chatCommand(); void statusCommand(); void screenshotCommand();
void initWebadmin(); void serviceWebadmin(); void clearWebadminOwnerContext();
Registration registrations[] = {
    {"AdminAddAdmin",95,adminCommand}, {"AdminAddAdminWithPassword",95,adminCommand},
    {"AdminRemoveAdmin",95,adminCommand}, {"AdminListAdmins",80,adminCommand},
    {"AdminChangePassword",95,adminCommand}, {"AdminChangeCommandPower",98,adminCommand},
    {"AdminListCommands",95,adminCommand}, {"ChangePassword",10,adminCommand},
    {"undercover",60,adminCommand}, {"writenvcfg",100,adminCommand},
    {"permban",80,banCommand}, {"tempban",50,banCommand}, {"banUser",100,banCommand},
    {"banClient",100,banCommand}, {"unban",80,banCommand}, {"unbanUser",100,banCommand},
    {"kick",35,kickCommand}, {"onlykick",100,kickCommand}, {"clientkick",100,kickCommand},
    {"consay",100,chatCommand}, {"screensay",70,chatCommand},
    {"contell",100,chatCommand}, {"screentell",70,chatCommand},
    {"ministatus",1,statusCommand}, {"khz_admin_status",80,statusCommand},
    {"khz_admin_reload",100,statusCommand}, {"getss",45,screenshotCommand}
};
void owner() { iassert(Sys_IsMainThread()); }
std::int64_t unixTime() { return static_cast<std::int64_t>(std::time(nullptr)); }
int slot(const client_t* client) {
    if (!client || !svs.clients) return -1;
    const auto index = client - svs.clients;
    return index >= 0 && index < KHZ_MaxMatchClients ? static_cast<int>(index) : -1;
}
std::uint32_t ipv4(const netadr_t& address) {
    if (address.type == NA_LOOPBACK) return 0x0100007f;
    if (address.type != NA_IP) return 0;
    std::uint32_t value; std::memcpy(&value, address.ip, 4); return value;
}
bool numeric(const char* text, int maximum, int& result) {
    if (!text || !*text) return false;
    unsigned value = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || value > unsigned(maximum) / 10) return false;
        value = value * 10 + unsigned(*p - '0'); if (value > unsigned(maximum)) return false;
    }
    result = int(value); return true;
}
const char* resultText(policy::Result result) {
    switch (result) {
    case policy::Result::Ok: return "ok"; case policy::Result::Denied: return "insufficient power";
    case policy::Result::Invalid: return "invalid arguments"; case policy::Result::Full: return "bounded storage is full";
    case policy::Result::NotFound: return "not found"; default: return "storage error";
    }
}
void cleanLog(char* text) {
    for (char* p = text; *p; ++p) if (static_cast<unsigned char>(*p) < 32 || *p == 127) *p = ' ';
}
const char* invokerName() {
    if (currentInvoker.console) return remoteConsoleContext ? "rcon" : "console";
    if (redirectSlot >= 0 && redirectSlot < KHZ_MaxMatchClients && svs.clients) return svs.clients[redirectSlot].name;
    if (const auto* account = store.FindAdmin(currentInvoker.identity)) return account->name;
    return "local-key";
}
bool invokerCanExecute(const char* command, int clientIndex) {
    if (!storageReady || !command) return false;
    char name[64];
    if (!policy::ValidateCommand(command, name, sizeof(name)) || !Cmd_FindCommand(name)) return false;
    const auto* whitelist = clientIndex >= 0 && clientIndex < KHZ_MaxMatchClients ? &clients[clientIndex].whitelist : nullptr;
    return policy::CanExecute(store, currentInvoker, command, whitelist);
}
bool playerCanExecute(int index, const char* command) {
    if (!storageReady || !command || index < 0 || index >= KHZ_MaxMatchClients || !svs.clients ||
        svs.clients[index].header.state < CS_CONNECTED || KHZ_IsNativeBot(&svs.clients[index])) return false;
    char name[64];
    if (!policy::ValidateCommand(command, name, sizeof(name)) || !Cmd_FindCommand(name)) return false;
    return policy::CanExecute(store, {clients[index].fingerprint, false, clients[index].temporaryPower}, command, &clients[index].whitelist);
}
void auditEvent(const char* action, const char* target = "") {
    if (auditCount == audit.size()) { ++auditDropped; return; }
    auto& entry = audit[(auditHead + auditCount++) % audit.size()]; entry.unixTime = unixTime();
    char issuer[65]; id::EncodeHex(currentInvoker.identity.data(), currentInvoker.identity.size(), issuer, sizeof(issuer));
    std::snprintf(entry.text, sizeof(entry.text), "power=%d issuer=%s name=%s action=%s target=%s",
        store.Power(currentInvoker), currentInvoker.console ? "console/rcon" : issuer, invokerName(), action, target);
    cleanLog(entry.text);
}
bool persist() {
    if (!storageReady || !store.Save(storagePath.c_str())) {
        Com_Printf(CON_CHANNEL_SERVER, "cod4x1khz administration: could not persist policy; changes remain in memory.\n");
        return false;
    }
    return true;
}
bool allowed(const char* command) {
    if (!invokerCanExecute(command, redirectSlot)) {
        ++deniedCommands; Com_Printf(CON_CHANNEL_SERVER, "Command denied: %s\n", command); return false;
    }
    return true;
}
bool containsCaseInsensitive(const char* text, const char* fragment) {
    if (!*fragment) return false;
    for (; *text; ++text) {
        const char *a = text, *b = fragment;
        while (*a && *b) {
            const char ac = *a >= 'A' && *a <= 'Z' ? char(*a + 32) : *a;
            const char bc = *b >= 'A' && *b <= 'Z' ? char(*b + 32) : *b;
            if (ac != bc) break;
            ++a; ++b;
        }
        if (!*b) return true;
    }
    return false;
}
client_t* findClient(const char* handle) {
    if (!svs.clients || !sv_maxclients || !handle) return nullptr;
    int index;
    if (numeric(handle, KHZ_MaxMatchClients - 1, index)) return index < sv_maxclients->current.integer && svs.clients[index].header.state >= CS_CONNECTED ? &svs.clients[index] : nullptr;
    client_t* match = nullptr;
    for (int i = 0; i < (std::min)(KHZ_MaxMatchClients, sv_maxclients->current.integer); ++i) {
        auto& client = svs.clients[i]; if (client.header.state < CS_CONNECTED) continue;
        if (!I_stricmp(handle, client.name)) return &client;
        // Public commands permit a unique substring, with ambiguity rejected.
        if (containsCaseInsensitive(client.name, handle)) { if (match) return nullptr; match = &client; }
    }
    return match;
}
bool targetIdentity(const char* handle, id::Fingerprint& fingerprint, client_t** online = nullptr) {
    fingerprint = {}; if (online) *online = nullptr;
    if (auto* client = findClient(handle)) {
        const int index = slot(client); fingerprint = clients[index].fingerprint;
        if (online) *online = client;
        return !id::IsEmpty(fingerprint);
    }
    if (id::DecodeHex(handle ? std::string_view(handle) : std::string_view{}, fingerprint.data(), fingerprint.size()) && !id::IsEmpty(fingerprint)) return true;
    if (const auto* admin = store.FindAdmin(handle ? handle : "")) { fingerprint = admin->identity; return true; }
    return false;
}
policy::Subject subjectFor(client_t* client, const id::Fingerprint& fingerprint) {
    policy::Subject subject{}; subject.identity = fingerprint;
    if (client) subject.ipv4 = ipv4(client->header.netchan.remoteAddress);
    return subject; // No official identity provider has authenticated a Steam ID.
}
void argsText(int start, char* text, unsigned capacity) { Cmd_ArgsBuffer(start, text, int(capacity)); }
bool reasonText(int start, char* text, unsigned capacity) {
    std::size_t length = 0;
    for (int i = start; i < Cmd_Argc(); ++i) {
        if (i != start) ++length;
        length += std::strlen(Cmd_Argv(i));
        if (length >= capacity) return false;
    }
    argsText(start, text, capacity); return true;
}
void adminCommand() {
    owner(); const char* command = Cmd_Argv(0); if (!allowed(command)) return;
    policy::Result result = policy::Result::Invalid;
    id::Fingerprint fingerprint{}; client_t* online = nullptr;
    if (!I_stricmp(command, "AdminListAdmins")) {
        for (const auto& record : store.Data().admins) if (record.power) {
            char full[65]; id::EncodeHex(record.identity.data(), record.identity.size(), full, sizeof(full));
            Com_Printf(CON_CHANNEL_SERVER, "%s power=%u identity=%s%s\n", record.name, record.power, full, record.undercover ? " undercover" : "");
        }
        return;
    }
    if (!I_stricmp(command, "AdminListCommands")) {
        for (const auto& rule : store.Data().commands) if (*rule.name && rule.power <= store.Power(currentInvoker))
            Com_Printf(CON_CHANNEL_SERVER, "%s power=%u\n", rule.name, rule.power);
        return;
    }
    if (!I_stricmp(command, "writenvcfg")) { persist(); return; }
    if (!I_stricmp(command, "AdminChangeCommandPower")) {
        int power;
        if (Cmd_Argc() == 3 && numeric(Cmd_Argv(2), 100, power)) result = store.SetCommandPower(currentInvoker, Cmd_Argv(1), power);
    } else if (!I_stricmp(command, "ChangePassword")) {
        if (Cmd_Argc() == 3 && store.Authorize(store.FindAdmin(currentInvoker.identity) ? store.FindAdmin(currentInvoker.identity)->name : "", Cmd_Argv(1)))
            result = store.SetPassword(currentInvoker, currentInvoker.identity, Cmd_Argv(2));
    } else if (!I_stricmp(command, "undercover")) {
        client_t* target = nullptr; bool hidden = false;
        if (Cmd_Argc() == 1 && !currentInvoker.console && redirectSlot >= 0) {
            target = &svs.clients[redirectSlot]; hidden = !clients[redirectSlot].undercover;
        } else if (currentInvoker.console && Cmd_Argc() == 3) {
            int setting;
            if (numeric(Cmd_Argv(2), 1, setting)) { target = findClient(Cmd_Argv(1)); hidden = setting != 0; }
        }
        if (target && target->gentity && target->gentity->client && target->gentity->client->sess.sessionState != SESS_STATE_PLAYING) {
            auto& state = clients[slot(target)]; state.undercover = hidden;
            result = store.FindAdmin(state.fingerprint) ? store.SetUndercover(currentInvoker, state.fingerprint, hidden) : policy::Result::Ok;
        } else if (target) {
            Com_Printf(CON_CHANNEL_SERVER, "Undercover requires a spectator.\n"); result = policy::Result::Denied;
        }
    } else if (Cmd_Argc() >= 2 && targetIdentity(Cmd_Argv(1), fingerprint, &online)) {
        if (!I_stricmp(command, "AdminRemoveAdmin") && Cmd_Argc() == 2) result = store.RemoveAdmin(currentInvoker, fingerprint);
        else if (!I_stricmp(command, "AdminChangePassword") && Cmd_Argc() == 3) result = store.SetPassword(currentInvoker, fingerprint, Cmd_Argv(2));
        else if ((!I_stricmp(command, "AdminAddAdmin") && Cmd_Argc() == 3) || (!I_stricmp(command, "AdminAddAdminWithPassword") && Cmd_Argc() == 4)) {
            int power;
            if (numeric(Cmd_Argv(2), 100, power)) {
                const auto* old = store.FindAdmin(fingerprint);
                const char* name = online ? online->name : (old ? old->name : Cmd_Argv(1));
                char fallback[32];
                if (std::strlen(name) >= sizeof(fallback)) { std::snprintf(fallback, sizeof(fallback), "admin-%016llx", id::LocalPlayerId(fingerprint)); name = fallback; }
                result = Cmd_Argc() == 4 ? store.SetAdminWithPassword(currentInvoker, fingerprint, name, power, Cmd_Argv(3)) :
                    store.SetAdmin(currentInvoker, fingerprint, name, power);
            }
        }
    }
    Com_Printf(CON_CHANNEL_SERVER, "%s: %s\n", command, resultText(result));
    if (result == policy::Result::Ok) { auditEvent(command, !I_stricmp(command, "ChangePassword") ? "self (password hidden)" : Cmd_Argv(1)); persist(); }
}
void banCommand() {
    owner(); const char* command = Cmd_Argv(0); if (!allowed(command)) return;
    if (Cmd_Argc() < 2) { Com_Printf(CON_CHANNEL_SERVER, "Usage: %s <slot|unique-name|full-key-identity> [duration] [reason]\n", command); return; }
    id::Fingerprint fingerprint{}; client_t* online = nullptr;
    if (!targetIdentity(Cmd_Argv(1), fingerprint, &online)) { Com_Printf(CON_CHANNEL_SERVER, "No unique player with a proven local identity.\n"); return; }
    const auto subject = subjectFor(online, fingerprint);
    if (!I_stricmp(command, "unban") || !I_stricmp(command, "unbanUser")) {
        const auto result = store.RemoveBan(currentInvoker, subject); Com_Printf(CON_CHANNEL_SERVER, "unban: %s\n", resultText(result));
        if (result == policy::Result::Ok) { auditEvent(command, Cmd_Argv(1)); persist(); } return;
    }
    const auto now = unixTime(); std::int64_t expires = 0; char reason[127]{};
    if (!I_stricmp(command, "tempban")) {
        std::int64_t seconds;
        if (Cmd_Argc() < 4 || !policy::ParseBanDuration(Cmd_Argv(2), seconds) || now > INT64_MAX - seconds) {
            Com_Printf(CON_CHANNEL_SERVER, "Usage: tempban <player> <Nm|Nh|Nd,maximum30d> <reason>\n"); return;
        }
        expires = now + seconds;
        if (!reasonText(3, reason, sizeof(reason))) { Com_Printf(CON_CHANNEL_SERVER, "Ban reason exceeds126 characters.\n"); return; }
    } else {
        if (Cmd_Argc() >= 3) {
            if (!reasonText(2, reason, sizeof(reason))) { Com_Printf(CON_CHANNEL_SERVER, "Ban reason exceeds126 characters.\n"); return; }
        } else I_strncpyz(reason, "Banned by administrator", sizeof(reason));
    }
    if (online && KHZ_AdminClientPower(online) > store.Power(currentInvoker)) { Com_Printf(CON_CHANNEL_SERVER, "Cannot ban an administrator with higher power.\n"); return; }
    const auto result = store.AddBan(currentInvoker, subject, online ? online->name : "offline", reason, now, expires, false);
    Com_Printf(CON_CHANNEL_SERVER, "%s: %s\n", command, resultText(result));
    if (result == policy::Result::Ok) {
        auditEvent(command, Cmd_Argv(1)); persist();
        if (online) SV_DropClient(online, va("Banned: %s", reason), true);
    }
}
void kickCommand() {
    owner(); const char* command = Cmd_Argv(0); if (!allowed(command)) return;
    if (Cmd_Argc() < 2) { Com_Printf(CON_CHANNEL_SERVER, "Usage: %s <player|all> [reason]\n", command); return; }
    char reason[127]; if (Cmd_Argc() >= 3) argsText(2, reason, sizeof(reason)); else I_strncpyz(reason, "Kicked by administrator", sizeof(reason));
    auto drop = [&](client_t* client) {
        if (KHZ_AdminClientPower(client) > store.Power(currentInvoker)) { Com_Printf(CON_CHANNEL_SERVER, "Cannot kick an administrator with higher power.\n"); return; }
        auditEvent(command, client->name); SV_DropClient(client, va("Kicked: %s", reason), true);
    };
    if (!I_stricmp(Cmd_Argv(1), "all")) {
        if (svs.clients && sv_maxclients) for (int i = 0; i < (std::min)(KHZ_MaxMatchClients, sv_maxclients->current.integer); ++i)
            if (svs.clients[i].header.state >= CS_CONNECTED) drop(&svs.clients[i]);
    } else if (auto* client = findClient(Cmd_Argv(1))) drop(client); else Com_Printf(CON_CHANNEL_SERVER, "No unique player found.\n");
}
void chatCommand() {
    owner(); if (!allowed(Cmd_Argv(0))) return;
    const bool tell = !I_stricmp(Cmd_Argv(0), "contell") || !I_stricmp(Cmd_Argv(0), "screentell");
    const bool screen = !I_stricmp(Cmd_Argv(0), "screensay") || !I_stricmp(Cmd_Argv(0), "screentell");
    auto* target = tell ? findClient(Cmd_Argv(1)) : nullptr;
    if ((tell && !target) || Cmd_Argc() <= (tell ? 2 : 1)) return;
    char text[512]; argsText(tell ? 2 : 1, text, sizeof(text));
    for (char* p = text; *p; ++p) if (*p == '"' || *p == '\\' || static_cast<unsigned char>(*p) < 32) *p = ' ';
    SV_GameSendServerCommand(tell ? slot(target) : -1, SV_CMD_CAN_IGNORE, va("%c \"%s\"", screen ? 'c' : 'e', text));
    auditEvent(Cmd_Argv(0), tell ? target->name : "all");
}
void screenshotCommand() {
    owner(); if (!allowed("getss")) return;
    if (Cmd_Argc() < 2 || Cmd_Argc() > 3) { Com_Printf(CON_CHANNEL_SERVER, "Usage: getss <player|all> [basename]\n"); return; }
    const bool all = !I_stricmp(Cmd_Argv(1), "all"); auto* target = all ? nullptr : findClient(Cmd_Argv(1));
    if (!all && !target) { Com_Printf(CON_CHANNEL_SERVER, "No unique connected player found.\n"); return; }
    KHZ_ScreenshotRequester requester;
    requester.kind = currentInvoker.console ? (remoteConsoleContext ? KHZ_ScreenshotRequesterKind::RemoteConsole : KHZ_ScreenshotRequesterKind::Console) :
        (redirectSlot >= 0 ? KHZ_ScreenshotRequesterKind::PlayerAdmin : KHZ_ScreenshotRequesterKind::WebAdmin);
    requester.slot = redirectSlot; requester.power = store.Power(currentInvoker);
    std::memcpy(requester.fingerprint, currentInvoker.identity.data(), sizeof(requester.fingerprint));
    const char* basename = Cmd_Argc() == 3 ? Cmd_Argv(2) : nullptr;
    if (KHZ_ScreenshotServerRequest(target, basename, requester)) { auditEvent("getss", all ? "all accepted humans" : target->name); Com_Printf(CON_CHANNEL_SERVER, "Screenshot request queued.\n"); }
    else Com_Printf(CON_CHANNEL_SERVER, "Screenshot request rejected by bounded private-pair service.\n");
}
void statusCommand() {
    owner(); if (!allowed(Cmd_Argv(0))) return;
    if (!I_stricmp(Cmd_Argv(0), "khz_admin_reload")) {
        storageReady = store.Load(storagePath.c_str()); Com_Printf(CON_CHANNEL_SERVER, "Policy reload: %s\n", storageReady ? "ok" : "failed; administration and admission are closed"); return;
    }
    Com_Printf(CON_CHANNEL_SERVER, "%s\n", KHZ_AdminStatsJSON());
    if (svs.clients && sv_maxclients) for (int i = 0; i < (std::min)(KHZ_MaxMatchClients, sv_maxclients->current.integer); ++i) {
        const auto& client = svs.clients[i]; if (client.header.state < CS_CONNECTED) continue;
        char full[65]; id::EncodeHex(clients[i].fingerprint.data(), clients[i].fingerprint.size(), full, sizeof(full));
        const auto* admin = store.FindAdmin(clients[i].fingerprint);
        Com_Printf(CON_CHANNEL_SERVER, "%d %s power=%d identity=%s provider=local-key%s\n", i, client.name,
            admin && admin->undercover && !currentInvoker.console ? 1 : KHZ_AdminClientPower(&client), full, KHZ_IsNativeBot(&client) ? " (native bot)" : "");
    }
}
void flushAudit() {
    if (!auditCount) return;
    if (!logEnabled || !logEnabled->current.enabled) { auditHead = auditCount = 0; return; }
    std::error_code error;
    if (std::filesystem::exists(logPath, error) && std::filesystem::file_size(logPath, error) >= 4 * 1024 * 1024) {
        const auto previous = logPath.wstring() + L".previous";
        MoveFileExW(logPath.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    FILE* file = nullptr; _wfopen_s(&file, logPath.c_str(), L"ab"); if (!file) return;
    unsigned count = 0;
    while (auditCount && count++ < 16) {
        const auto& entry = audit[auditHead];
        if (std::fprintf(file, "%lld %s\n", entry.unixTime, entry.text) < 0) break;
        auditHead = (auditHead + 1) % audit.size(); --auditCount;
    }
    std::fclose(file);
}
void redirectOutput(char* text) {
    if (redirectSlot < 0 || redirectSlot >= KHZ_MaxMatchClients) return;
    // Bounded reliable print fragments; no quote can terminate the command.
    char fragment[241];
    for (const char* p = text; *p;) {
        unsigned length = 0;
        while (*p && length < sizeof(fragment) - 1) {
            char c = *p++; if (c == '"' || c == '\\' || c == '\r' || c == '\n') c = ' ';
            fragment[length++] = c;
        }
        fragment[length] = 0; SV_GameSendServerCommand(redirectSlot, SV_CMD_CAN_IGNORE, va("e \"%s\"", fragment));
    }
}
client_t* scriptClient(scr_entref_t ref, unsigned params, const char* usage) {
    owner(); if (Scr_GetNumParam() != params) Scr_Error(usage);
    if (ref.classnum || ref.entnum >= KHZ_MaxMatchClients || !svs.clients || !sv_maxclients ||
        ref.entnum >= sv_maxclients->current.integer || !g_entities[ref.entnum].client) Scr_ObjectError("not a player");
    return &svs.clients[ref.entnum];
}
}

void KHZ_AdminInit() {
    owner(); if (initialized) return; initialized = true;
    cryptoReady = id::RandomBytes(bootNonce.data(), bootNonce.size());
    reservedPower = Dvar_RegisterInt("khz_reserved_power", 100, 0, 100, DVAR_ARCHIVE, "Minimum proven local admin power for reserved slots; zero disables power admission");
    steam64 = Dvar_RegisterBool("sv_usesteam64id", true, DVAR_ARCHIVE, "Display provider identifiers in Steam64 decimal form");
    logEnabled = Dvar_RegisterBool("khz_admin_log", true, DVAR_ARCHIVE, "Write bounded administration audit records outside gameplay ticks");
    ipBanMinutes = Dvar_RegisterInt("banlist_maxipbantime", 240, 0, 240, DVAR_ARCHIVE, "Maximum minutes for transient IP bans; zero disables");
    appealUrl = Dvar_RegisterString("sv_banappealurl", "", DVAR_ARCHIVE, "Server-provided ban appeal URL");
    appealHours = Dvar_RegisterInt("sv_banappealminhours", 4, 0, 336, DVAR_ARCHIVE, "Minimum remaining ban hours before showing appeal URL");
    legacyAppealUrl = Dvar_RegisterString("banlist_appealurl", "", DVAR_ARCHIVE, "Earlier project alias of sv_banappealurl");
    legacyAppealHours = Dvar_RegisterInt("banlist_appealminhours", 4, 0, 336, DVAR_ARCHIVE, "Earlier project alias of sv_banappealminhours");
    randomRotation = Dvar_RegisterBool("sv_randomMapRotation", false, DVAR_ARCHIVE, "Shuffle complete map/gametype pairs when rotation resets");
    initWebadmin();
    try {
        const auto directory = std::filesystem::path(KHZ_HomeRoot()) / "administration";
        std::filesystem::create_directories(directory); storagePath = directory / "policy.bin"; logPath = directory / "audit.log";
        if (std::filesystem::exists(storagePath)) storageReady = store.Load(storagePath.c_str());
        else storageReady = true;
    } catch (...) { storageReady = false; }
    for (auto& registration : registrations) {
        store.RegisterCommand(registration.name, registration.power);
        Cmd_RemoveCommand(registration.name);
        Cmd_AddCommandInternal(registration.name, registration.function, &registration.command);
    }
    // Existing server functions retain their implementation and are reachable
    // from the in-game bounded service only after these explicit power checks.
    const std::pair<const char*, int> existing[] = {
        {"status",100},{"serverinfo",1},{"systeminfo",1},{"dumpuser",50},{"map",60},{"devmap",100},{"map_rotate",100},
        {"map_restart",50},{"fast_restart",100},{"gametype",80},{"heartbeat",100},{"killserver",100},
        {"say",70},{"tell",70},{"scriptUsage",100},{"stringUsage",100},{"showconfigstring",100}
    };
    for (const auto& command : existing) store.RegisterCommand(command.first, command.second);
    if (storageReady && !std::filesystem::exists(storagePath)) persist();
    Com_Printf(CON_CHANNEL_SERVER, "cod4x1khz local-key identity: %s; administration storage: %s; official identity provider unavailable.\n", cryptoReady ? "ready" : "failed", storageReady ? "ready" : "failed");
}
bool KHZ_AdminChallengeResponse(const netadr_t& from, int challenge, char* text, std::size_t capacity) {
    owner(); if (!initialized || !cryptoReady || !storageReady) return false;
    const auto now = GetTickCount64(); PendingProof* available = nullptr; PendingProof* oldest = &pendingProofs[0];
    for (auto& pending : pendingProofs) if (pending.used && pending.admittedSlot >= 0 && NET_CompareAdr(pending.address, from)) {
        const int index = pending.admittedSlot;
        const bool connectingRetry = pending.challenge.challenge == std::uint32_t(challenge) && now - pending.issued <= 30000 &&
            svs.clients && index < KHZ_MaxMatchClients && svs.clients[index].header.state == CS_CONNECTED &&
            clients[index].generation == pending.admittedGeneration && clients[index].fingerprint == pending.admitted;
        if (!connectingRetry) pending = {};
    }
    for (auto& pending : pendingProofs) {
        if (pending.used && pending.challenge.challenge == std::uint32_t(challenge) && NET_CompareAdr(pending.address, from) && now - pending.issued <= 30000) { available = &pending; break; }
        if (!pending.used && !available) available = &pending;
        if (pending.issued < oldest->issued) oldest = &pending;
    }
    if (!available) available = oldest;
    if (!available->used || available->challenge.challenge != std::uint32_t(challenge) || !NET_CompareAdr(available->address, from) || now - available->issued > 30000) {
        *available = {}; available->address = from; available->challenge.boot = bootNonce;
        available->challenge.challenge = std::uint32_t(challenge); available->challenge.protocol = khz::Protocol;
        if (!id::RandomBytes(available->challenge.nonce.data(), available->challenge.nonce.size())) return false;
        available->issued = now; available->used = true;
    }
    char boot[65], nonce[65]; id::EncodeHex(bootNonce.data(), bootNonce.size(), boot, sizeof(boot));
    id::EncodeHex(available->challenge.nonce.data(), available->challenge.nonce.size(), nonce, sizeof(nonce));
    const int length = std::snprintf(text, capacity, "challengeResponse %d %s %s", challenge, boot, nonce);
    return length > 0 && std::size_t(length) < capacity;
}
bool KHZ_AdminValidateConnect(const netadr_t& from, int challenge, int qport, const char* userinfo,
    id::Fingerprint& fingerprint, char* denial, std::size_t capacity) {
    owner(); fingerprint = {};
    auto deny = [&](const char* reason) { ++proofRejected; if (denial && capacity) std::snprintf(denial, capacity, "%s", reason); return false; };
    if (!initialized || !storageReady || !cryptoReady) return deny("Server identity/administration storage unavailable");
    if (const char* reason = KHZ_AdminIpBanMessage(from)) return deny(va("Temporary IP ban: %s", reason));
    if (qport < 0 || qport > 65535) return deny("Invalid client qport");
    id::Proof proof{};
    if (!id::DecodeProof(Info_ValueForKey(userinfo, "khz_proof"), proof)) return deny("cod4x1khz requires a valid local-key proof");
    const auto now = GetTickCount64(); PendingProof* found = nullptr;
    for (auto& pending : pendingProofs) if (pending.used && pending.challenge.challenge == std::uint32_t(challenge) && NET_CompareAdr(pending.address, from) && now - pending.issued <= 30000) { found = &pending; break; }
    if (!found) return deny("Expired or missing local identity challenge");
    auto context = found->challenge; context.qport = std::uint16_t(qport);
    if (!id::Verify(proof, context, fingerprint)) return deny("Invalid local identity signature");
    if (found->admittedSlot >= 0) {
        const int index = found->admittedSlot;
        if (!svs.clients || index >= KHZ_MaxMatchClients || svs.clients[index].header.state != CS_CONNECTED ||
            clients[index].generation != found->admittedGeneration || clients[index].fingerprint != fingerprint ||
            found->proof.publicKey != proof.publicKey || found->proof.signature != proof.signature || svs.clients[index].header.netchan.qport != qport)
            return deny("Local identity challenge was already consumed");
    }
    const auto subject = policy::Subject{fingerprint, 0, ipv4(from), false};
    const auto realNow = unixTime();
    if (const auto* ban = store.IsBanned(subject, realNow)) {
        char message[1024];
        const auto remaining = ban->expires ? (std::min)(ban->expires - realNow, std::int64_t(INT32_MAX)) : -1;
        KHZ_AdminFormatBanMessage(int(remaining), ban->reason, message, sizeof(message));
        return deny(message);
    }
    found->proof = proof; found->admitted = fingerprint; ++proofAccepted; return true;
}
void KHZ_AdminSetConnectingIdentity(client_t* client, const id::Fingerprint& fingerprint) {
    owner(); const int index = slot(client); if (index < 0) return;
    KHZ_AdminResetClient(client); clients[index].fingerprint = fingerprint;
    clients[index].uid = int(id::LocalPlayerId(fingerprint) & 0x7fffffff); if (!clients[index].uid) clients[index].uid = 1;
    if (const auto* admin = store.FindAdmin(fingerprint)) clients[index].undercover = admin->undercover != 0;
}
void KHZ_AdminAdmitClient(client_t* client, const id::Fingerprint& fingerprint) {
    owner(); const int index = slot(client); if (index < 0) return;
    if (clients[index].fingerprint != fingerprint) KHZ_AdminSetConnectingIdentity(client, fingerprint);
    for (auto& pending : pendingProofs) if (pending.used && pending.admitted == fingerprint &&
        pending.challenge.challenge == std::uint32_t(client->challenge) && NET_CompareAdr(pending.address, client->header.netchan.remoteAddress)) {
        pending.admittedSlot = index; pending.admittedGeneration = clients[index].generation; break;
    }
}
void KHZ_AdminResetClient(client_t* client) { owner(); const int index = slot(client); if (index >= 0) { clients[index] = {}; clients[index].generation = ++generationSerial; } }
void KHZ_AdminRecordCommand(client_t* client, std::uint64_t tick) {
    const int index = slot(client); if (index < 0) return; auto& state = clients[index];
    if (tick == state.lastCommandTick) return;
    state.lastCommandTick = tick; state.commandTicks[state.nextCommandTick] = tick;
    state.nextCommandTick = (state.nextCommandTick + 1) % state.commandTicks.size();
    state.commandCount = (std::min)(unsigned(state.commandTicks.size()), state.commandCount + 1);
}
int KHZ_AdminClientPower(const client_t* client) {
    const int index = slot(client); if (index < 0 || KHZ_IsNativeBot(client)) return 0;
    return store.Power(policy::Invoker{clients[index].fingerprint, false, clients[index].temporaryPower});
}
bool KHZ_AdminClientUndercover(const client_t* client) { const int index = slot(client); return index >= 0 && clients[index].undercover; }
const id::Fingerprint* KHZ_AdminClientIdentity(const client_t* client) { const int index = slot(client); return index >= 0 ? &clients[index].fingerprint : nullptr; }
std::uint64_t KHZ_AdminClientGeneration(const client_t* client) { const int index = slot(client); return index >= 0 ? clients[index].generation : 0; }
bool KHZ_AdminReservedSlot(const id::Fingerprint& fingerprint, const char* password) {
    return policy::CanUseReservedSlots(store.Power(fingerprint), sv_privatePassword && sv_privatePassword->current.string[0] &&
        policy::ConstantTimeEqual(password ? password : "", sv_privatePassword->current.string), reservedPower ? reservedPower->current.integer : 0);
}
bool KHZ_AdminAddIpBan(const netadr_t& from, const char* reason, std::int64_t expires) {
    owner(); return ipBanMinutes && ipBanCache.Add(ipv4(from), reason ? reason : "Banned", unixTime(), expires, unsigned(ipBanMinutes->current.integer));
}
void KHZ_AdminRemoveIpBan(const netadr_t& from) { owner(); ipBanCache.Remove(ipv4(from)); }
const char* KHZ_AdminIpBanMessage(const netadr_t& from) { return ipBanCache.Find(ipv4(from), unixTime()); }
bool KHZ_AdminFormatIpBanMessage(const netadr_t& from, char* output, std::size_t capacity) {
    owner(); return ipBanCache.Format(ipv4(from), unixTime(), output, capacity);
}
bool KHZ_AdminTakeNextRotation(char* map, std::size_t mapCapacity, char* gametype, std::size_t gametypeCapacity) {
    owner(); if (!map || !gametype || mapCapacity < 64 || gametypeCapacity < 64 || !sv_mapRotation || !sv_mapRotationCurrent || !sv_gametype) return false;
    policy::Rotation rotation; const bool reset = !sv_mapRotationCurrent->current.string[0];
    const char* text = reset ? sv_mapRotation->current.string : sv_mapRotationCurrent->current.string;
    if (!policy::ParseRotation(text, sv_gametype->current.string, rotation)) return false;
    if (reset && randomRotation && randomRotation->current.enabled && !policy::ShuffleRotation(rotation)) return false;
    char remaining[8192]; if (!policy::SerializeRotation(rotation, 1, remaining, sizeof(remaining))) return false;
    I_strncpyz(map, rotation.maps[0].map, int(mapCapacity)); I_strncpyz(gametype, rotation.maps[0].gametype, int(gametypeCapacity));
    Dvar_SetString(const_cast<dvar_s*>(sv_mapRotationCurrent), remaining); return true;
}
bool KHZ_AdminClientCommand(client_t* client, const char* text) {
    owner(); const int index = slot(client); if (index < 0 || !text) return false;
    std::string_view request(text);
    if (request.substr(0, 6) == "admin ") request.remove_prefix(6);
    else if (!request.empty() && request.front() == '$') request.remove_prefix(1);
    else if (request.substr(0, 6) == "say \"$") { request.remove_prefix(6); if (request.empty() || request.back() != '"') return false; request.remove_suffix(1); }
    else return false;
    if (KHZ_IsNativeBot(client) || client->header.state < CS_CONNECTED || requestCount == requests.size()) { ++deniedCommands; return true; }
    char command[64];
    if (!policy::ValidateCommand(request, command, sizeof(command))) { ++deniedCommands; return true; }
    constexpr std::pair<const char*, const char*> aliases[] = {
        {"authSetAdmin","AdminAddAdminWithPassword"},{"authUnsetAdmin","AdminRemoveAdmin"},
        {"authChangePassword","ChangePassword"},{"authListAdmins","AdminListAdmins"},
        {"cmdpowerlist","AdminListCommands"},{"setCmdMinPower","AdminChangeCommandPower"}
    };
    const char* canonical = command;
    for (const auto& alias : aliases) if (!I_stricmp(command, alias.first)) { canonical = alias.second; break; }
    char rewritten[1024];
    const auto suffix = request.substr(request.find(command) + std::strlen(command));
    const int length = std::snprintf(rewritten, sizeof(rewritten), "%s%.*s", canonical, int(suffix.size()), suffix.data());
    if (length < 0 || unsigned(length) >= sizeof(rewritten) || !playerCanExecute(index, rewritten)) {
        ++deniedCommands; SV_GameSendServerCommand(index, SV_CMD_CAN_IGNORE, "e \"Insufficient power or invalid command\""); return true;
    }
    auto& queued = requests[(requestHead + requestCount++) % requests.size()]; queued.slot = index;
    queued.generation = clients[index].generation; std::memcpy(queued.text, rewritten, length + 1); return true;
}
void KHZ_AdminService() {
    owner(); if (!initialized || com_inServerFrame) return;
    for (unsigned drained = 0; pluginBanCount && drained != 4; ++drained) {
        const auto request = pluginBans[pluginBanHead];
        pluginBanHead = (pluginBanHead + 1) % pluginBans.size(); --pluginBanCount;
        // A rebuilt native plugin is trusted to ban, as in the pinned public
        // SDK. Its actual issuer remains in the record and audit; privilege is
        // not reinterpreted as a slot-number console command at drain time.
        auto authority = request.issuer; authority.console = true;
        const auto result = !storageReady ? policy::Result::StorageError : request.expires == request.submitted ?
            policy::Result::Ok : store.AddBan(authority, request.subject, request.name,
                request.reason, request.submitted, request.expires, false);
        if (result != policy::Result::Ok) { ++pluginBansRejected; continue; }
        ++pluginBansAccepted;
        if (auditCount == audit.size()) ++auditDropped;
        else {
            auto& entry = audit[(auditHead + auditCount++) % audit.size()]; entry.unixTime = request.submitted;
            char issuer[65], target[65];
            id::EncodeHex(request.issuer.identity.data(), request.issuer.identity.size(), issuer, sizeof(issuer));
            id::EncodeHex(request.subject.identity.data(), request.subject.identity.size(), target, sizeof(target));
            std::snprintf(entry.text, sizeof(entry.text), "power=%d issuer=%s name=%s plugin=%s action=Plugin_BanClient target=%s uid=%d expires=%lld reason=%s",
                request.issuerPower, request.issuer.console ? "console/rcon" : issuer, request.issuerName,
                request.plugin, target, request.invokerUid, request.expires, request.reason);
            cleanLog(entry.text);
        }
        persist();
        auto* target = svs.clients && request.slot >= 0 && request.slot < KHZ_MaxMatchClients ? &svs.clients[request.slot] : nullptr;
        if (target && target->header.state >= CS_CONNECTED && policy::SameAdmission(request.admission,
            {clients[request.slot].fingerprint, clients[request.slot].generation}))
        {
            char message[1024];
            const auto remaining = request.expires ? (std::max)(std::int64_t(0), request.expires - unixTime()) : -1;
            KHZ_AdminFormatBanMessage(int(remaining), request.reason, message, sizeof(message));
            SV_DropClient(target, message, true);
        }
        else ++pluginBansReusedSlot;
    }
    const auto previousInvoker = currentInvoker;
    unsigned drained = 0;
    while (requestCount && drained++ < 4) {
        auto request = requests[requestHead]; requestHead = (requestHead + 1) % requests.size(); --requestCount;
        if (!svs.clients || request.slot < 0 || request.slot >= KHZ_MaxMatchClients ||
            svs.clients[request.slot].header.state < CS_CONNECTED || clients[request.slot].generation != request.generation) continue;
        currentInvoker = {clients[request.slot].fingerprint, false, clients[request.slot].temporaryPower};
        if (!playerCanExecute(request.slot, request.text)) { ++deniedCommands; continue; }
        char output[4096]; redirectSlot = request.slot;
        Com_BeginRedirect(output, sizeof(output), redirectOutput);
        ++acceptedCommands; Cmd_ExecuteSingleCommand(0, 0, request.text);
        Com_EndRedirect(); redirectSlot = -1;
    }
    currentInvoker = previousInvoker;
    try { serviceWebadmin(); }
    catch (...) { clearWebadminOwnerContext(); KHZ_WebadminErrorCleanup(); Com_Printf(CON_CHANNEL_SERVER, "Private HTTP service discarded an exceptional request.\n"); }
    flushAudit();
}
void KHZ_AdminErrorCleanup() {
    currentInvoker = {{}, true}; redirectSlot = -1; remoteConsoleContext = false; requestHead = requestCount = 0;
    clearWebadminOwnerContext(); KHZ_WebadminErrorCleanup();
    pluginBanHead = pluginBanCount = 0;
}
bool KHZ_AdminRegisterCommand(const char* name, int defaultPower) {
    owner(); return initialized && name && store.RegisterCommand(name, defaultPower) == policy::Result::Ok;
}
bool KHZ_AdminCanPlayerUseCommand(int index, const char* name) {
    // The public SDK alias Auth_CanPlayerUseCommand queries whitelist only;
    // command dispatch separately combines membership and live minimum power.
    owner(); char command[64];
    return initialized && storageReady && svs.clients && index >= 0 && index < KHZ_MaxMatchClients &&
        svs.clients[index].header.state >= CS_CONNECTED && !KHZ_IsNativeBot(&svs.clients[index]) &&
        name && policy::ValidateCommand(name, command, sizeof(command)) && !std::strcmp(name, command) &&
        Cmd_FindCommand(command) && clients[index].whitelist.Contains(command);
}
bool KHZ_AdminCanInvokerUseCommand(const char* name) { owner(); return invokerCanExecute(name, redirectSlot); }
bool KHZ_AdminWhitelistCommand(int index, const char* name) {
    owner(); char command[64];
    return initialized && storageReady && svs.clients && index >= 0 && index < KHZ_MaxMatchClients &&
        svs.clients[index].header.state >= CS_CONNECTED && !KHZ_IsNativeBot(&svs.clients[index]) &&
        name && policy::ValidateCommand(name, command, sizeof(command)) && !std::strcmp(name, command) &&
        Cmd_FindCommand(command) && store.CommandPower(command) >= 0 && clients[index].whitelist.Add(command);
}
bool KHZ_AdminQueuePluginBan(int index, int minutes, int invokerUid, const char* reason, const char* plugin) {
    owner();
    if (!initialized || !storageReady || !svs.clients || index < 0 || index >= KHZ_MaxMatchClients ||
        svs.clients[index].header.state < CS_CONNECTED || KHZ_IsNativeBot(&svs.clients[index]) ||
        id::IsEmpty(clients[index].fingerprint) || minutes < -1 || minutes > 30 * 24 * 60 || pluginBanCount == pluginBans.size()) {
        ++pluginBansRejected; return false;
    }
    const auto submitted = unixTime();
    const auto seconds = std::int64_t((std::max)(minutes, 0)) * 60;
    if (submitted < 0 || (minutes != -1 && submitted > INT64_MAX - seconds)) { ++pluginBansRejected; return false; }
    auto& request = pluginBans[(pluginBanHead + pluginBanCount++) % pluginBans.size()]; request = {};
    request.subject = subjectFor(&svs.clients[index], clients[index].fingerprint);
    request.admission = {clients[index].fingerprint, clients[index].generation}; request.slot = index;
    request.issuer = currentInvoker; request.issuerPower = store.Power(currentInvoker); request.invokerUid = invokerUid;
    request.submitted = submitted; request.expires = minutes == -1 ? 0 : submitted + seconds;
    I_strncpyz(request.name, svs.clients[index].name[0] ? svs.clients[index].name : "unnamed", sizeof(request.name));
    I_strncpyz(request.reason, reason && *reason ? reason : "Banned by plugin", sizeof(request.reason));
    I_strncpyz(request.issuerName, invokerName(), sizeof(request.issuerName));
    I_strncpyz(request.plugin, plugin ? plugin : "plugin", sizeof(request.plugin));
    cleanLog(request.name); cleanLog(request.reason); cleanLog(request.issuerName); cleanLog(request.plugin);
    return true;
}
void KHZ_AdminPluginLog(const char* text, const char* plugin) {
    owner(); if (auditCount == audit.size()) { ++auditDropped; return; }
    auto& entry = audit[(auditHead + auditCount++) % audit.size()]; entry.unixTime = unixTime();
    char issuer[65]; id::EncodeHex(currentInvoker.identity.data(), currentInvoker.identity.size(), issuer, sizeof(issuer));
    const char* name = invokerName();
    std::snprintf(entry.text, sizeof(entry.text), "power=%d issuer=%s name=%s plugin=%s action=plugin_log text=%s",
        store.Power(currentInvoker), currentInvoker.console ? "console/rcon" : issuer, name, plugin ? plugin : "plugin", text ? text : "");
    cleanLog(entry.text);
}
void KHZ_AdminFormatBanMessage(int remainingSeconds, const char* reason, char* text, std::size_t capacity) {
    owner();
    const bool legacy = (!appealUrl || !appealUrl->current.string[0]) && legacyAppealUrl && legacyAppealUrl->current.string[0];
    const auto* url = legacy ? legacyAppealUrl : appealUrl;
    const auto* hours = legacy ? legacyAppealHours : appealHours;
    policy::FormatBanMessage(remainingSeconds, reason, url ? url->current.string : "",
        hours ? unsigned(hours->current.integer) : 4, text, capacity);
}
int KHZ_AdminInvokerSlot() { owner(); return redirectSlot; }
int KHZ_AdminInvokerPower() { owner(); return store.Power(currentInvoker); }
const char* KHZ_AdminInvokerName(char* output, int capacity) {
    owner(); if (output && capacity > 0) I_strncpyz(output, currentInvoker.console ? "System/Rcon" : invokerName(), capacity); return output;
}
int KHZ_AdminClientUid(const client_t* client) { owner(); const int index = slot(client); return index >= 0 ? clients[index].uid : 0; }
client_t* KHZ_AdminFindClient(const char* handle) { owner(); return findClient(handle); }
bool KHZ_AdminIsOwnedConsoleCommand(const char* name) {
    if (!name || !policy::SafeRotationToken(name)) return false;
    for (const auto& registration : registrations) if (!I_stricmp(name, registration.name)) return true;
    const char* core[] = {"status","serverinfo","systeminfo","dumpuser","map","devmap","map_rotate","map_restart","fast_restart",
        "gametype","heartbeat","killserver","say","tell","scriptUsage","stringUsage","showconfigstring",
        "loadPlugin","unloadPlugin","plugins","pluginInfo"};
    for (const auto* command : core) if (!I_stricmp(name, command)) return true;
    return false;
}
int KHZ_AdminCheckRcon(const netadr_t& from, const char* password) {
    owner(); return int(rconLimiter.Validate(ipv4(from), GetTickCount64(), password ? password : "", rcon_password ? rcon_password->current.string : ""));
}
void KHZ_AdminAuditRcon(const netadr_t& from, const char* command) {
    owner(); char name[64];
    if (!policy::ValidateCommand(command ? command : "", name, sizeof(name))) I_strncpyz(name, "invalid", sizeof(name));
    const auto saved = currentInvoker; const bool savedRemote = remoteConsoleContext;
    currentInvoker = {{}, true}; remoteConsoleContext = true;
    auditEvent(name, NET_AdrToString(from)); currentInvoker = saved; remoteConsoleContext = savedRemote;
}
const char* KHZ_AdminStatsJSON() {
    static char text[1536];
    std::snprintf(text, sizeof(text), "{\"storage_ready\":%s,\"crypto_ready\":%s,\"official_provider\":false,\"admins\":%u,\"bans\":%u,\"proof_accepted\":%llu,\"proof_rejected\":%llu,\"commands_accepted\":%llu,\"commands_denied\":%llu,\"queued\":%u,\"audit_queued\":%u,\"audit_dropped\":%llu,\"plugin_bans_queued\":%u,\"plugin_bans_accepted\":%llu,\"plugin_bans_rejected\":%llu,\"plugin_bans_stale_slots\":%llu,\"webadmin\":%s}",
        storageReady ? "true" : "false", cryptoReady ? "true" : "false", store.AdminCount(), store.BanCount(unixTime()), proofAccepted, proofRejected,
        acceptedCommands, deniedCommands, requestCount, auditCount, auditDropped, pluginBanCount,
        pluginBansAccepted, pluginBansRejected, pluginBansReusedSlot, KHZ_WebadminStatsJSON()); return text;
}

void KHZ_AdminSetRemoteConsoleContext(bool active) { owner(); remoteConsoleContext = active; }

#include "khz_webadmin_adapter.inl"
void KHZ_ScrGetPlayerId(scr_entref_t ref) {
    auto* client = scriptClient(ref, 0, "Usage: self getplayerid();"); const auto* fingerprint = KHZ_AdminClientIdentity(client);
    char text[80];
    if (steam64 && steam64->current.enabled) std::snprintf(text, sizeof(text), "%llu", fingerprint && !id::IsEmpty(*fingerprint) ? id::LocalPlayerId(*fingerprint) : 0);
    else if (fingerprint && !id::IsEmpty(*fingerprint)) { text[0] = 'k'; text[1] = ':'; id::EncodeHex(fingerprint->data(), fingerprint->size(), text + 2, sizeof(text) - 2); }
    else I_strncpyz(text, "[I:0:0]", sizeof(text)); Scr_AddString(text);
}
void KHZ_ScrGetPlayerId64(scr_entref_t ref) {
    auto* client = scriptClient(ref, 0, "Usage: self getplayerid64();"); const auto* fingerprint = KHZ_AdminClientIdentity(client);
    char text[32]; std::snprintf(text, sizeof(text), "%llu", fingerprint && !id::IsEmpty(*fingerprint) ? id::LocalPlayerId(*fingerprint) : 0); Scr_AddString(text);
}
void KHZ_ScrGetSteamId(scr_entref_t ref) { scriptClient(ref, 0, "Usage: self getsteamid();"); Scr_AddString(steam64 && steam64->current.enabled ? "0" : "[I:0:0]"); }
void KHZ_ScrGetSteamId64(scr_entref_t ref) { scriptClient(ref, 0, "Usage: self getsteamid64();"); Scr_AddString("0"); }
void KHZ_ScrGetUid(scr_entref_t ref) { auto* client = scriptClient(ref, 0, "Usage: self getuid();"); Scr_AddInt(clients[slot(client)].uid); }
void KHZ_ScrSetUid(scr_entref_t ref) { auto* client = scriptClient(ref, 1, "Usage: self setuid(<int>);"); clients[slot(client)].uid = Scr_GetInt(0); }
void KHZ_ScrGetPower(scr_entref_t ref) { auto* client = scriptClient(ref, 0, "Usage: self getpower();"); Scr_AddInt(KHZ_AdminClientPower(client)); }
void KHZ_ScrSetPower(scr_entref_t ref) {
    auto* client = scriptClient(ref, 1, "Usage: self setpower(<int>);"); const int power = Scr_GetInt(0);
    if (power < 1 || power > 100 || KHZ_IsNativeBot(client)) Scr_ParamError(0, "Power requires a human and integer1..100"); clients[slot(client)].temporaryPower = power;
}
void KHZ_ScrGetUserinfo(scr_entref_t ref) { auto* client = scriptClient(ref, 1, "Usage: self getuserinfo(<key>);"); Scr_AddString(Info_ValueForKey(client->userinfo, Scr_GetString(0))); }
void KHZ_ScrGetPing(scr_entref_t ref) { auto* client = scriptClient(ref, 0, "Usage: self getping();"); Scr_AddInt(KHZ_IsNativeBot(client) ? 0 : client->ping); }
void KHZ_ScrGetCountedFps(scr_entref_t ref) {
    auto* client = scriptClient(ref, 0, "Usage: self getcountedfps();"); const auto& state = clients[slot(client)];
    unsigned count = 0; if (state.lastCommandTick != UINT64_MAX) for (unsigned i = 0; i != state.commandCount; ++i)
        if (state.lastCommandTick - state.commandTicks[i] < 1000) ++count;
    Scr_AddInt(int(count));
}
void KHZ_ScrGetGeolocation(scr_entref_t ref) {
    scriptClient(ref, 1, "Usage: self getgeolocation(<type>);");
    const int type = Scr_GetInt(0); if (type >= 0 && type <= 3) Scr_AddString(""); else Scr_AddInt(0);
}
void KHZ_ScrSteamGroupMembershipQuery(scr_entref_t ref) {
    scriptClient(ref, 2, "Usage: self steamgroupmembershipquery(<id>, <callback>);");
    Scr_GetString(0); Scr_GetFunc(1); Scr_AddBool(false); // Provider absent: no fabricated membership/callback.
}
