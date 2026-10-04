#include <universal/q_shared.h>
#include "khz_plugins.h"
#if defined(COD4X1KHZ) && defined(KISAK_MP)
#include "khz_paths.h"
#include "khz_admin_server.h"
#include "khz_admin.h"
#include "khz_identity.h"
#include "khz_controls.h"
#include "khz_plugin_http.h"
#include "khz_plugin_fs.h"
#include "khz_plugin_tcp.h"
#include "khz_plugin_net_address.h"
#include "khz_plugin_dns.h"
#include "khz_runtime.h"
#include <plugin_registry.hpp>
#include <plugin_journal.hpp>
#include <stats_offsets.hpp>
#include <steam_identity_format.hpp>
#include <plugin_io_policy.hpp>
#include <plugin_packet_mailbox.hpp>
#include <server_mp/server_mp.h>
#include <server/sv_game.h>
#include <game_mp/g_public_mp.h>
#include <game_mp/g_main_mp.h>
#include <universal/com_files.h>
#include <game_mp/g_utils_mp.h>
#include <client_mp/client_mp.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/threads.h>
#include <qcommon/msg_mp.h>
#include <win32/win_net.h>
#include <script/scr_vm.h>
#include <script/scr_main.h>
#include <script/scr_stringlist.h>
#include <script/scr_debugger.h>
#include <Windows.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>
#include <mutex>
#include <intrin.h>
#define KHZ_PLUGIN_HOST
#define KHZ_PLUGIN_NATIVE_TYPES
#include "../../../sdk/include/pinc.h"
#define KHZ_API extern "C" __declspec(dllexport)

