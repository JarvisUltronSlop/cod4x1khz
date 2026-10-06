#include <universal/q_shared.h>
#include "khz_host.h"
#include "khz_admin_server.h"
#include "khz_admin.h"
#include "khz_runtime.h"
#include "khz_plugins.h"
#include "khz_bots.h"
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/threads.h>
#include <qcommon/net_chan_mp.h>
#include <server_mp/server_mp.h>
#include <database/database.h>
#include <win32/win_local.h>
#include <core.hpp>
#include <host_checkpoint_policy.hpp>
#include <host_transport.hpp>
#include <Windows.h>
#include <sddl.h>
#include <bcrypt.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

#ifndef KISAK_DEDICATED
#include <client_mp/client_mp.h>
#include <client/client.h>
#include <ui_mp/ui_mp.h>
#endif

namespace {
using namespace khz::hosting;
enum class Phase { Idle, Starting, Loading, Ready, Stopping, Failed };
const char* const commonRules[] = {
    "sv_hostname", "g_password", "g_allowvote", "sv_voice", "sv_minPing", "sv_maxPing",
    "scr_teambalance", "scr_oldschool", "scr_hardcore", "scr_game_spectatetype", "scr_game_allowkillcam",
    "scr_game_onlyheadshots", "scr_team_fftype", "scr_player_maxhealth", "scr_player_healthregentime",
    "scr_player_suicidespawndelay", "scr_player_forcerespawn", "scr_player_sprinttime", "scr_team_teamkillspawndelay",
    "scr_team_kickteamkillers", "scr_team_teamkillpointloss", "scr_team_teamkillpunish"
};
const char* const modeSuffixes[] = {
    "timelimit", "scorelimit", "roundlimit", "winlimit", "playerrespawndelay", "waverespawndelay",
    "numlives", "bombtimer", "planttime", "defusetime", "roundswitch", "capturelimit", "destroytime",
    "spawntime", "kothmode", "delayPlayer", "delayHot", "delayNeutral", "spawnDelay", "neutral"
};

struct HostState {
    Channel channel;
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    DWORD childPid = 0;
    DWORD parentPid = 0;
    wchar_t pipeName[160]{};
    char token[33]{};
    Packet configuration{};
    Phase phase = Phase::Idle;
    bool initialized = false;
    bool managedServer = false;
    bool authenticated = false;
    bool configured = false;
    bool pendingReady = false;
    bool restoreMod = false;
    bool quitQueued = false;
    bool frontendConnected = false;
    unsigned sentSequence = 0;
    unsigned receivedSequence = 0;
    unsigned serverIdBeforeLoad = 0;
    unsigned transitions = 0;
    unsigned pendingRequest = 0;
    unsigned failures = 0;
    int localClientNum = 0;
    ULONGLONG deadline = 0;
    ULONGLONG nextPoll = 0;
    char previousMod[128]{};
    char error[160]{};
    bool checkpointPaused = false;
    bool checkpointAccepted = false;
    unsigned checkpointRequest = 0;
    std::uint64_t checkpointFixture = 0, checkpointSession = 0;
    std::uint64_t checkpointDiagnosticFixture = 0;
    char checkpointDiagnostic[160]{};
};
// A single local host and one fixed-capacity control channel per process.
// Overlapped I/O never calls gameplay from a worker or allocates in a game tick.
HostState host;
char commandCapture[2048]{};
unsigned commandCaptureSize = 0;
bool commandCaptureTruncated = false;
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
bool beginArchiveProbeOwnedCommand(const Packet&);
void endArchiveProbeOwnedCommand();
#endif
void captureCommandOutput(char* text) {
    const auto available = sizeof(commandCapture) - 1 - commandCaptureSize;
    const auto bytes = (std::min)(std::strlen(text), available);
    std::memcpy(commandCapture + commandCaptureSize, text, bytes);
    commandCaptureSize += static_cast<unsigned>(bytes);
    commandCapture[commandCaptureSize] = 0;
    commandCaptureTruncated |= bytes != std::strlen(text);
}

template<std::size_t N> bool terminated(const char (&value)[N]) { return std::memchr(value, 0, N) != nullptr; }
bool identifier(const char* value, unsigned maxLength, bool map = false) {
    if (!value || !*value) return false;
    unsigned length = 0;
    for (; *value; ++value) {
        const unsigned char ch = static_cast<unsigned char>(*value);
        if (++length >= maxLength || !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return !map || length >= 4;
}
bool tokenValid(const char* value) {
    if (!value || std::strlen(value) != 32) return false;
    for (unsigned i = 0; i < 32; ++i) if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
    return true;
}
bool sameToken(const char* value) {
    unsigned mismatch = 0;
    for (unsigned i = 0; i < sizeof(host.token); ++i) mismatch |= static_cast<unsigned char>(value[i] ^ host.token[i]);
    return mismatch == 0;
}
const char* phaseName(Phase value) {
    switch (value) {
    case Phase::Idle: return "idle"; case Phase::Starting: return "starting";
    case Phase::Loading: return "loading"; case Phase::Ready: return "ready";
    case Phase::Stopping: return "stopping"; default: return "failed";
    }
}
void closeHandle(HANDLE& handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    handle = nullptr;
}
void closeChannel() { khz::hosting::Close(host.channel); }
void closeOwnedProcess() {
    // This handle refers solely to the child created by this controller. Closing
    // the job cannot affect an independently launched or remote server.
    closeHandle(host.job);
    closeHandle(host.process);
    host.childPid = 0;
    host.authenticated = false;
}
bool makeEvents(Channel& channel) { return khz::hosting::MakeEvents(channel); }
Packet packet(HostMessage type) {
    Packet result{};
    result.magic = WireMagic; result.version = WireVersion; result.message = type;
    result.sequence = ++host.sentSequence; result.protocol = khz::Protocol; result.pid = GetCurrentProcessId();
    std::memcpy(result.token, host.token, sizeof(result.token));
    return result;
}
bool enqueue(const Packet& value, bool mandatory = true) {
    auto& channel = host.channel;
    if (channel.writeIndex - channel.readIndex >= QueueCapacity) {
        if (mandatory) channel.failed = true;
        return false;
    }
    channel.outgoing[channel.writeIndex++ % QueueCapacity] = value;
    return true;
}
bool validPacket(const Packet& value) {
    if (value.magic != WireMagic || value.version != WireVersion || value.protocol != khz::Protocol ||
        !terminated(value.token) || !sameToken(value.token) || value.sequence <= host.receivedSequence ||
        !terminated(value.map) || !terminated(value.gametype) || !terminated(value.error) ||
        !terminated(value.command) || !terminated(value.output) || value.ruleCount > MaxRules) return false;
    if (value.pid != (host.managedServer ? host.parentPid : host.childPid)) return false;
    host.receivedSequence = value.sequence;
    return true;
}
bool allowedRule(const char* name, const char* gametype) {
    for (const auto* allowed : commonRules) if (!I_stricmp(name, allowed)) return true;
    char candidate[64];
    for (const auto* suffix : modeSuffixes) {
        std::snprintf(candidate, sizeof(candidate), "scr_%s_%s", gametype, suffix);
        if (!I_stricmp(name, candidate)) return true;
    }
    return false;
}
void reportFrontendError(const char* message) {
#ifndef KISAK_DEDICATED
    Dvar_SetStringByName("com_errorMessage", message);
    UI_SetActiveMenu(host.localClientNum, UIMENU_MAIN);
#endif
    Com_PrintError(CON_CHANNEL_SYSTEM, "Local match: %s\n", message);
}
void fail(const char* message) {
    if (host.phase == Phase::Failed) return;
    ++host.failures;
    I_strncpyz(host.error, message, sizeof(host.error));
    host.phase = Phase::Failed;
    if (host.managedServer) {
        if (host.channel.connected && !host.channel.failed) {
            auto response = packet(HostMessage::Failure);
            I_strncpyz(response.error, message, sizeof(response.error)); enqueue(response);
        }
        host.quitQueued = true;
        host.deadline = GetTickCount64() + 250;
    } else {
#ifndef KISAK_DEDICATED
        const bool disconnect = KHZ_IsOwnedHostConnection();
#endif
        closeOwnedProcess(); closeChannel();
#ifndef KISAK_DEDICATED
        if (disconnect) Cbuf_AddText(host.localClientNum, "disconnect\n");
#endif
        reportFrontendError(message);
    }
}
bool pumpConnect() { return khz::hosting::PumpConnect(host.channel, host.childPid); }
void pumpWrite() { khz::hosting::PumpWrite(host.channel); }
bool receive(Packet& value) { if (!khz::hosting::Receive(host.channel, value)) return false; if (!validPacket(value)) { host.channel.failed = true; return false; } return true; }
bool createPrivatePipe() { return khz::hosting::OpenServer(host.channel, host.pipeName); }
bool newToken() { return khz::hosting::NewToken(host.token); }
bool captureRules(Packet& value) {
    auto capture = [&](const char* name) {
        if (!Dvar_FindVar(name)) return true;
        if (value.ruleCount >= MaxRules) return false;
        const char* current = Dvar_GetVariantString(name);
        if (!current || std::strlen(current) >= sizeof(Rule::value)) return false;
        auto& rule = value.rules[value.ruleCount++];
        I_strncpyz(rule.name, name, sizeof(rule.name)); I_strncpyz(rule.value, current, sizeof(rule.value));
        return true;
    };
    for (const auto* name : commonRules) if (!capture(name)) return false;
    char name[64];
    for (const auto* suffix : modeSuffixes) {
        std::snprintf(name, sizeof(name), "scr_%s_%s", value.gametype, suffix);
        if (!capture(name)) return false;
    }
    return true;
}
bool validConfiguration(const Packet& value) {
    if (!identifier(value.map, sizeof(value.map), true) || !identifier(value.gametype, sizeof(value.gametype)) ||
        value.port < 1024 || value.port > 65535 || value.dedicated > 2 || value.bots > 15 || value.difficulty > 3 || value.maxClients < 1 || value.maxClients > 16 || value.manager > 1) return false;
    for (unsigned i = 0; i < value.ruleCount; ++i) {
        const auto& rule = value.rules[i];
        if (!terminated(rule.name) || !terminated(rule.value) || !allowedRule(rule.name, value.gametype)) return false;
        for (unsigned j = 0; j < i; ++j) if (!I_stricmp(rule.name, value.rules[j].name)) return false;
    }
    return true;
}
bool mapAvailable(const char* map) {
    // This component hosts the fastfile runtime prepared by the project. Do
    // the actual stock map existence check on the server owner before queuing
    // a transition, rather than treating a syntactically valid name as loaded.
    return IsFastFileLoad() && (DB_FileSize(map, 0) > 0 || DB_FileSize(map, 1) > 0);
}
void requestMap(const char* map, const char* gametype, bool restart) {
    host.serverIdBeforeLoad = static_cast<unsigned>(sv_serverId_value);
    host.pendingReady = true;
    host.phase = Phase::Loading;
    host.deadline = GetTickCount64() + ReadyTimeout;
    if (gametype && *gametype) Dvar_SetStringByName("g_gametype", gametype);
    Cbuf_AddText(0, restart ? "map_restart\n" : va("map %s\n", map));
}
void handleServer(const Packet& value) {
    if (!host.authenticated) { fail("Unexpected host control before authentication."); return; }
    switch (value.message) {
    case HostMessage::Configure:
        if (host.configured || !validConfiguration(value) || !mapAvailable(value.map) || value.port != static_cast<unsigned>(Dvar_GetInt("net_port"))) {
            fail("Invalid local match configuration."); return;
        }
        host.configuration = value;
        for (unsigned i = 0; i < value.ruleCount; ++i)
            Dvar_SetFromStringByNameFromSource(value.rules[i].name, value.rules[i].value, DVAR_SOURCE_INTERNAL);
        Dvar_SetIntByName("sv_maxclients", int(value.maxClients)); Dvar_SetIntByName("ui_maxclients", int(value.maxClients));
        Dvar_SetIntByName("sv_fps", 1000); Dvar_SetIntByName("sv_maxRate", 2000000);
        Dvar_SetIntByName("net_lanauthorize", 0); Dvar_SetIntByName("sv_punkbuster", 0);
        Dvar_SetIntByName("khz_bot_count", static_cast<int>(value.bots));
        Dvar_SetIntByName("khz_bot_difficulty", static_cast<int>(value.difficulty));
        Dvar_SetBoolByName("khz_bots", true);
        // Developer fixtures explicitly supply their credential in the child's
        // environment. No benchmark password is compiled into distributed engines.
        if (Dvar_GetBool("khz_benchmark") && Dvar_GetBool("khz_test_controls") &&
            !I_stricmp(Dvar_GetString("net_ip"), "127.0.0.1")) {
            char password[128]{};
            const auto length = GetEnvironmentVariableA("KHZ_BENCHMARK_RCON_PASSWORD", password, sizeof(password));
            if (length && length < sizeof(password)) Dvar_SetStringByName("rcon_password", password);
            Dvar_SetBoolByName("sv_pure", true);
        }
        host.configured = true;
        host.pendingRequest = value.sequence;
        if (value.manager) {
            char map[64]{}, mode[64]{};
            if (!KHZ_AdminTakeNextRotation(map, sizeof(map), mode, sizeof(mode)) || !mapAvailable(map)) {
                fail("The configured map/mode rotation is invalid or unavailable."); return;
            }
            I_strncpyz(host.configuration.map, map, sizeof(host.configuration.map));
            I_strncpyz(host.configuration.gametype, mode, sizeof(host.configuration.gametype));
            requestMap(map, mode, false);
        } else requestMap(value.map, value.gametype, false);
        break;
    case HostMessage::Map:
        if (host.checkpointPaused) { fail("Discard the benchmark checkpoint before changing maps."); return; }
        if (!host.configured || host.phase != Phase::Ready || !identifier(value.map, sizeof(value.map), true) ||
            (*value.gametype && !identifier(value.gametype, sizeof(value.gametype))) || !mapAvailable(value.map)) { fail("Invalid local map transition."); return; }
        I_strncpyz(host.configuration.map, value.map, sizeof(host.configuration.map));
        if (*value.gametype) I_strncpyz(host.configuration.gametype, value.gametype, sizeof(host.configuration.gametype));
        host.pendingRequest = value.sequence;
        requestMap(value.map, value.gametype, false); break;
    case HostMessage::Restart:
        if (host.checkpointPaused) { fail("Discard the benchmark checkpoint before restarting maps."); return; }
        if (!host.configured || host.phase != Phase::Ready) { fail("Local match is not ready to restart."); return; }
        host.pendingRequest = value.sequence;
        requestMap(host.configuration.map, nullptr, true); break;
    case HostMessage::Stop:
        host.checkpointPaused = false;
        host.phase = Phase::Stopping;
        host.quitQueued = true; host.deadline = GetTickCount64() + 100; break;
    case HostMessage::Status: {
        auto reply = packet(HostMessage::StatusResult); reply.request = value.sequence;
        reply.tick = KHZ_ServerTickNumber(); reply.session = KHZ_CurrentServerSession();
        reply.fixture = GetTickCount64(); // Status clock belongs to the server, independent of GUI polling jitter.
        reply.port = static_cast<unsigned>(Dvar_GetInt("net_port"));
        reply.maxClients = sv_maxclients ? sv_maxclients->current.integer : 0;
        for (const auto* name : {"sv_fps", "net_ip", "scr_hardcore", "scr_player_maxhealth", "scr_dm_scorelimit", "scr_dm_timelimit", "scr_war_scorelimit", "scr_war_timelimit", "sv_mapRotationCurrent"}) {
            auto& rule = reply.rules[reply.ruleCount++];
            I_strncpyz(rule.name, name, sizeof(rule.name));
            I_strncpyz(rule.value, Dvar_GetVariantString(name), sizeof(rule.value));
        }
        I_strncpyz(reply.map, Dvar_GetString("mapname"), sizeof(reply.map));
        I_strncpyz(reply.gametype, Dvar_GetString("g_gametype"), sizeof(reply.gametype));
        reply.bots = Dvar_GetInt("khz_bot_count"); reply.difficulty = Dvar_GetInt("khz_bot_difficulty");
        if (svs.clients && sv_maxclients) for (int i = 0; i < (std::min)(16, sv_maxclients->current.integer); ++i) {
            const auto& client = svs.clients[i]; if (client.header.state < CS_CONNECTED) continue;
            auto& player = reply.players[reply.playerCount++]; player.slot = i;
            player.generation = KHZ_AdminClientGeneration(&client); player.bot = KHZ_IsNativeBot(&client);
            player.power = KHZ_AdminClientPower(&client); player.ping = client.ping;
            I_strncpyz(player.name, client.name, sizeof(player.name));
            if (const auto* identity = KHZ_AdminClientIdentity(&client))
                khz::identity::EncodeHex(identity->data(), identity->size(), player.identity, sizeof(player.identity));
        }
        enqueue(reply, false); break;
    }
    case HostMessage::Bots:
        if (value.bots > 15 || value.difficulty > 3 || value.bots >= host.configuration.maxClients) break;
        Dvar_SetIntByName("khz_bot_count", int(value.bots));
        Dvar_SetIntByName("khz_bot_difficulty", int(value.difficulty));
        if (svs.clients) {
            int bots = 0;
            for (unsigned i = 0; i < host.configuration.maxClients; ++i)
                if (svs.clients[i].header.state >= CS_CONNECTED && KHZ_IsNativeBot(&svs.clients[i])) ++bots;
            if (bots > int(value.bots)) {
                Dvar_SetIntByName("bots_manage_add", 0);
                KHZ_RemoveBotClients(bots - int(value.bots));
            }
        }
        break;
    case HostMessage::PlayerAction: {
        auto reply = packet(HostMessage::CommandResult); reply.request = value.sequence;
        if (!svs.clients || value.targetSlot < 0 || value.targetSlot >= int(host.configuration.maxClients) ||
            svs.clients[value.targetSlot].header.state < CS_CONNECTED || !value.admission ||
            value.session != KHZ_CurrentServerSession() || KHZ_AdminClientGeneration(&svs.clients[value.targetSlot]) != value.admission) {
            I_strncpyz(reply.output, "Player left or slot was reused; refresh the player list.\n", sizeof(reply.output));
            enqueue(reply, false); break;
        }
        char reason[128]; I_strncpyz(reason, value.command, sizeof(reason));
        if (KHZ_IsNativeBot(&svs.clients[value.targetSlot]) && value.action != khz::hosting::PlayerAction::Kick) {
            I_strncpyz(reply.output, "Bots cannot receive identity bans or administrator privileges.\n", sizeof(reply.output));
            enqueue(reply, false); break;
        }
        for (auto* p = reason; *p; ++p) if (*p == '"' || *p == '\\' || *p == ';' || unsigned(char(*p)) < 32) *p = ' ';
        char command[256]{}; const auto slot = value.targetSlot;
        switch (value.action) {
        case khz::hosting::PlayerAction::Kick: std::snprintf(command, sizeof(command), "onlykick %d %s", slot, reason); break;
        case khz::hosting::PlayerAction::TemporaryBan:
            if (value.minutes < 1 || value.minutes > 525600) break;
            std::snprintf(command, sizeof(command), "tempban %d %um %s", slot, value.minutes, reason); break;
        case khz::hosting::PlayerAction::PermanentBan: std::snprintf(command, sizeof(command), "permban %d %s", slot, reason); break;
        case khz::hosting::PlayerAction::AddAdmin:
            if (value.power > 100) break;
            std::snprintf(command, sizeof(command), "AdminAddAdmin %d %u", slot, value.power); break;
        default: command[0] = 0; break;
        }
        if (*command) { auto action = value; action.message = HostMessage::Command;
            I_strncpyz(action.command, command, sizeof(action.command)); handleServer(action); }
        break;
    }
    case HostMessage::Command: {
        char name[64];
        if (host.checkpointPaused || !host.configured || host.phase != Phase::Ready ||
            !khz::admin::ValidateCommand(value.command, name, sizeof(name)) || !KHZ_AdminIsOwnedConsoleCommand(name)) {
            auto rejected = packet(HostMessage::CommandResult); rejected.request = value.sequence;
            I_strncpyz(rejected.output, "Command is unavailable or unsupported. Use profile settings for configuration changes.\n", sizeof(rejected.output));
            enqueue(rejected, false); return;
        }
        iassert(!com_inServerFrame);
        char redirect[1024]{};
        commandCaptureSize = 0; commandCapture[0] = 0; commandCaptureTruncated = false;
        Com_BeginRedirect(redirect, sizeof(redirect), captureCommandOutput);
        char executableCommand[sizeof(value.command)];
        std::memcpy(executableCommand, value.command, sizeof(executableCommand));
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
        if (!beginArchiveProbeOwnedCommand(value)) {
            Com_EndRedirect(); fail("Invalid direct owned command scope."); return;
        }
#endif
        Cmd_ExecuteSingleCommand(0, 0, executableCommand);
#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
        endArchiveProbeOwnedCommand();
#endif
        Com_EndRedirect();
        auto result = packet(HostMessage::CommandResult); result.request = value.sequence;
        if (commandCaptureTruncated) {
            constexpr char suffix[] = "\n[output truncated]\n";
            const auto at = sizeof(commandCapture) - sizeof(suffix);
            std::memcpy(commandCapture + at, suffix, sizeof(suffix));
        }
        I_strncpyz(result.output, commandCapture, sizeof(result.output));
        // An administrative reply may be dropped under console spam. It must
        // not turn a healthy match into a failed process-control channel.
        enqueue(result, false);
        break;
    }
    case HostMessage::CheckpointPause:
    case HostMessage::CheckpointResume: {
        auto reply = packet(HostMessage::CheckpointFence); reply.request = value.sequence;
        reply.fixture = value.fixture; reply.session = value.session;
        reply.tick = KHZ_ServerTickNumber();
        khz::host_checkpoint::State state{};
        state.benchmark = Dvar_GetBool("khz_benchmark"); state.configured = host.configured;
        state.ready = host.phase == Phase::Ready; state.fixture = value.fixture != 0;
        const auto currentSession = KHZ_CurrentServerSession();
        state.session = value.session == currentSession;
        state.running = com_sv_running->current.enabled; state.game = sv.state == SS_GAME;
        state.inServerFrame = com_inServerFrame; state.clients = svs.clients != nullptr;
        // The first checkpoint fixture excludes arbitrary plugin state and
        // external humans. Both require their own explicit restore providers.
        state.pluginsAbsent = std::strstr(KHZ_PluginStatsJSON(), "\"loaded\":0") != nullptr;
        state.webadminAbsent = !Dvar_GetBool("sv_webadmin");
        int rejectedSlot = -1;
        khz::host_checkpoint::Channel rejectedChannel{};
        if (state.clients) for (int i = 0; i < sv_maxclients->current.integer; ++i) {
            const auto& client = svs.clients[i];
            if (client.header.state >= CS_CONNECTED && !KHZ_IsNativeBot(&client)) {
                ++state.humans;
                const auto& channel = client.header.netchan;
                const khz::host_checkpoint::Channel observed{channel.fragmentLength, channel.unsentLength,
                    channel.unsentBufferSize, channel.unsentFragments != 0, client.header.state == CS_ACTIVE,
                    client.downloading != 0, client.download != 0, client.downloadingWWW != 0};
                const auto failed = khz::host_checkpoint::channelFailure(observed);
                if (failed != khz::host_checkpoint::ChannelFailure::None && rejectedSlot == -1) {
                    rejectedSlot = i; rejectedChannel = observed; state.channel = failed;
                }
            }
        }
        state.queuesQuiet = NET_CheckpointQueuesQuiescent(NS_SERVER);
        state.pauseStateValid = value.message == HostMessage::CheckpointPause ?
            !host.checkpointPaused || host.checkpointFixture == value.fixture :
            host.checkpointPaused && host.checkpointFixture == value.fixture;
        const auto failures = khz::host_checkpoint::failureMask(state);
        const bool accepted = failures == 0;
        if (accepted) {
            host.checkpointPaused = value.message == HostMessage::CheckpointPause;
            host.checkpointFixture = value.fixture; host.checkpointSession = value.session;
        }
        reply.dedicated = accepted ? 1u : 0u;
        reply.bots = host.checkpointPaused ? 1u : 0u;
        if (!accepted) {
            if (rejectedSlot >= 0) khz::host_checkpoint::channelDiagnostic(reply.error, sizeof(reply.error),
                state, rejectedSlot, rejectedChannel);
            else khz::host_checkpoint::stateDiagnostic(reply.error, sizeof(reply.error), state,
                value.fixture, value.session, currentSession, host.checkpointPaused);
        }
        if (!enqueue(reply)) fail("Checkpoint control reply queue is full.");
        break;
    }
    default: fail("Unexpected local server control message."); break;
    }
}
void handleFrontend(const Packet& value) {
#ifndef KISAK_DEDICATED
    switch (value.message) {
    case HostMessage::Hello:
        if (host.authenticated || host.phase != Phase::Starting) { fail("Unexpected local server greeting."); return; }
        host.authenticated = true;
        host.configuration.sequence = ++host.sentSequence;
        host.configuration.pid = GetCurrentProcessId();
        host.pendingRequest = host.configuration.sequence;
        if (!enqueue(host.configuration)) { fail("Local server configuration queue is full."); return; }
        host.phase = Phase::Loading; break;
    case HostMessage::Ready:
        if (!host.authenticated || (host.phase != Phase::Loading && host.phase != Phase::Ready) ||
            !identifier(value.map, sizeof(value.map), true) || !identifier(value.gametype, sizeof(value.gametype)) ||
            value.port != host.configuration.port || (host.phase == Phase::Loading &&
                (value.request != host.pendingRequest || I_stricmp(value.map, host.configuration.map) ||
                 I_stricmp(value.gametype, host.configuration.gametype))) ||
            (host.phase == Phase::Ready && value.request != 0)) { fail("Local server readiness did not match this match."); return; }
        I_strncpyz(host.configuration.map, value.map, sizeof(host.configuration.map));
        I_strncpyz(host.configuration.gametype, value.gametype, sizeof(host.configuration.gametype));
        host.pendingRequest = 0;
        host.phase = Phase::Ready;
        ++host.transitions;
        if (!host.frontendConnected) {
            host.frontendConnected = true;
            Cbuf_AddText(host.localClientNum, va("connect 127.0.0.1:%u\n", value.port));
        }
        break;
    case HostMessage::Failure:
        // Peer text is never copied to logs/JSON as a format string.
        fail("The local server could not start or load this match."); break;
    case HostMessage::CommandResult:
        if (!host.authenticated || !value.request || value.request > host.sentSequence) {
            fail("Unmatched owned-server console response."); return;
        }
        Com_Printf(CON_CHANNEL_DONT_FILTER, "%s", value.output); break;
    case HostMessage::StatusResult: break;
    case HostMessage::CheckpointFence:
        if (!host.authenticated || !host.checkpointRequest || value.request != host.checkpointRequest ||
            value.fixture != host.checkpointFixture || value.session != host.checkpointSession ||
            value.dedicated > 1 || value.bots > 1) {
            host.checkpointDiagnosticFixture = host.checkpointFixture;
            std::snprintf(host.checkpointDiagnostic, sizeof(host.checkpointDiagnostic),
                "reply req=%x/%x f=%llx/%llx s=%llx/%llx auth=%u bits=%x/%x",
                value.request, host.checkpointRequest, value.fixture, host.checkpointFixture, value.session,
                host.checkpointSession, unsigned(host.authenticated), value.dedicated, value.bots);
            fail("Unmatched benchmark checkpoint fence."); return;
        }
        host.checkpointDiagnosticFixture = value.fixture;
        I_strncpyz(host.checkpointDiagnostic, value.error, sizeof(host.checkpointDiagnostic));
        host.checkpointRequest = 0;
        host.checkpointAccepted = value.dedicated == 1;
        host.checkpointPaused = value.bots == 1;
        break;
    default: fail("Unexpected local client control message."); break;
    }
#endif
}
void jsonEscape(const char* input, char* output, std::size_t size) {
    std::size_t used = 0;
    for (; *input && used + 6 < size; ++input) {
        const unsigned char ch = static_cast<unsigned char>(*input);
        if (ch == '"' || ch == '\\') { output[used++] = '\\'; output[used++] = static_cast<char>(ch); }
        else if (ch < 32 || ch > 126) { const int count = std::snprintf(output + used, size - used, "\\u%04x", ch); used += count; }
        else output[used++] = static_cast<char>(ch);
    }
    output[used] = 0;
}
}

bool KHZ_IsManagedHostProcess() {
    // Safe before engine/dvar initialization; the authenticated named-pipe
    // handshake later establishes actual ownership. This only hides startup UI.
    const char* command = GetCommandLineA();
    return command && std::strstr(command, "+set khz_host_parent ") && std::strstr(command, "+set khz_host_token ");
}
void KHZ_InitHost() {
    iassert(Sys_IsMainThread());
    if (host.initialized) return;
    host.initialized = true;
    Dvar_RegisterString("khz_host_pipe", "", DVAR_INIT, "Private local-host control pipe");
    Dvar_RegisterString("khz_host_token", "", DVAR_INIT, "Private local-host session token");
    Dvar_RegisterInt("khz_host_parent", 0, 0, 2147483647, DVAR_INIT, "Owning client process");
    for (const char* name : { "khz_host_pipe", "khz_host_token", "khz_host_parent" }) Com_StartupVariable(name);
    if (!com_dedicated || !com_dedicated->current.integer || !Dvar_GetInt("khz_host_parent")) return;
    const auto* token = Dvar_GetString("khz_host_token");
    const auto* pipe = Dvar_GetString("khz_host_pipe");
    if (!tokenValid(token) || !pipe || !*pipe || std::strlen(pipe) >= sizeof(host.pipeName) / sizeof(wchar_t)) {
        host.managedServer = true; fail("Invalid managed-server startup parameters."); return;
    }
    host.managedServer = true;
    host.parentPid = static_cast<DWORD>(Dvar_GetInt("khz_host_parent"));
    I_strncpyz(host.token, token, sizeof(host.token));
    wchar_t expected[160];
    std::swprintf(expected, ARRAY_COUNT(expected), L"%ls%lu-%hs", PipePrefix, host.parentPid, token);
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pipe, -1, host.pipeName, ARRAY_COUNT(host.pipeName));
    if (!length || std::wcscmp(host.pipeName, expected)) { fail("Invalid managed-server pipe name."); return; }
    host.channel.pipe = CreateFileW(host.pipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (host.channel.pipe == INVALID_HANDLE_VALUE || !makeEvents(host.channel)) { fail("Could not connect to the owning client."); return; }
    ULONG parent = 0;
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!GetNamedPipeServerProcessId(host.channel.pipe, &parent) || parent != host.parentPid ||
        !SetNamedPipeHandleState(host.channel.pipe, &mode, nullptr, nullptr)) { fail("The local host owner did not match."); return; }
    host.channel.connected = host.channel.peerVerified = true;
    host.authenticated = true;
    host.phase = Phase::Starting;
    host.deadline = GetTickCount64() + ReadyTimeout;
    enqueue(packet(HostMessage::Hello));
}

bool KHZ_StartLocalHost(int localClientNum, const char* map, const char* gametype, int dedicatedMode) {
    iassert(Sys_IsMainThread());
#ifdef KISAK_DEDICATED
    return false;
#else
    KHZ_InitHost();
    if (localClientNum != 0 || !identifier(map, 64, true) || !identifier(gametype, 32) || dedicatedMode < 0 || dedicatedMode > 2) {
        reportFrontendError("Select a valid local map and game mode."); return false;
    }
    if (host.process || host.managedServer) { reportFrontendError("A local match is already starting or running."); return false; }
    closeChannel();
    host.authenticated = host.configured = host.pendingReady = host.frontendConnected = false;
    host.sentSequence = host.receivedSequence = 0;
    host.error[0] = 0; host.phase = Phase::Starting; host.localClientNum = localClientNum;
    if (!newToken()) { fail("Could not create a private local match token."); return false; }
    std::swprintf(host.pipeName, ARRAY_COUNT(host.pipeName), L"%ls%lu-%hs", PipePrefix, GetCurrentProcessId(), host.token);
    if (!createPrivatePipe()) { fail("Could not create the private local match channel."); return false; }
    auto configuration = packet(HostMessage::Configure);
    I_strncpyz(configuration.map, map, sizeof(configuration.map));
    I_strncpyz(configuration.gametype, gametype, sizeof(configuration.gametype));
    configuration.dedicated = static_cast<unsigned>(dedicatedMode);
    configuration.bots = static_cast<unsigned>((std::clamp)(Dvar_GetInt("ui_khz_botCount"), 0, 15));
    configuration.difficulty = static_cast<unsigned>((std::clamp)(Dvar_GetInt("ui_khz_botDifficulty"), 0, 3));
    // Pick a free UDP port, then validate the actual port in the owner's Ready
    // reply. A bind race fails startup rather than attaching to another server.
    const SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int addressSize = sizeof(address);
    if (probe == INVALID_SOCKET || bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
        getsockname(probe, reinterpret_cast<sockaddr*>(&address), &addressSize)) {
        if (probe != INVALID_SOCKET) closesocket(probe);
        fail("Could not reserve a port for the local match."); return false;
    }
    configuration.port = ntohs(address.sin_port); closesocket(probe);
    if (!captureRules(configuration)) { fail("A selected game setting exceeded the local host limits."); return false; }
    host.configuration = configuration;
    I_strncpyz(host.previousMod, Dvar_GetString("fs_game"), sizeof(host.previousMod));
    host.restoreMod = true;
    wchar_t runtime[260];
    if (!MultiByteToWideChar(CP_ACP, 0, Sys_DefaultInstallPath(), -1, runtime, ARRAY_COUNT(runtime))) {
        fail("The project runtime path is unavailable."); return false;
    }
    wchar_t executable[300];
#ifdef _WIN64
    std::swprintf(executable, ARRAY_COUNT(executable), L"%ls\\x64\\cod4x1khz_server.exe", runtime);
#else
    std::swprintf(executable, ARRAY_COUNT(executable), L"%ls\\cod4x1khz_server.exe", runtime);
#endif
    if (GetFileAttributesW(executable) == INVALID_FILE_ATTRIBUTES) { fail("Build the project server before starting a local match."); return false; }
    wchar_t command[1024];
    const int commandLength = std::swprintf(command, ARRAY_COUNT(command),
        L"\"%ls\" allowdupe +set dedicated 1 +set khz_host_parent %lu +set khz_host_token %hs +set khz_host_pipe \"%ls\" +set net_ip %ls +set net_port %u +set khz_benchmark %u +set khz_test_controls %u +set khz_profile_scripts %u +set developer %u",
        executable, GetCurrentProcessId(), host.token, host.pipeName, dedicatedMode ? L"0.0.0.0" : L"127.0.0.1", configuration.port,
        Dvar_GetBool("khz_benchmark") ? 1u : 0u,
        Dvar_GetBool("khz_benchmark") && Dvar_GetBool("khz_test_controls") ? 1u : 0u,
        Dvar_GetBool("khz_benchmark") && Dvar_GetBool("khz_profile_scripts") ? 1u : 0u,
        Dvar_GetBool("khz_benchmark") && Dvar_GetBool("khz_profile_scripts") ? 1u : 0u);
    if (commandLength <= 0 || commandLength >= 1000) { fail("The local host startup path is too long."); return false; }
    std::string spawnError;
    if (!khz::hosting::SpawnOwned(executable, command, runtime, host.job, host.process, host.childPid, spawnError)) {
        fail(spawnError.c_str()); return false;
    }
    host.deadline = GetTickCount64() + ReadyTimeout;
    host.nextPoll = 0;
    Com_Printf(CON_CHANNEL_UI, "Starting local %s on %s with up to %u bots.\n", gametype, map, configuration.bots);
    return true;
#endif
}

void KHZ_HostServerTick() {
    iassert(Sys_IsMainThread());
    if (!host.managedServer || !com_sv_running || !com_sv_running->current.enabled ||
        sv.state != SS_GAME || !sv_mapname || !host.configured) return;
    if (!host.pendingReady && host.phase != Phase::Ready) return;
    if (static_cast<unsigned>(sv_serverId_value) == host.serverIdBeforeLoad) return;
    if (host.pendingReady && (I_stricmp(sv_mapname->current.string, host.configuration.map) ||
        I_stricmp(Dvar_GetString("g_gametype"), host.configuration.gametype))) {
        fail("The local server loaded a different map or game mode."); return;
    }
    auto ready = packet(HostMessage::Ready);
    I_strncpyz(ready.map, sv_mapname->current.string, sizeof(ready.map));
    I_strncpyz(ready.gametype, Dvar_GetString("g_gametype"), sizeof(ready.gametype));
    ready.port = static_cast<unsigned>(Dvar_GetInt("net_port"));
    ready.request = host.pendingReady ? host.pendingRequest : 0;
    ready.bots = host.configuration.bots; ready.difficulty = host.configuration.difficulty;
    if (!enqueue(ready)) { fail("The local server readiness queue is full."); return; }
    I_strncpyz(host.configuration.map, ready.map, sizeof(host.configuration.map));
    I_strncpyz(host.configuration.gametype, ready.gametype, sizeof(host.configuration.gametype));
    host.serverIdBeforeLoad = static_cast<unsigned>(sv_serverId_value);
    host.pendingRequest = 0; host.pendingReady = false; host.phase = Phase::Ready; ++host.transitions;
}
void KHZ_HostPoll() {
    iassert(Sys_IsMainThread());
    if (!host.initialized || (!host.process && !host.managedServer && !host.restoreMod)) return;
    const auto now = GetTickCount64();
    if (now < host.nextPoll) return;
    host.nextPoll = now + 10;
#ifndef KISAK_DEDICATED
    if (host.restoreMod && !host.process && CL_GetLocalClientConnectionState(host.localClientNum) <= CA_DISCONNECTED) {
        Dvar_SetStringByName("fs_game", host.previousMod); host.restoreMod = false;
    }
#endif
    if (!host.managedServer && host.process && WaitForSingleObject(host.process, 0) == WAIT_OBJECT_0) {
        const bool expected = host.phase == Phase::Stopping;
        closeOwnedProcess(); closeChannel();
        if (expected) host.phase = Phase::Idle;
        else fail("The local server stopped before the match ended.");
        return;
    }
    pumpConnect(); pumpWrite();
    Packet incoming{};
    for (unsigned i = 0; i < 4 && receive(incoming); ++i) {
        if (host.managedServer) handleServer(incoming); else handleFrontend(incoming);
    }
    if (host.channel.failed && host.phase != Phase::Stopping && host.phase != Phase::Failed) fail("The private local server channel closed.");
    if (host.managedServer) KHZ_HostServerTick();
    pumpWrite();
    if ((host.phase == Phase::Starting || host.phase == Phase::Loading) && now >= host.deadline) fail("The server did not become ready within 120 seconds.");
    if (host.phase == Phase::Stopping && !host.managedServer && now >= host.deadline) {
        closeOwnedProcess(); closeChannel(); host.phase = Phase::Idle;
    }
    if (host.managedServer && host.quitQueued && (now >= host.deadline ||
        (!host.channel.writing && host.channel.readIndex == host.channel.writeIndex))) {
        host.quitQueued = false; Cbuf_AddText(0, "quit\n");
    }
}
void KHZ_StopOwnedHost(bool) {
    iassert(Sys_IsMainThread());
    if (!host.process || host.managedServer || host.phase == Phase::Stopping) return;
    if (host.channel.connected && host.authenticated && !host.channel.failed) enqueue(packet(HostMessage::Stop));
    host.phase = Phase::Stopping;
    host.deadline = GetTickCount64() + StopTimeout;
    host.nextPoll = 0;
    pumpWrite();
}
void KHZ_ShutdownHost() {
    iassert(Sys_IsMainThread());
    if (host.process && !host.managedServer && host.channel.connected && host.authenticated && !host.channel.failed) {
        enqueue(packet(HostMessage::Stop)); pumpWrite();
    }
    closeOwnedProcess(); closeChannel();
}
bool KHZ_HostMap(const char* map, const char* gametype) {
    iassert(Sys_IsMainThread());
    if (!host.process || host.managedServer || host.phase != Phase::Ready || !host.authenticated ||
        !identifier(map, sizeof(host.configuration.map), true) || (gametype && *gametype && !identifier(gametype, sizeof(host.configuration.gametype)))) return false;
    auto value = packet(HostMessage::Map);
    I_strncpyz(value.map, map, sizeof(value.map));
    if (gametype) I_strncpyz(value.gametype, gametype, sizeof(value.gametype));
    if (!enqueue(value)) return false;
    host.pendingRequest = value.sequence;
    I_strncpyz(host.configuration.map, map, sizeof(host.configuration.map));
    if (gametype && *gametype) I_strncpyz(host.configuration.gametype, gametype, sizeof(host.configuration.gametype));
    host.phase = Phase::Loading; host.deadline = GetTickCount64() + ReadyTimeout; host.nextPoll = 0;
    return true;
}
namespace {
bool requestCheckpointFence(HostMessage type, std::uint64_t fixture, std::uint64_t session, unsigned timeout) {
#ifdef KISAK_DEDICATED
    return false;
#else
    iassert(Sys_IsMainThread());
    host.checkpointDiagnosticFixture = fixture; host.checkpointDiagnostic[0] = 0;
    if (!fixture || !session || !timeout || timeout > 5000 || !Dvar_GetBool("khz_benchmark") ||
        !host.process || host.managedServer || !host.authenticated || host.phase != Phase::Ready ||
        host.checkpointRequest || host.channel.failed) {
        std::snprintf(host.checkpointDiagnostic, sizeof(host.checkpointDiagnostic),
            "front f=%llx s=%llx ms=%u bench=%u owned=%u managed=%u auth=%u ready=%u req=%u failed=%u",
            fixture, session, timeout, unsigned(Dvar_GetBool("khz_benchmark")), unsigned(host.process != nullptr),
            unsigned(host.managedServer), unsigned(host.authenticated), unsigned(host.phase == Phase::Ready),
            host.checkpointRequest, unsigned(host.channel.failed));
        return false;
    }
    auto value = packet(type); value.fixture = fixture; value.session = session;
    host.checkpointFixture = fixture; host.checkpointSession = session;
    host.checkpointAccepted = false; host.checkpointRequest = value.sequence;
    if (!enqueue(value)) {
        host.checkpointRequest = 0;
        I_strncpyz(host.checkpointDiagnostic, "frontend_control_enqueue_failed", sizeof(host.checkpointDiagnostic));
        return false;
    }
    const auto deadline = GetTickCount64() + timeout;
    do {
        host.nextPoll = 0; KHZ_HostPoll();
        if (!host.checkpointRequest) {
            const bool accepted = host.checkpointAccepted && host.checkpointPaused == (type == HostMessage::CheckpointPause);
            if (!accepted && !host.checkpointDiagnostic[0])
                std::snprintf(host.checkpointDiagnostic, sizeof(host.checkpointDiagnostic),
                    "reply_state accepted=%u paused=%u requested_pause=%u", unsigned(host.checkpointAccepted),
                    unsigned(host.checkpointPaused), unsigned(type == HostMessage::CheckpointPause));
            return accepted;
        }
        if (host.phase != Phase::Ready || host.channel.failed) {
            if (!host.checkpointDiagnostic[0]) std::snprintf(host.checkpointDiagnostic, sizeof(host.checkpointDiagnostic),
                "frontend_channel_transition ready=%u channel_failed=%u request=%u", unsigned(host.phase == Phase::Ready),
                unsigned(host.channel.failed), host.checkpointRequest);
            return false;
        }
        Sleep(1);
    } while (GetTickCount64() < deadline);
    // A delayed acknowledgement can still arrive. Retain the pending request
    // and leave the caller's simulation owner parked until explicit recovery.
    std::snprintf(host.checkpointDiagnostic, sizeof(host.checkpointDiagnostic),
        "fence_reply_timeout request=%u fixture=%llu session=%llu", host.checkpointRequest, fixture, session);
    return false;
#endif
}
}
bool KHZ_HostCheckpointPause(std::uint64_t fixture, std::uint64_t session, unsigned timeout) {
    return requestCheckpointFence(HostMessage::CheckpointPause, fixture, session, timeout);
}
bool KHZ_HostCheckpointResume(std::uint64_t fixture, unsigned timeout) {
    if (fixture != host.checkpointFixture) return false;
    return requestCheckpointFence(HostMessage::CheckpointResume, fixture, host.checkpointSession, timeout);
}
bool KHZ_HostCheckpointFenced(std::uint64_t fixture, std::uint64_t session) {
    return fixture && session && host.checkpointAccepted && !host.checkpointRequest &&
        host.checkpointPaused && fixture == host.checkpointFixture && session == host.checkpointSession;
}
bool KHZ_ServerCheckpointPaused() { return host.managedServer && host.checkpointPaused; }
const char* KHZ_HostCheckpointFailure(std::uint64_t fixture) {
    iassert(Sys_IsMainThread());
    return fixture && fixture == host.checkpointDiagnosticFixture ? host.checkpointDiagnostic : "";
}
bool KHZ_HostRestart() {
    iassert(Sys_IsMainThread());
    if (!host.process || host.managedServer || host.phase != Phase::Ready || !host.authenticated) return false;
    const auto value = packet(HostMessage::Restart);
    if (!enqueue(value)) return false;
    host.pendingRequest = value.sequence;
    host.phase = Phase::Loading; host.deadline = GetTickCount64() + ReadyTimeout; host.nextPoll = 0;
    return true;
}
bool KHZ_HasOwnedHost() { return host.process && !host.managedServer; }
bool KHZ_HostConsoleCommand(const char* command) {
    iassert(Sys_IsMainThread());
    if (!KHZ_HasOwnedHost() || !command) return false;
    char name[64];
    if (!khz::admin::ValidateCommand(command, name, sizeof(name)) || !KHZ_AdminIsOwnedConsoleCommand(name)) return false;
    if (host.phase != Phase::Ready || !host.authenticated) {
        Com_Printf(CON_CHANNEL_DONT_FILTER, "Local server is not ready for administration.\n"); return true;
    }
    if (std::strlen(command) >= sizeof(Packet::command)) {
        Com_Printf(CON_CHANNEL_DONT_FILTER, "Local server command exceeds 1023 bytes.\n"); return true;
    }
    auto value = packet(HostMessage::Command);
    I_strncpyz(value.command, command, sizeof(value.command));
    if (!enqueue(value, false)) Com_Printf(CON_CHANNEL_DONT_FILTER, "Local server command queue is full.\n");
    host.nextPoll = 0;
    return true;
}
bool KHZ_IsOwnedHostConnection() {
#ifdef KISAK_DEDICATED
    return false;
#else
    if (!KHZ_HasOwnedHost() || CL_GetLocalClientConnectionState(host.localClientNum) < CA_CONNECTING) return false;
    const auto* connection = CL_GetLocalClientConnection(host.localClientNum);
    const auto& address = connection->serverAddress;
    return address.type == NA_IP && address.ip[0] == 127 && address.ip[1] == 0 && address.ip[2] == 0 && address.ip[3] == 1 &&
        ntohs(address.port) == host.configuration.port;
#endif
}
const char* KHZ_HostStatusJSON() {
    static char result[1536];
    char error[512]; jsonEscape(host.error, error, sizeof(error));
    std::snprintf(result, sizeof(result), "{\"phase\":\"%s\",\"managed_server\":%s,\"owned\":%s,\"authenticated\":%s,"
        "\"child_pid\":%lu,\"map\":\"%s\",\"gametype\":\"%s\",\"port\":%u,\"bot_count\":%u,\"bot_difficulty\":%u,"
        "\"transitions\":%u,\"failures\":%u,\"error\":\"%s\"}", phaseName(host.phase), host.managedServer ? "true" : "false",
        KHZ_HasOwnedHost() ? "true" : "false", host.authenticated ? "true" : "false", host.childPid, host.configuration.map,
        host.configuration.gametype, host.configuration.port, host.configuration.bots, host.configuration.difficulty,
        host.transitions, host.failures, error);
    return result;
}

#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
#include "khz/khz_archive_probe_host.inl"
#endif
