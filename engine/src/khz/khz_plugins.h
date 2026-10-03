#pragma once
#include <cstdint>
struct scr_entref_t;
struct netadr_t;
enum class KHZPluginEvent : unsigned {
    InfoRequest, PlayerDC, PlayerConnect, ExitLevel, MessageSent, Frame,
    OneSecond, TenSeconds, ClientAuthorized, ClientSpawn, ClientEnterWorld,
    TcpServerPacket, UdpNetEvent, UdpNetSend, SpawnServer, PreFastRestart,
    PostFastRestart, PreGameRestart, PostGameRestart, ClientUserinfoChanged,
    ClientCommand, ClientMoveCommand, PlayerKilled, PlayerWantReservedSlot,
    FilesystemStarted, PlayerGotAuthInfo, PlayerAddBan, PlayerGetBanStatus,
    PlayerRemoveBan, ModuleLoaded, ScrUsercallFunction, ScrUsercallMethod,
    ScreenshotArrived, DemoArrived, Terminate
};
void KHZ_PluginInit();
// Main/network lifecycle only; copied immutable masks serve pure SDK readers.
void KHZ_PluginRefreshNetworkInterfaces();
// Native main/server packet boundaries only. false drops received traffic;
// send returns -1 for invalid/drop, 0 to send, 1 for a plugin veto (success).
// Address/payload edits are validated before native dispatch. Client owner
// traffic never enters DLL callbacks.
bool KHZ_PluginUdpReceive(netadr_t* source, unsigned char* data, int length);
int KHZ_PluginUdpSend(netadr_t* destination, unsigned char* data, int length);
void KHZ_PluginBeginTick(std::uint64_t tick);
void KHZ_PluginFrame(std::uint64_t tick);
void KHZ_PluginService();
void KHZ_PluginShutdown();
void KHZ_PluginErrorCleanup();
void KHZ_PluginVmShutdownBarrier();
// Map-owned game log: capture text/time on the owner and flush outside ticks.
// Open/close bind each native FS handle to a new generation, preventing reuse.
void KHZ_PluginGameLogOpen(int handle);
void KHZ_PluginGameLogClose(int handle);
void KHZ_PluginGameLogWrite(const char* text);
void KHZ_PluginEvent(KHZPluginEvent event, std::uintptr_t a0 = 0, std::uintptr_t a1 = 0,
    std::uintptr_t a2 = 0, std::uintptr_t a3 = 0, std::uintptr_t a4 = 0,
    std::uintptr_t a5 = 0, std::uintptr_t a6 = 0);
using KHZPluginFunction = void (*)();
using KHZPluginMethod = void (*)(scr_entref_t);
KHZPluginFunction KHZ_PluginFindFunction(const char** name, int* type, bool replacementsOnly);
KHZPluginMethod KHZ_PluginFindMethod(const char** name, int* type, bool replacementsOnly);
const char* KHZ_PluginStatsJSON();
void KHZ_ScrPluginUsercall();
void KHZ_ScrPluginUsercallMethod(scr_entref_t ref);