namespace {
namespace plug = khz::plugins;
plug::Registry registry;
plug::MaintenanceCadence cadence;
using Callback = void (__cdecl*)(std::uintptr_t, std::uintptr_t, std::uintptr_t,
    std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
struct Module {
    HMODULE handle = nullptr;
    std::uintptr_t imageSize = 0;
    pluginInfo_t information{};
    std::array<Callback, plug::PublicEventCount> callbacks{};
    void (__cdecl* unload)() = nullptr;
    void (__cdecl* tick)(std::uint64_t, double) = nullptr;
    std::array<void*, 50> allocations{};
    std::array<int, plug::MaxExports> importedProviders{};
    unsigned importCount = 0;
    bool unloadCalled = false;
    bool servicesCancelled = false;
};
std::array<Module, plug::MaxPlugins> modules{};
thread_local int activePlugin = -1;
thread_local bool authoritativeCallback = false;
thread_local bool networkCallback = false;
thread_local std::uint64_t activeGeneration = 0;
std::atomic<bool> pluginThreadDebug{false};
std::atomic<std::uint64_t> workerFailures{0};
void invokePluginWorker(void (*work)(void*), void* argument, int* exitCode);
void leaveWorkerCritical();
bool hasPluginThreads(int plugin);
void servicePluginThreads();
const char* pluginThreadStats();
void cancelPluginHttp(int plugin);
void pluginNetworkActivate(unsigned plugin, std::uint64_t generation);
void pluginNetworkCancel(unsigned plugin, std::uint64_t generation);
void pluginNetworkInvalidate();
void pluginNetworkService();
const char* pluginNetworkStats();
std::uint64_t authoritativeTick = 0;
std::uint64_t lastFrameTick = UINT64_MAX;
bool initialized = false;
std::atomic<bool> shuttingDown{false};
const dvar_t* enabled = nullptr;
LARGE_INTEGER frequency{};
std::uint64_t frameCalls = 0, tickCalls = 0, callbackOverruns = 0, jobsRejected = 0, logsDropped = 0;
std::atomic<std::uint64_t> apiRejected{0};
std::uint64_t maximumCallbackUsec = 0;
std::array<cmd_function_s, plug::MaxCommands> dynamicCommands{};
struct Text { char text[1024]{}; int channel = CON_CHANNEL_SERVER; };
std::array<Text, 128> logs{};
std::array<Text, 128> commandQueue{};
unsigned logHead = 0, logCount = 0, commandHead = 0, commandCount = 0;
plug::FileJournal<128, 1024> gameLogs{};
std::uint64_t gameLogsDropped = 0;
// Logging from asynchronous jobs has a separate lock; gameplay callbacks never
// acquire it. Its journal drains only in outer service, bounded per pass.
SRWLOCK workerLogLock = SRWLOCK_INIT;
std::array<Text, 128> workerLogs{};
unsigned workerLogHead = 0, workerLogCount = 0;
std::atomic<std::uint64_t> workerLogsDropped{0};
struct DvarMetadata { char name[128]{}, description[1024]{}; };
// Native dvars retain name/description pointers. Keep host-owned storage until
// process exit, including after their registering DLL is unloaded.
std::array<DvarMetadata, 4096> dvarMetadata{};
unsigned dvarMetadataCount = 0;
enum JobState { Free, Pending, Running, Complete };
struct Job {
    std::atomic<int> state{Free};
    int owner = -1;
    std::uint64_t generation = 0;
    void (*work)(void*) = nullptr;
    void (*completion)(void*) = nullptr;
    void* argument = nullptr;
    bool workExecuted = false;
};
std::array<Job, plug::MaxPlugins * 8> jobs{};
constexpr const char* callbackNames[] = {
    "OnInfoRequest", "OnPlayerDC", "OnPlayerConnect", "OnExitLevel", "OnMessageSent", "OnFrame",
    "OnOneSecond", "OnTenSeconds", "OnClientAuthorized", "OnClientSpawn", "OnClientEnterWorld",
    "OnTcpServerPacket", "OnUdpNetEvent", "OnUdpNetSend", "OnSpawnServer", "OnPreFastRestart",
    "OnPostFastRestart", "OnPreGameRestart", "OnPostGameRestart", "OnClientUserinfoChanged",
    "OnClientCommand", "OnClientMoveCommand", "OnPlayerKilled", "OnPlayerWantReservedSlot",
    "OnFilesystemStarted", "OnPlayerGotAuthInfo", "OnPlayerAddBan", "OnPlayerGetBanStatus",
    "OnPlayerRemoveBan", "OnModuleLoaded", "OnScrUsercallFunction", "OnScrUsercallMethod",
    "OnScreenshotArrived", "OnDemoArrived", "OnTerminate"
};
static_assert(ARRAY_COUNT(callbackNames) == plug::PublicEventCount);
static_assert(sizeof(scr_entref_t) == 4);
static_assert(COD4X1KHZ_PLUGIN_SDK_VERSION == (sizeof(void*) == 8 ? 3 : 2));
void owner() { iassert(Sys_IsMainThread()); }
void callerContext(void* address) {
    if (!Sys_IsMainThread()) return; // job ownership is already pinned in TLS
    const auto caller = reinterpret_cast<std::uintptr_t>(address);
    for (unsigned i = 0; i != modules.size(); ++i) if (modules[i].handle) {
        const auto base = reinterpret_cast<std::uintptr_t>(modules[i].handle);
        if (caller >= base && caller - base < modules[i].imageSize) { activePlugin = int(i); return; }
    }
}
bool ownerApiCaller(void* caller, bool allowTick = true) {
    // Direct imported functions retain their C signature. Attribute an API call
    // to its actual DLL return address, even when another plugin calls an export.
    if (Sys_IsMainThread()) callerContext(caller);
    if (!Sys_IsMainThread() || activePlugin < 0 || (!allowTick && (authoritativeCallback || networkCallback || com_inServerFrame))) { ++apiRejected; return false; }
    const auto* plugin = registry.Get(activePlugin);
    return plugin && plugin->state != plug::State::Empty;
}
bool tickAllowed(bool value = true) { return value; }
#define ownerApi(...) ownerApiCaller(_ReturnAddress(), tickAllowed(__VA_ARGS__))
DvarMetadata* metadata(const char* name, const char* description) {
    if (!name || !*name || std::strlen(name) >= sizeof(DvarMetadata::name) ||
        (description && std::strlen(description) >= sizeof(DvarMetadata::description))) return nullptr;
    for (unsigned i = 0; i != dvarMetadataCount; ++i) if (plug::sameName(name, dvarMetadata[i].name)) return &dvarMetadata[i];
    if (dvarMetadataCount == dvarMetadata.size()) return nullptr;
    auto& value = dvarMetadata[dvarMetadataCount++]; I_strncpyz(value.name, name, sizeof(value.name));
    I_strncpyz(value.description, description ? description : "", sizeof(value.description)); return &value;
}
struct Scope {
    int previous, plugin;
    bool entered;
    LARGE_INTEGER begin{};
    Scope(int id, bool metadataOnly = false, bool unloadCallback = false) : previous(activePlugin), plugin(id),
        entered(unloadCallback ? registry.EnterUnload(id) : metadataOnly ? registry.EnterMetadata(id) : registry.Enter(id)) {
        if (entered) { activePlugin = id; QueryPerformanceCounter(&begin); }
    }
    ~Scope() {
        if (!entered) return;
        LARGE_INTEGER end{}; QueryPerformanceCounter(&end);
        const auto elapsed = frequency.QuadPart ? std::uint64_t(end.QuadPart - begin.QuadPart) * 1000000 / frequency.QuadPart : 0;
        maximumCallbackUsec = (std::max)(maximumCallbackUsec, elapsed);
        if ((authoritativeCallback || networkCallback || com_inServerFrame) && elapsed >= 1000) ++callbackOverruns;
        activePlugin = previous; registry.Leave(plugin);
    }
};
FARPROC symbol(HMODULE module, const char* name) {
    if (auto result = GetProcAddress(module, name)) return result;
    char decorated[96]; std::snprintf(decorated, sizeof(decorated), "_%s", name); return GetProcAddress(module, decorated);
}
void journal(int channel, const char* text) {
    if (Sys_IsMainThread()) {
        if (logCount == logs.size()) { ++logsDropped; return; }
        auto& entry = logs[(logHead + logCount++) % logs.size()]; entry.channel = channel; I_strncpyz(entry.text, text, sizeof(entry.text));
    } else {
        AcquireSRWLockExclusive(&workerLogLock);
        if (workerLogCount == workerLogs.size()) ++workerLogsDropped;
        else { auto& entry = workerLogs[(workerLogHead + workerLogCount++) % workerLogs.size()]; entry.channel = channel; I_strncpyz(entry.text, text, sizeof(entry.text)); }
        ReleaseSRWLockExclusive(&workerLogLock);
    }
}
void formatted(int channel, const char* format, va_list arguments, const char* prefix = "") {
    char text[1024]; const auto prefixLength = std::strlen(prefix);
    std::memcpy(text, prefix, prefixLength);
    std::vsnprintf(text + prefixLength, sizeof(text) - prefixLength, format ? format : "", arguments);
    text[sizeof(text) - 1] = 0;
    // Control commands must print while their native redirect is active.
    // Authoritative callbacks and worker calls retain bounded journals so
    // no steady gameplay tick waits for synchronous console/file output.
    if (Sys_IsMainThread() && !authoritativeCallback && !networkCallback && !com_inServerFrame) Com_Printf(channel, "%s", text);
    else journal(channel, text);
}
void flushGameLogs(unsigned maximum) {
    // Only service/lifecycle paths call this. Pop before native I/O so an
    // engine error cannot leave an entry poised for a reused file handle.
    plug::FileJournal<128, 1024>::Record entry;
    for (unsigned i = 0; i != maximum && gameLogs.Pop(entry); ++i) {
        if (!gameLogs.Active(entry)) { ++gameLogsDropped; continue; }
        FS_Write(entry.text, int(std::strlen(entry.text)), entry.handle);
    }
}
template<unsigned Slot> void scriptFunction() {
    owner(); const auto* binding = registry.Script(Slot);
    if (!binding || binding->kind != plug::Kind::Function) { Scr_Error("plugin function is unavailable after disable/unload"); return; }
    Scope scope(binding->owner); if (scope.entered) reinterpret_cast<void (*)()>(binding->callback)();
}
template<unsigned Slot> void scriptMethod(scr_entref_t ref) {
    owner(); const auto* binding = registry.Script(Slot);
    if (!binding || binding->kind != plug::Kind::Method) { Scr_Error("plugin method is unavailable after disable/unload"); return; }
    Scope scope(binding->owner); if (scope.entered) reinterpret_cast<void (*)(scr_entref_t)>(binding->callback)(ref);
}
template<std::size_t... Slots> constexpr auto functionThunks(std::index_sequence<Slots...>) { return std::array<KHZPluginFunction, sizeof...(Slots)>{&scriptFunction<Slots>...}; }
template<std::size_t... Slots> constexpr auto methodThunks(std::index_sequence<Slots...>) { return std::array<KHZPluginMethod, sizeof...(Slots)>{&scriptMethod<Slots>...}; }
constexpr auto pluginFunctionThunks = functionThunks(std::make_index_sequence<plug::MaxScriptBindings>{});
constexpr auto pluginMethodThunks = methodThunks(std::make_index_sequence<plug::MaxScriptBindings>{});
void dynamicCommand() {
    owner(); unsigned index;
    const auto* binding = registry.FindCommand(Cmd_Argv(0), &index);
    if (!binding || !KHZ_AdminCanInvokerUseCommand(binding->name)) { Com_Printf(CON_CHANNEL_SERVER, "Plugin command unavailable or insufficient power.\n"); return; }
    Scope scope(binding->owner); if (scope.entered) reinterpret_cast<xcommand_t>(binding->callback)();
}
std::array<int, plug::MaxCommands> commandOwners = [] { std::array<int, plug::MaxCommands> value{}; value.fill(-1); return value; }();
void removeOwnedCommands(int plugin) {
    for (unsigned i = 0; i != commandOwners.size(); ++i) if (commandOwners[i] == plugin) {
        if (dynamicCommands[i].name) Cmd_RemoveCommand(dynamicCommands[i].name);
        dynamicCommands[i] = {}; commandOwners[i] = -1;
    }
}
void tryUnload(int plugin) {
    const auto* entry = registry.Get(plugin);
    if (entry && entry->state == plug::State::UnloadPending) cancelPluginHttp(plugin);
    if (entry && entry->state == plug::State::UnloadPending && !entry->callbacks) {
        bool activeJob = hasPluginThreads(plugin);
        for (unsigned i = unsigned(plugin) * 8; i != unsigned(plugin) * 8 + 8; ++i)
            if (jobs[i].state.load(std::memory_order_acquire) != Free) activeJob = true;
        // Retire consumer imports only once its own callbacks and work are gone.
        // This also allows jointly requested unloads to break import cycles.
        if (!activeJob) {
            auto& module = modules[plugin];
            for (unsigned i = 0; i != module.importCount; ++i) registry.ReleaseImport(module.importedProviders[i]);
            module.importCount = 0;
        }
    }
    if (!registry.CanUnload(plugin)) return;
    auto& module = modules[plugin];
    if (!module.unloadCalled) {
        module.unloadCalled = true;
        // OnUnload itself is DLL code and must pin its own lifetime too. A
        // nested unload request cannot reclaim this callback's module/view.
        Scope scope(plugin, false, true); if (scope.entered && module.unload) module.unload();
    }
    if (!registry.CanUnload(plugin)) return;
    khz::plugin_http::Discard(plugin, entry->generation);
    khz::plugin_fs::Discard(plugin, entry->generation);
    khz::plugin_tcp::Discard(plugin, entry->generation);
    khz::plugin_dns::Discard(plugin, entry->generation);
    removeOwnedCommands(plugin);
    for (unsigned i = 0; i != module.importCount; ++i) registry.ReleaseImport(module.importedProviders[i]);
    for (auto allocation : module.allocations) if (allocation) HeapFree(GetProcessHeap(), 0, allocation);
    if (module.handle) FreeLibrary(module.handle);
    module = {}; registry.FinishUnload(plugin);
}
bool load(const char* requested) {
    owner(); if (!enabled || !enabled->current.enabled || shuttingDown || authoritativeCallback || com_inServerFrame || !requested) return false;
    std::string name(requested);
    if (name.size() > 4 && plug::sameName(std::string_view(name).substr(name.size() - 4), ".dll")) name.resize(name.size() - 4);
    const int plugin = registry.BeginLoad(name); if (plugin < 0) return false;
    pluginNetworkActivate(unsigned(plugin), registry.Get(plugin)->generation);
    const auto directory = std::filesystem::path(KHZ_RuntimeRoot()) / "plugins"
#ifdef _WIN64
        / "x64"
#endif
        ;
    std::error_code error; std::filesystem::create_directories(directory, error);
    const auto path = directory / (name + ".dll");
    auto& module = modules[plugin];
    module.handle = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module.handle) { registry.RequestUnload(plugin); tryUnload(plugin); return false; }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.handle);
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const byte*>(module.handle) + dos->e_lfanew);
    module.imageSize = pe->OptionalHeader.SizeOfImage;
    const auto* compatibility = reinterpret_cast<const std::uint32_t*>(symbol(module.handle, "KHZ_PluginCompatibility"));
    if (!compatibility || compatibility[0] != COD4X1KHZ_PLUGIN_PROTOCOL || compatibility[1] != COD4X1KHZ_PLUGIN_SDK_VERSION) {
        registry.RequestUnload(plugin); tryUnload(plugin); return false;
    }
    auto info = reinterpret_cast<void (__cdecl*)(pluginInfo_t*)>(symbol(module.handle, "OnInfoRequest"));
    auto init = reinterpret_cast<int (__cdecl*)()>(symbol(module.handle, "OnInit"));
    module.unload = reinterpret_cast<void (__cdecl*)()>(symbol(module.handle, "OnUnload"));
    module.tick = reinterpret_cast<void (__cdecl*)(std::uint64_t, double)>(symbol(module.handle, "OnTick"));
    for (unsigned i = 0; i != module.callbacks.size(); ++i) module.callbacks[i] = reinterpret_cast<Callback>(symbol(module.handle, callbackNames[i]));
    bool success = false;
    if (info && init) {
        Scope scope(plugin); info(&module.information);
        module.information.fullName[63] = module.information.shortDescription[127] = module.information.longDescription[1023] = 0;
        if (module.information.handlerVersion.major == PLUGIN_HANDLER_VERSION_MAJOR &&
            module.information.handlerVersion.minor <= PLUGIN_HANDLER_VERSION_MINOR) success = init() >= 0;
    }
    if (success && registry.Enable(plugin)) {
        Com_Printf(CON_CHANNEL_SERVER, "Loaded rebuilt plugin %s (%s).\n", name.c_str(), module.information.fullName); return true;
    }
    registry.RequestUnload(plugin); tryUnload(plugin); return false;
}
void loadCommand() { if (Cmd_Argc() != 2 || !load(Cmd_Argv(1))) Com_Printf(CON_CHANNEL_SERVER, "loadPlugin <name>: initialization failed; use a rebuilt project SDK DLL in %s.\n", sizeof(void*) == 8 ? "runtime/plugins/x64" : "runtime/plugins"); }
void unloadCommand() {
    const int plugin = Cmd_Argc() == 2 ? registry.FindPlugin(Cmd_Argv(1)) : -1;
    if (plugin < 0) { Com_Printf(CON_CHANNEL_SERVER, "unloadPlugin <name>: plugin not found.\n"); return; }
    registry.RequestUnload(plugin); tryUnload(plugin);
    if (registry.Get(plugin)->state != plug::State::Empty) Com_Printf(CON_CHANNEL_SERVER, "Unload deferred while callbacks, imports, or jobs still pin the module.\n");
}
void listCommand() {
    for (unsigned i = 0; i != modules.size(); ++i) if (registry.Get(i)->state != plug::State::Empty)
        Com_Printf(CON_CHANNEL_SERVER, "%u %s state=%u callbacks=%u pins=%u\n", i, registry.Get(i)->name,
            unsigned(registry.Get(i)->state), registry.Get(i)->callbacks, registry.Get(i)->imports);
}
void infoCommand() {
    const int id = Cmd_Argc() == 2 ? registry.FindPlugin(Cmd_Argv(1)) : -1; if (id < 0) return;
    const auto& info = modules[id].information;
    Com_Printf(CON_CHANNEL_SERVER, "%s %d.%d: %s\n%s\n", info.fullName, info.pluginVersion.major, info.pluginVersion.minor, info.shortDescription, info.longDescription);
}
cmd_function_s loadRegistration{}, unloadRegistration{}, listRegistration{}, infoRegistration{};
DWORD WINAPI runJob(void* pointer) {
    auto& job = *static_cast<Job*>(pointer); activePlugin = job.owner; activeGeneration = job.generation;
    int exitCode = 0;
    try { if (job.work) invokePluginWorker(job.work, job.argument, &exitCode); job.workExecuted = true; }
    catch (...) { ++workerFailures; journal(CON_CHANNEL_ERROR, "Plugin job threw an exception; completion suppressed.\n"); }
    leaveWorkerCritical(); activePlugin = -1; activeGeneration = 0;
    job.state.store(Complete, std::memory_order_release); return 0;
}
client_t* client(int index) {
    return svs.clients && sv_maxclients && index >= 0 && index < sv_maxclients->current.integer ? &svs.clients[index] : nullptr;
}
int clientIndex(const client_t* value) {
    if (!value || !svs.clients) return -1;
    const auto address = reinterpret_cast<std::uintptr_t>(value), base = reinterpret_cast<std::uintptr_t>(svs.clients);
    if (!sv_maxclients || address < base || (address - base) % sizeof(client_t)) return -1;
    const auto index = (address - base) / sizeof(client_t);
    return index < unsigned(sv_maxclients->current.integer) ? int(index) : -1;
}
void message(int slot, char opcode, const char* format, va_list arguments) {
    if (!ownerApi()) return; char text[900]; std::vsnprintf(text, sizeof(text), format ? format : "", arguments);
    text[sizeof(text) - 1] = 0;
    for (char* p = text; *p; ++p) if (*p == '"' || *p == '\\' || static_cast<unsigned char>(*p) < 32) *p = ' ';
    char command[1024]; std::snprintf(command, sizeof(command), "%c \"%s\"", opcode, text); SV_GameSendServerCommand(slot, SV_CMD_CAN_IGNORE, command);
}
bool publicPluginAddress(const netadr_t& native, khz::plugin_net::Address* output) {
    khz::plugin_net::LegacyIPv4 fields{}; fields.type = int(native.type); fields.port = native.port;
    std::memcpy(fields.ip, native.ip, sizeof(fields.ip));
    if (!khz::plugin_net::FromLegacyIPv4(fields, output)) return false;
    if (output->type == KHZ_NET_NA_IP || output->type == KHZ_NET_NA_BROADCAST) output->sock = KHZ_NativeUdpSocket();
    return true;
}
bool nativePluginAddress(const khz::plugin_net::Address* input, netadr_t* output, bool ipBan = false) {
    if (!input || !output || (ipBan && input->type != KHZ_NET_NA_IP && input->type != KHZ_NET_NA_LOOPBACK)) return false;
    khz::plugin_net::LegacyIPv4 fields{}; if (!khz::plugin_net::ToLegacyIPv4(*input, &fields)) return false;
    netadr_t native{}; native.type = netadrtype_t(fields.type); native.port = fields.port;
    std::memcpy(native.ip, fields.ip, sizeof(native.ip)); *output = native; return true;
}
}

void KHZ_PluginInit() {
    owner(); if (initialized) return; initialized = true; QueryPerformanceFrequency(&frequency);
    KHZ_PluginRefreshNetworkInterfaces();
    if (!khz::plugin_http::Init()) Com_Printf(CON_CHANNEL_SERVER, "Plugin HTTP worker unavailable.\n");
    if (!khz::plugin_fs::Init()) Com_Printf(CON_CHANNEL_SERVER, "Plugin filesystem worker unavailable.\n");
    else if (!KHZ_PluginFsCaptureCatalog()) Com_Printf(CON_CHANNEL_SERVER, "Plugin filesystem catalog unavailable.\n");
    if (!khz::plugin_tcp::Init()) Com_Printf(CON_CHANNEL_SERVER, "Plugin TCP worker unavailable.\n");
    if (!khz::plugin_dns::Init()) Com_Printf(CON_CHANNEL_SERVER, "Plugin DNS worker unavailable.\n");
    enabled = Dvar_RegisterBool("khz_plugins", true, DVAR_ARCHIVE, "Enable rebuilt public plugin APIs for the custom protocol");
    Cmd_AddCommandInternal("loadPlugin", loadCommand, &loadRegistration); Cmd_AddCommandInternal("unloadPlugin", unloadCommand, &unloadRegistration);
    Cmd_AddCommandInternal("plugins", listCommand, &listRegistration); Cmd_AddCommandInternal("pluginInfo", infoCommand, &infoRegistration);
    KHZ_AdminRegisterCommand("loadPlugin", 100); KHZ_AdminRegisterCommand("unloadPlugin", 100);
    KHZ_AdminRegisterCommand("plugins", 20); KHZ_AdminRegisterCommand("pluginInfo", 20);
}
void KHZ_PluginEvent(KHZPluginEvent event, std::uintptr_t a0, std::uintptr_t a1, std::uintptr_t a2,
    std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5, std::uintptr_t a6) {
    owner(); const unsigned index = unsigned(event); if (!initialized || index >= plug::PublicEventCount) return;
    if (event == KHZPluginEvent::SpawnServer) { authoritativeTick = 0; lastFrameTick = UINT64_MAX; }
    if (event == KHZPluginEvent::SpawnServer) pluginNetworkInvalidate();
    // Public SDK2 has a different address layout/enum. The native packet and
    // proof owners retain their native20 endpoint; only an explicit field copy
    // is exposed to callbacks. No DLL receives a native pointer as public32.
    khz::plugin_net::Address publicAddress{};
    const netadr_t* nativeAddress = event == KHZPluginEvent::PlayerConnect ? reinterpret_cast<const netadr_t*>(a1) :
        event == KHZPluginEvent::PlayerWantReservedSlot ? reinterpret_cast<const netadr_t*>(a0) : nullptr;
    if (nativeAddress) {
        if (!publicPluginAddress(*nativeAddress, &publicAddress)) { ++apiRejected; return; }
        if (event == KHZPluginEvent::PlayerConnect) a1 = reinterpret_cast<std::uintptr_t>(&publicAddress);
        else a0 = reinterpret_cast<std::uintptr_t>(&publicAddress);
    }
    for (unsigned plugin = 0; plugin != modules.size(); ++plugin) if (auto callback = modules[plugin].callbacks[index]) {
        Scope scope(plugin); if (scope.entered) callback(a0, a1, a2, a3, a4, a5, a6);
    }
}
void KHZ_PluginFrame(std::uint64_t tick) {
    owner(); if (!initialized || shuttingDown || tick == lastFrameTick) return; lastFrameTick = tick;
    authoritativeTick = tick; const bool previous = authoritativeCallback; authoritativeCallback = true;
    KHZ_PluginEvent(KHZPluginEvent::Frame); ++frameCalls;
    for (unsigned plugin = 0; plugin != modules.size(); ++plugin) if (modules[plugin].tick) {
        Scope scope(plugin); if (scope.entered) { modules[plugin].tick(tick, 1.0); ++tickCalls; }
    }
    authoritativeCallback = previous;
}
void KHZ_PluginBeginTick(std::uint64_t tick) { owner(); authoritativeTick = tick; }
void KHZ_PluginService() {
    owner(); if (com_inServerFrame || authoritativeCallback) return;
    flushGameLogs(16);
    if (!initialized) return;
    // Commit copied package/loose-read references before any completion can
    // perform a pure handshake or otherwise publish results to the engine.
    KHZ_PluginFsServiceReferences();
    for (unsigned plugin = 0; plugin != modules.size(); ++plugin) {
        const auto* entry = registry.Get(plugin);
        if (entry && (entry->state == plug::State::Disabled || entry->state == plug::State::UnloadPending)) cancelPluginHttp(plugin);
    }
    pluginNetworkService();
    servicePluginThreads();
    for (auto& job : jobs) {
        const int state = job.state.load(std::memory_order_acquire);
        if (state == Pending) {
            job.state.store(Running, std::memory_order_release);
            if (!QueueUserWorkItem(runJob, &job, WT_EXECUTEDEFAULT)) job.state.store(Complete, std::memory_order_release);
        } else if (state == Complete) {
            const auto* plugin = registry.Get(job.owner);
            if (plugin && plugin->generation == job.generation && job.workExecuted && job.completion) {
                const auto completion = job.completion; job.completion = nullptr;
                Scope scope(job.owner); if (scope.entered) completion(job.argument);
            }
            registry.ReleaseImport(job.owner); job.owner = -1; job.state.store(Free, std::memory_order_release);
        }
    }
    const auto due = cadence.Due(GetTickCount64());
    if (due & 1) KHZ_PluginEvent(KHZPluginEvent::OneSecond);
    if (due & 2) KHZ_PluginEvent(KHZPluginEvent::TenSeconds);
    for (unsigned i = 0; i != 16 && logCount; ++i) {
        auto& entry = logs[logHead]; Com_Printf(entry.channel, "%s", entry.text); logHead = (logHead + 1) % logs.size(); --logCount;
    }
    for (unsigned i = 0; i != 16; ++i) {
        Text entry; bool present = false; AcquireSRWLockExclusive(&workerLogLock);
        if (workerLogCount) { entry = workerLogs[workerLogHead]; workerLogHead = (workerLogHead + 1) % workerLogs.size(); --workerLogCount; present = true; }
        ReleaseSRWLockExclusive(&workerLogLock); if (!present) break; Com_Printf(entry.channel, "%s", entry.text);
    }
    for (unsigned i = 0; i != 4 && commandCount; ++i) {
        auto entry = commandQueue[commandHead]; commandHead = (commandHead + 1) % commandQueue.size(); --commandCount;
        Cbuf_AddText(0, entry.text);
    }
    for (unsigned plugin = 0; plugin != modules.size(); ++plugin) tryUnload(plugin);
}
void KHZ_PluginShutdown() {
    owner(); if (!initialized || shuttingDown) return; shuttingDown = true;
    KHZ_PluginEvent(KHZPluginEvent::Terminate);
    for (unsigned plugin = 0; plugin != modules.size(); ++plugin) if (registry.Get(plugin)->state != plug::State::Empty) registry.RequestUnload(plugin);
    KHZ_PluginService();
    // Jobs/threads may still be inside Make/Poll or reading a returned view.
    // Keep host storage alive with their DLL pins; process teardown reclaims
    // retained workers. Never delete the HTTP State beneath a live caller.
    if (!registry.Loaded()) {
        khz::plugin_http::Shutdown();
        khz::plugin_fs::Shutdown();
        khz::plugin_tcp::Shutdown();
        khz::plugin_dns::Shutdown();
    }
    if (registry.Loaded()) Com_Printf(CON_CHANNEL_SERVER, "Plugin shutdown retains %u pinned modules until asynchronous work exits.\n", registry.Loaded());
}
void KHZ_PluginErrorCleanup() {
    owner(); activePlugin = -1; authoritativeCallback = networkCallback = false; registry.ErrorCleanup();
    pluginNetworkInvalidate();
    khz::plugin_fs::Invalidate();
    // Filesystem error/map abort may have closed native handles behind us.
    gameLogsDropped += gameLogs.Invalidate();
    for (unsigned i = 0; i != modules.size(); ++i) if (registry.Get(i)->state == plug::State::Initializing) registry.RequestUnload(i);
}
void KHZ_PluginVmShutdownBarrier() { owner(); registry.VmShutdownBarrier(); }
void KHZ_PluginGameLogOpen(int handle) {
    owner(); flushGameLogs(128); gameLogsDropped += gameLogs.Bind(handle);
}
void KHZ_PluginGameLogClose(int handle) {
    owner(); if (handle != gameLogs.Handle()) return;
    flushGameLogs(128); gameLogsDropped += gameLogs.Invalidate();
}
void KHZ_PluginGameLogWrite(const char* text) {
    owner(); if (gameLogs.Handle() && text && !gameLogs.Push(text)) ++gameLogsDropped;
}
void KHZ_ScrPluginUsercall() {
    owner(); if (Scr_GetNumParam() < 1) { Scr_Error("Usage: usercall(<syscall_name>, ...)"); return; }
    KHZ_PluginEvent(KHZPluginEvent::ScrUsercallFunction, reinterpret_cast<std::uintptr_t>(Scr_GetString(0)));
}
void KHZ_ScrPluginUsercallMethod(scr_entref_t ref) {
    owner(); if (ref.classnum || ref.entnum >= MAX_GENTITIES || !g_entities[ref.entnum].client) { Scr_ObjectError("usercall receiver is not a player"); return; }
    if (Scr_GetNumParam() < 1) { Scr_Error("Usage: self usercall(<syscall_name>, ...)"); return; }
    KHZ_PluginEvent(KHZPluginEvent::ScrUsercallMethod, reinterpret_cast<std::uintptr_t>(Scr_GetString(0)), ref.entnum);
}
KHZPluginFunction KHZ_PluginFindFunction(const char** name, int* type, bool replacementsOnly) {
    owner(); unsigned slot; const auto* binding = registry.FindScript(*name, plug::Kind::Function, &slot);
    if (!binding || (replacementsOnly && !binding->replace)) return nullptr; *name = binding->name; *type = 0; return pluginFunctionThunks[slot];
}
KHZPluginMethod KHZ_PluginFindMethod(const char** name, int* type, bool replacementsOnly) {
    owner(); unsigned slot; const auto* binding = registry.FindScript(*name, plug::Kind::Method, &slot);
    if (!binding || (replacementsOnly && !binding->replace)) return nullptr; *name = binding->name; *type = 0; return pluginMethodThunks[slot];
}
const char* KHZ_PluginStatsJSON() {
    const auto files = khz::plugin_fs::Stats();
    const auto tcp = khz::plugin_tcp::Stats();
    static char text[3072]; std::snprintf(text, sizeof(text), "{\"loaded\":%u,\"retired_script_slots\":%u,\"frame_dispatches\":%llu,\"tick_callbacks\":%llu,\"callback_overruns\":%llu,\"maximum_callback_us\":%llu,\"api_rejected\":%llu,\"job_rejected\":%llu,\"logs_dropped\":%llu,\"game_log_queued\":%u,\"game_logs_dropped\":%llu,\"worker_failures\":%llu,\"threads\":%s,\"filesystem\":{\"epoch\":%llu,\"streams\":%u,\"buffers\":%u,\"buffer_bytes\":%zu,\"opened\":%llu,\"rejected\":%llu,\"read_bytes\":%llu,\"written_bytes\":%llu,\"reference_dropped\":%llu,\"cancelled\":%llu},\"tcp\":{\"connected\":%u,\"pending\":%u,\"retiring\":%u,\"submitted\":%llu,\"completed\":%llu,\"failed\":%llu,\"cancelled\":%llu,\"rejected\":%llu,\"sent_bytes\":%llu,\"received_bytes\":%llu},\"network\":%s}",
        registry.Loaded(), registry.RetiredScripts(), frameCalls, tickCalls, callbackOverruns, maximumCallbackUsec, apiRejected.load(), jobsRejected, logsDropped + workerLogsDropped.load(), gameLogs.Count(), gameLogsDropped, workerFailures.load(), pluginThreadStats(),
        files.epoch, files.streams, files.buffers, files.bufferBytes, files.opened, files.rejected, files.bytesRead, files.bytesWritten, files.referenceDropped, files.cancelled,
        tcp.connected, tcp.pending, tcp.retiring, tcp.submitted, tcp.completed, tcp.failed, tcp.cancelled, tcp.rejected, tcp.bytesSent, tcp.bytesReceived, pluginNetworkStats()); return text;
}

KHZ_API char* __cdecl Plugin_Cmd_Argv(int index) { return ownerApi() ? const_cast<char*>(Cmd_Argv(index)) : const_cast<char*>(""); }
KHZ_API int __cdecl Plugin_Cmd_Argc() { return ownerApi() ? Cmd_Argc() : 0; }
KHZ_API char* __cdecl Plugin_Cmd_Args(char* buffer, int length) { if (buffer && length > 0) { buffer[0] = 0; if (ownerApi()) Cmd_ArgsBuffer(1, buffer, length); } return buffer; }
#define KHZ_PRINT_API(Name, Channel, Prefix) KHZ_API void __cdecl Name(const char* format, ...) { va_list args; va_start(args, format); formatted(Channel, format, args, Prefix); va_end(args); }
KHZ_PRINT_API(Plugin_Printf, CON_CHANNEL_SERVER, "")
KHZ_PRINT_API(Plugin_PrintWarning, CON_CHANNEL_ERROR, "^3Warning: ")
KHZ_PRINT_API(Plugin_PrintError, CON_CHANNEL_ERROR, "^1Error: ")
#undef KHZ_PRINT_API
KHZ_API void __cdecl Plugin_G_LogPrintf(const char* format, ...) {
    if (!ownerApi()) return;
    char text[1024]; va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof(text), format ? format : "", args); va_end(args); text[sizeof(text) - 1] = 0;
    G_LogPrintf("%s", text);
}
KHZ_API void __cdecl Plugin_PrintAdministrativeLog(const char* format, ...) {
    if (!ownerApi()) return;
    char text[512]; va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof(text), format ? format : "", args); va_end(args); text[sizeof(text) - 1] = 0;
    KHZ_AdminPluginLog(text, registry.Get(activePlugin)->name);
}
KHZ_API void __cdecl Plugin_DPrintf(const char* format, ...) {
    if (!ownerApi() || !Dvar_GetBool("developer")) return;
    va_list args; va_start(args, format); formatted(CON_CHANNEL_SERVER, format, args); va_end(args);
}
KHZ_API void __cdecl Plugin_Cbuf_AddText(const char* text) {
    if (!ownerApi() || !text || std::strlen(text) >= sizeof(Text::text) || commandCount == commandQueue.size()) { ++apiRejected; return; }
    I_strncpyz(commandQueue[(commandHead + commandCount++) % commandQueue.size()].text, text, sizeof(Text::text));
}
KHZ_API void __cdecl Plugin_AddCommand(const char* name, xcommand_t callback, int power) {
    if (!ownerApi(false) || !name || Cmd_FindCommand(name)) { ++apiRejected; return; }
    unsigned index;
    if (registry.AddCommand(activePlugin, name, reinterpret_cast<std::uintptr_t>(callback), power, index) != plug::RegistrationResult::Ok ||
        !KHZ_AdminRegisterCommand(name, power)) { ++apiRejected; return; }
    const auto* binding = registry.Command(index); commandOwners[index] = activePlugin;
    Cmd_AddCommandInternal(binding->name, dynamicCommand, &dynamicCommands[index]);
}
KHZ_API void __cdecl Plugin_RemoveCommand(char* name) {
    if (!ownerApi(false) || !name) return; unsigned index; const auto* binding = registry.FindCommand(name, &index);
    // Pinned CoD4x permits a trusted loaded plugin to remove native commands
    // too. Retire another plugin's host binding as well, so later slot reuse
    // cannot preserve a stale command thunk.
    Cmd_RemoveCommand(name);
    if (binding) { const int registeredOwner = binding->owner; registry.RemoveCommand(registeredOwner, name); dynamicCommands[index] = {}; commandOwners[index] = -1; }
}
KHZ_API void* __cdecl Plugin_Malloc(std::size_t size) {
    if (!ownerApi(false) || !size || size > 16 * 1024 * 1024) return nullptr;
    for (auto& allocation : modules[activePlugin].allocations) if (!allocation) { allocation = HeapAlloc(GetProcessHeap(), 0, size); return allocation; }
    ++apiRejected; return nullptr;
}
KHZ_API void __cdecl Plugin_Free(void* pointer) {
    if (!ownerApi(false) || !pointer) return;
    for (auto& allocation : modules[activePlugin].allocations) if (allocation == pointer) { HeapFree(GetProcessHeap(), 0, allocation); allocation = nullptr; return; }
    ++apiRejected;
}
KHZ_API void __cdecl Plugin_Error(int code, const char* format, ...) {
    if (!ownerApi()) return;
    char text[1024]; va_list args; va_start(args, format); std::vsnprintf(text, sizeof(text), format ? format : "", args); va_end(args);
    text[sizeof(text) - 1] = 0; PHandler_Error(activePlugin, code, text);
}
KHZ_API int __cdecl Plugin_GetPluginID() { callerContext(_ReturnAddress()); return activePlugin; }
KHZ_API int __cdecl Plugin_Milliseconds() { return int(Sys_Milliseconds()); }
KHZ_API void __cdecl Plugin_RandomBytes(byte* buffer, int length) { if (buffer && length > 0) khz::identity::RandomBytes(buffer, std::size_t(length)); }
KHZ_API int __cdecl Plugin_GetLevelTime() { return ownerApi() ? level.time : 0; }
KHZ_API int __cdecl Plugin_GetServerTime() { return ownerApi() ? svs.time : 0; }
KHZ_API time_t __cdecl Plugin_GetRealtime() { return std::time(nullptr); }
KHZ_API int __cdecl Plugin_GetSlotCount() { return ownerApi() && sv_maxclients ? sv_maxclients->current.integer : 0; }
KHZ_API qboolean __cdecl Plugin_IsSvRunning() { return ownerApi() && com_sv_running && com_sv_running->current.enabled; }
KHZ_API client_t* __cdecl Plugin_GetClientForClientNum(int slot) { return ownerApi() ? client(slot) : nullptr; }
KHZ_API unsigned int __cdecl Plugin_GetClientNumForClient(client_t* value) { return ownerApi() ? unsigned(clientIndex(value)) : UINT32_MAX; }
KHZ_API char* __cdecl Plugin_GetPlayerName(int slot) { auto* value = ownerApi() ? client(slot) : nullptr; return value ? value->name : const_cast<char*>(""); }
KHZ_API int __cdecl Plugin_GetPlayerUid(int) {
    journal(CON_CHANNEL_ERROR, "Plugin_GetPlayerUid: deprecated in pinned CoD4x18; 0 returned.\n"); return 0;
}
KHZ_API uint64_t __cdecl Plugin_GetPlayerID(unsigned int slot) {
    const auto* identity = ownerApi() ? KHZ_AdminClientIdentity(client(int(slot))) : nullptr;
    return identity && !khz::identity::IsEmpty(*identity) ? khz::identity::LocalPlayerId(*identity) : 0;
}
KHZ_API uint64_t __cdecl Plugin_GetPlayerSteamID(unsigned int slot) { if (ownerApi()) client(int(slot)); return 0; } // provider explicitly unavailable
KHZ_API clientScoreboard_t __cdecl Plugin_GetClientScoreboard(int slot) {
    clientScoreboard_t score{}; if (ownerApi() && client(slot) && level.clients) {
        const auto& session = level.clients[slot].sess; score = {session.score, session.deaths, session.kills, session.assists};
    } return score;
}
KHZ_API gentity_t* __cdecl Plugin_GetGentityForEntityNum(int entity) { return ownerApi() && entity >= 0 && entity < MAX_GENTITIES ? &g_entities[entity] : nullptr; }
KHZ_API playerState_t* __cdecl Plugin_SV_GameClientNum(int slot) { return ownerApi() && client(slot) ? SV_GameClientNum(slot) : nullptr; }
KHZ_API level_locals_t* __cdecl Plugin_GetLevelBase() { return ownerApi() ? &level : nullptr; }
KHZ_API void __cdecl Plugin_ChatPrintf(int slot, const char* format, ...) { va_list args; va_start(args, format); message(slot, 'h', format, args); va_end(args); }
KHZ_API void __cdecl Plugin_BoldPrintf(int slot, const char* format, ...) { va_list args; va_start(args, format); message(slot, 'c', format, args); va_end(args); }
KHZ_API void __cdecl Plugin_DropClient(int slot, const char* reason) { if (ownerApi() && client(slot)) SV_DropClient(client(slot), reason ? reason : "Plugin disconnected client", true); }
KHZ_API void __cdecl Plugin_BanClient(unsigned int slot, int minutes, int invoker, char* reason) {
    if (!ownerApi()) return;
    if (!KHZ_AdminQueuePluginBan(int(slot), minutes, invoker, reason, registry.Get(activePlugin)->name)) ++apiRejected;
}
KHZ_API void __cdecl Plugin_UpdatePlayername(unsigned int slot, const char* name) {
    if (!ownerApi() || !client(int(slot)) || !name) return;
    Info_SetValueForKey(client(int(slot))->userinfo, "name", name); ClientUserinfoChanged(slot);
}
KHZ_API client_t* __cdecl Plugin_SV_Cmd_GetPlayerClByHandle(const char* handle) { return ownerApi() ? KHZ_AdminFindClient(handle) : nullptr; }
KHZ_API const char* __cdecl Plugin_SV_Cmd_GetPlayerNameByHandle(const char* handle) { auto* value = Plugin_SV_Cmd_GetPlayerClByHandle(handle); return value ? value->name : ""; }
KHZ_API uint64_t __cdecl Plugin_SV_Cmd_GetPlayerIDByHandle(const char* handle) { auto* value = Plugin_SV_Cmd_GetPlayerClByHandle(handle); return value ? Plugin_GetPlayerID(clientIndex(value)) : 0; }
KHZ_API uint64_t __cdecl Plugin_SV_Cmd_GetPlayerSteamIDByHandle(const char* handle) { auto* value = Plugin_SV_Cmd_GetPlayerClByHandle(handle); return value ? Plugin_GetPlayerSteamID(clientIndex(value)) : 0; }
KHZ_API int __cdecl Plugin_Cmd_GetInvokerSlot() { return ownerApi() ? KHZ_AdminInvokerSlot() : -1; }
KHZ_API int __cdecl Plugin_Cmd_GetInvokerClnum() { return Plugin_Cmd_GetInvokerSlot(); }
KHZ_API int __cdecl Plugin_Cmd_GetInvokerUID() { return Plugin_GetPlayerUid(Plugin_Cmd_GetInvokerSlot()); }
KHZ_API int __cdecl Plugin_Cmd_GetInvokerPower() { return ownerApi() ? KHZ_AdminInvokerPower() : 0; }
KHZ_API uint64_t __cdecl Plugin_Cmd_GetInvokerSteamID() { return Plugin_GetPlayerSteamID(Plugin_Cmd_GetInvokerSlot()); }
KHZ_API const char* __cdecl Plugin_Cmd_GetInvokerName(char* buffer, int length) {
    if (buffer && length > 0) { buffer[0] = 0; if (ownerApi()) KHZ_AdminInvokerName(buffer, length); } return buffer;
}
KHZ_API qboolean __cdecl Plugin_CanPlayerUseCommand(int slot, const char* name) { return ownerApi() && KHZ_AdminCanPlayerUseCommand(slot, name); }
KHZ_API void __cdecl Plugin_SetStat(int slot, int index, int value) {
    if (!ownerApi()) return;
    auto* target = client(slot); const auto field = khz::stats::Locate(index);
    if (!target || target->header.state < CS_CONNECTED || target->statPacketsReceived != 127 || !field.width ||
        (field.width == 1 && (value < 0 || value > 255))) { ++apiRejected; return; }
    SV_SetClientStat(slot, index, static_cast<std::uint32_t>(value));
}
KHZ_API int __cdecl Plugin_GetStat(int slot, int index) {
    if (!ownerApi()) return 0;
    auto* target = client(slot);
    if (!target || target->header.state < CS_CONNECTED || target->statPacketsReceived != 127 || !khz::stats::Locate(index).width) { ++apiRejected; return 0; }
    return SV_GetClientStat(slot, index);
}
KHZ_API void __cdecl Plugin_AddCommandForClientToWhitelist(int slot, const char* name) {
    if (ownerApi() && !KHZ_AdminWhitelistCommand(slot, name)) ++apiRejected;
}

KHZ_API CONVAR_T* __cdecl Plugin_Cvar_RegisterString(const char* name, const char* value, int flags, const char* description) { if (!ownerApi(false) || !value) return nullptr; auto* text = metadata(name, description); return text ? const_cast<dvar_t*>(Dvar_RegisterString(text->name, value, flags, text->description)) : nullptr; }
KHZ_API CONVAR_T* __cdecl Plugin_Cvar_RegisterBool(const char* name, qboolean value, int flags, const char* description) { if (!ownerApi(false)) return nullptr; auto* text = metadata(name, description); return text ? const_cast<dvar_t*>(Dvar_RegisterBool(text->name, value != 0, flags, text->description)) : nullptr; }
KHZ_API CONVAR_T* __cdecl Plugin_Cvar_RegisterInt(const char* name, int value, int minimum, int maximum, int flags, const char* description) { if (!ownerApi(false)) return nullptr; auto* text = metadata(name, description); return text ? const_cast<dvar_t*>(Dvar_RegisterInt(text->name, value, minimum, maximum, flags, text->description)) : nullptr; }
KHZ_API CONVAR_T* __cdecl Plugin_Cvar_RegisterFloat(const char* name, float value, float minimum, float maximum, int flags, const char* description) { if (!ownerApi(false)) return nullptr; auto* text = metadata(name, description); return text ? const_cast<dvar_t*>(Dvar_RegisterFloat(text->name, value, minimum, maximum, flags, text->description)) : nullptr; }
KHZ_API void __cdecl Plugin_Cvar_SetInt(const CONVAR_T* variable, int value) { if (ownerApi() && variable) Dvar_SetInt(variable, value); }
KHZ_API void __cdecl Plugin_Cvar_SetBool(const CONVAR_T* variable, qboolean value) { if (ownerApi() && variable) Dvar_SetBool(variable, value != 0); }
KHZ_API void __cdecl Plugin_Cvar_SetFloat(const CONVAR_T* variable, float value) { if (ownerApi() && variable) Dvar_SetFloat(variable, value); }
KHZ_API void __cdecl Plugin_Cvar_SetString(const CONVAR_T* variable, const char* value) { if (ownerApi(false) && variable && value) Dvar_SetString(variable, value); }
KHZ_API int __cdecl Plugin_Cvar_GetInteger(const CONVAR_T* variable) { KHZ_DvarScope synchronization; return variable ? variable->current.integer : 0; }
KHZ_API qboolean __cdecl Plugin_Cvar_GetBoolean(const CONVAR_T* variable) { KHZ_DvarScope synchronization; return variable && variable->current.enabled; }
KHZ_API float __cdecl Plugin_Cvar_GetValue(const CONVAR_T* variable) { KHZ_DvarScope synchronization; return variable ? variable->current.value : 0.0f; }
KHZ_API const char* __cdecl Plugin_Cvar_GetString(const CONVAR_T* variable, char* buffer, int length) {
    KHZ_DvarScope synchronization;
    if (buffer && length > 0) { buffer[0] = 0; if (variable) I_strncpyz(buffer, Dvar_DisplayableValue(variable), length); } return buffer;
}
KHZ_API void __cdecl Plugin_Cvar_VariableStringBuffer(const char* name, char* buffer, size_t length) { KHZ_DvarScope synchronization; if (buffer && length > 0 && length <= INT32_MAX) { buffer[0] = 0; if (name) I_strncpyz(buffer, Dvar_GetString(name), int(length)); } }
KHZ_API int __cdecl Plugin_Cvar_VariableIntegerValue(const char* name) { KHZ_DvarScope synchronization; return name ? Dvar_GetInt(name) : 0; }
KHZ_API int __cdecl Plugin_Cvar_VariableBooleanValue(const char* name) { KHZ_DvarScope synchronization; return name ? Dvar_GetBool(name) : 0; }
KHZ_API float __cdecl Plugin_Cvar_VariableValue(const char* name) { KHZ_DvarScope synchronization; return name ? float(Dvar_GetFloat(name)) : 0.0f; }
KHZ_API void __cdecl Plugin_Cvar_Set(const char* name, const char* value) { if (ownerApi(false) && name && value) Dvar_SetFromStringByName(name, value); }

void addScript(const char* name, std::uintptr_t callback, plug::Kind kind, bool replace) {
    if (!ownerApi(false) || !name) return; unsigned index;
    if (registry.AddScript(activePlugin, name, kind, callback, replace, index) != plug::RegistrationResult::Ok) ++apiRejected;
}
KHZ_API void __cdecl Plugin_ScrAddFunction(char* name, void (*function)()) { if (ownerApi(false)) addScript(name, reinterpret_cast<std::uintptr_t>(function), plug::Kind::Function, false); }
KHZ_API void __cdecl Plugin_ScrAddMethod(char* name, void (*method)(scr_entref_t)) { if (ownerApi(false)) addScript(name, reinterpret_cast<std::uintptr_t>(method), plug::Kind::Method, false); }
KHZ_API void __cdecl Plugin_ScrReplaceFunction(char* name, xfunction_t function) { if (ownerApi(false)) addScript(name, reinterpret_cast<std::uintptr_t>(function), plug::Kind::Function, true); }
KHZ_API void __cdecl Plugin_ScrReplaceMethod(char* name, xfunction_t function) { if (ownerApi(false)) addScript(name, reinterpret_cast<std::uintptr_t>(function), plug::Kind::Method, true); }
#define KHZ_SCRIPT_RETURN(Name, Type, Args, Call, Failure) KHZ_API Type __cdecl Name Args { return ownerApi() ? (Call) : (Failure); }
#define KHZ_SCRIPT_VOID(Name, Args, Call) KHZ_API void __cdecl Name Args { if (ownerApi()) { Call; } }
KHZ_SCRIPT_RETURN(Plugin_Scr_GetNumParam, int, (), Scr_GetNumParam(), 0)
KHZ_SCRIPT_RETURN(Plugin_Scr_GetInt, int, (unsigned int parameter), Scr_GetInt(parameter), 0)
KHZ_SCRIPT_RETURN(Plugin_Scr_GetFloat, float, (unsigned int parameter), Scr_GetFloat(parameter), 0.0f)
KHZ_SCRIPT_RETURN(Plugin_Scr_GetString, char*, (unsigned int parameter), const_cast<char*>(Scr_GetString(parameter)), nullptr)
KHZ_SCRIPT_RETURN(Plugin_Scr_GetEntity, gentity_t*, (unsigned int parameter), Scr_GetEntity(parameter), nullptr)
KHZ_SCRIPT_RETURN(Plugin_Scr_GetConstString, short, (unsigned int parameter), short(Scr_GetConstString(parameter)), 0)
KHZ_SCRIPT_RETURN(Plugin_Scr_GetType, unsigned int, (unsigned int parameter), Scr_GetType(parameter), 0)
KHZ_SCRIPT_RETURN(Plugin_Scr_GetFunc, int, (unsigned int parameter), Scr_GetFunc(parameter), 0)
KHZ_SCRIPT_RETURN(Plugin_Scr_ExecEntThread, short, (gentity_t* entity, int function, unsigned int arguments), Scr_ExecEntThread(entity, function, arguments), 0)
KHZ_SCRIPT_RETURN(Plugin_Scr_ExecThread, short, (int function, unsigned int arguments), Scr_ExecThread(function, arguments), 0)
KHZ_SCRIPT_RETURN(Plugin_Scr_AllocString, int, (const char* value), SL_GetString(value, 0), 0)
KHZ_SCRIPT_RETURN(Plugin_SL_ConvertToString, const char*, (int string), SL_ConvertToString(string), "")
KHZ_SCRIPT_VOID(Plugin_Scr_GetVector, (unsigned int parameter, vec3_t* vector), Scr_GetVector(parameter, *vector))
KHZ_SCRIPT_VOID(Plugin_Scr_AddEntity, (gentity_t* entity), Scr_AddEntity(entity))
KHZ_SCRIPT_VOID(Plugin_Scr_Error, (const char* text), Scr_Error(text))
KHZ_SCRIPT_VOID(Plugin_Scr_ParamError, (int parameter, const char* text), Scr_ParamError(parameter, text))
KHZ_SCRIPT_VOID(Plugin_Scr_ObjectError, (const char* text), Scr_ObjectError(text))
KHZ_SCRIPT_VOID(Plugin_Scr_AddInt, (int value), Scr_AddInt(value))
KHZ_SCRIPT_VOID(Plugin_Scr_AddFloat, (float value), Scr_AddFloat(value))
KHZ_SCRIPT_VOID(Plugin_Scr_AddBool, (qboolean value), Scr_AddBool(value != 0))
KHZ_SCRIPT_VOID(Plugin_Scr_AddString, (const char* value), Scr_AddString(value))
KHZ_SCRIPT_VOID(Plugin_Scr_AddUndefined, (), Scr_AddUndefined())
KHZ_SCRIPT_VOID(Plugin_Scr_AddVector, (vec3_t vector), Scr_AddVector(vector))
KHZ_SCRIPT_VOID(Plugin_Scr_AddArray, (), Scr_AddArray())
KHZ_SCRIPT_VOID(Plugin_Scr_MakeArray, (), Scr_MakeArray())
KHZ_SCRIPT_VOID(Plugin_Scr_AddArrayKeys, (int object), Scr_AddArrayKeys(object))
KHZ_SCRIPT_VOID(Plugin_Scr_AddArrayStringIndexed, (int string), Scr_AddArrayStringIndexed(string))
KHZ_SCRIPT_VOID(Plugin_Scr_FreeThread, (short thread), Scr_FreeThread(uint16_t(thread)))
KHZ_SCRIPT_VOID(Plugin_Scr_NotifyNum, (int entity, unsigned int type, unsigned int string, unsigned int arguments), Scr_NotifyNum(entity, type, string, arguments))
KHZ_SCRIPT_VOID(Plugin_Scr_Notify, (gentity_t* entity, unsigned short string, unsigned int arguments), Scr_Notify(entity, string, arguments))
KHZ_SCRIPT_VOID(Plugin_SV_SetConfigstring, (int index, const char* value), SV_SetConfigstring(index, value))
KHZ_SCRIPT_VOID(Plugin_SV_GetConfigstring, (int index, char* buffer, int length), SV_GetConfigstring(index, buffer, length))
#undef KHZ_SCRIPT_RETURN
#undef KHZ_SCRIPT_VOID
KHZ_API void __cdecl Plugin_Scr_NotifyLevel(int string, unsigned int arguments) {
    if (!ownerApi()) return;
    if (arguments > scrVmPub.inparamcount) { Scr_Error("plugin level notify has too many arguments"); return; }
    SL_CheckExists(string);
    const char* usage = scrVarPub.varUsagePos;
    if (!usage) scrVarPub.varUsagePos = "<plugin level notify>";
    Scr_ClearOutParams();
    VariableValue* start = scrVmPub.top - arguments;
    const unsigned remaining = scrVmPub.inparamcount - arguments;
    if (scrVarPub.levelId) {
        if (scrVmDebugPub.checkBreakon) {
            auto* top = scrVmPub.top; scrVmPub.inparamcount = 0;
            Scr_CheckBreakonNotify(scrVarPub.levelId, string, top, 0, 0); scrVmPub.top = top;
        }
        const auto type = start->type; start->type = VAR_PRECODEPOS; scrVmPub.inparamcount = 0;
        VM_Notify(scrVarPub.levelId, string, scrVmPub.top); start->type = type;
    }
    while (scrVmPub.top != start) { RemoveRefToValue(scrVmPub.top->type, scrVmPub.top->u); --scrVmPub.top; }
    scrVmPub.inparamcount = remaining; iassert(!scrVmPub.outparamcount); scrVarPub.varUsagePos = usage;
}
KHZ_API void __cdecl Plugin_SteamIDToString(uint64_t id, char* buffer, int length) {
    if (!buffer || length <= 0) return;
    bool decimal;
    {
        KHZ_DvarScope synchronization;
        decimal = Dvar_GetBool("sv_usesteam64id");
    }
    khz::identity_format::Selected(id, decimal, buffer, length);
}
KHZ_API void __cdecl Plugin_SteamIDTo64String(uint64_t id, char* buffer, int length) {
    khz::identity_format::Steam64(id, buffer, length);
}
KHZ_API uint64_t __cdecl Plugin_StringToSteamID(const char* value) { return value ? khz::identity::StringToSteamId(value) : 0; }
KHZ_API qboolean __cdecl Plugin_SteamIDIsIndividual(uint64_t id) { return khz::identity::SteamIdIndividual(id); }
KHZ_API qboolean __cdecl Plugin_SteamIDIsIndividualAndSteamAccount(uint64_t id) { return khz::identity::SteamIdIndividual(id, true); }
KHZ_API uint64_t __cdecl Plugin_GUID2PlayerID(const char* value) { return value ? khz::identity::GuidToPlayerId(value) : 0; }
KHZ_API void __cdecl Plugin_RemoveBanByip(khz_public_netadr_t* address) {
    if (!ownerApi()) return; netadr_t native{};
    if (!nativePluginAddress(address, &native, true)) { ++apiRejected; return; }
    KHZ_AdminRemoveIpBan(native);
}
KHZ_API void __cdecl Plugin_PlayerAddBanByip(khz_public_netadr_t* address, char* message, int expires) {
    if (!ownerApi()) return; netadr_t native{};
    if (!nativePluginAddress(address, &native, true)) { ++apiRejected; return; }
    KHZ_AdminAddIpBan(native, message, expires < 0 ? 0 : expires);
}
KHZ_API char* __cdecl Plugin_PlayerBannedByip(khz_public_netadr_t* address, char* buffer, int length) {
    if (buffer && length > 0) buffer[0] = 0;
    if (!ownerApi() || !buffer || length <= 0) return nullptr; netadr_t native{};
    if (!nativePluginAddress(address, &native, true)) { ++apiRejected; return nullptr; }
    return KHZ_AdminFormatIpBanMessage(native, buffer, unsigned(length)) ? buffer : nullptr;
}
KHZ_API const char* __cdecl Plugin_WriteBanTimelimit(int seconds, char* buffer, int length) {
    if (!buffer || length <= 0) return buffer;
    khz::admin::WriteBanTimelimit(seconds, buffer, unsigned(length));
    return buffer;
}
KHZ_API const char* __cdecl Plugin_FormatBanMessage(int seconds, char* buffer, int length, const char* format, ...) {
    if (!buffer || length <= 0) return buffer; buffer[0] = 0;
    if (!ownerApi()) return buffer;
    char reason[512]; va_list args; va_start(args, format);
    std::vsnprintf(reason, sizeof(reason), format ? format : "", args); va_end(args); reason[sizeof(reason) - 1] = 0;
    KHZ_AdminFormatBanMessage(seconds, reason, buffer, unsigned(length)); return buffer;
}
KHZ_API const char* __cdecl Plugin_GetCommonVersionString() { return "cod4x1khz protocol10002 adapted-public-sdk2"; }
KHZ_API qboolean __cdecl Plugin_ExportFunction(const char* name, void* (*callback)()) {
    if (!ownerApi(false) || !name) return false; unsigned slot;
    if (registry.AddExport(activePlugin, name, reinterpret_cast<std::uintptr_t>(callback), slot) != plug::RegistrationResult::Ok) { ++apiRejected; return false; }
    return true;
}
KHZ_API void* __cdecl Plugin_ImportFunction(const char* name, const char* exported) {
    if (!ownerApi(false) || !name || !exported) return nullptr;
    const int provider = registry.FindPlugin(name); const auto* binding = registry.FindExport(provider, exported);
    auto& importer = modules[activePlugin];
    if (binding && provider == activePlugin) return reinterpret_cast<void*>(binding->callback);
    if (!binding || importer.importCount >= importer.importedProviders.size() || !registry.PinImport(provider)) return nullptr;
    importer.importedProviders[importer.importCount++] = provider; return reinterpret_cast<void*>(binding->callback);
}
KHZ_API qboolean __cdecl Plugin_QueueJob(void (*work)(void*), void* argument, void (*completion)(void*)) {
    if (!ownerApi() || !work || shuttingDown) return false;
    const unsigned begin = unsigned(activePlugin) * 8;
    for (unsigned i = begin; i != begin + 8; ++i) if (jobs[i].state.load(std::memory_order_acquire) == Free && registry.PinImport(activePlugin)) {
        auto& job = jobs[i]; job.owner = activePlugin; job.generation = registry.Get(activePlugin)->generation;
        job.work = work; job.argument = argument; job.completion = completion; job.workExecuted = false;
        job.state.store(Pending, std::memory_order_release); return true;
    }
    ++jobsRejected; return false;
}
KHZ_API uint64_t __cdecl Plugin_GetAuthoritativeTick() { return ownerApi() ? authoritativeTick : 0; }
KHZ_API qboolean __cdecl Plugin_IsAuthoritativeTick() { return Sys_IsMainThread() && (authoritativeCallback || com_inServerFrame); }
#include "khz_plugin_aliases.inl"
#include "khz_plugin_threads.inl"
#include "khz_plugin_http.inl"
#include "khz_plugin_files.inl"
#include "khz_plugin_tcp.inl"
#include "khz_plugin_net.inl"
#endif

#if defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)
#include "khz/khz_archive_probe_plugins.inl"
#endif
