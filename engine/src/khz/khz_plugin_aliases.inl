// Include from khz_plugins.cpp after its existing exported functions. These
// definitions are deliberately outside the current Build54 checkpoint.
#include <script/scr_variable.h>
namespace {
void legacyNotice(const char* name) {
    char text[192]; std::snprintf(text, sizeof(text), "%s: deprecated in pinned CoD4x18; no identifier/provider change.\n", name);
    journal(CON_CHANNEL_ERROR, text);
}
}
KHZ_API int __cdecl Plugin_ClientToSlot(client_t* value) { return ownerApi() ? clientIndex(value) : -1; }
KHZ_API uint64_t __cdecl Plugin_Cmd_GetSteamID() { return Plugin_Cmd_GetInvokerSteamID(); }
KHZ_API void __cdecl Plugin_SetPlayerGUID(unsigned int, const char*) { legacyNotice("Plugin_SetPlayerGUID"); }
KHZ_API void __cdecl Plugin_SetPlayerUID(unsigned int, unsigned int) { legacyNotice("Plugin_SetPlayerUID"); }
KHZ_API unsigned int __cdecl Plugin_GetPlayerUID(unsigned int) { legacyNotice("Plugin_GetPlayerUID"); return 0; }
KHZ_API const char* __cdecl Plugin_GetPlayerGUID(unsigned int) { legacyNotice("Plugin_GetPlayerGUID"); return ""; }
KHZ_API void __cdecl Plugin_SetPlayerNoPB(unsigned int) { legacyNotice("Plugin_SetPlayerNoPB"); }
KHZ_API int __cdecl Plugin_DoesServerUseUids() { legacyNotice("Plugin_DoesServerUseUids"); return 0; }
KHZ_API void __cdecl Plugin_SetServerToUseUids(int) { legacyNotice("Plugin_SetServerToUseUids"); }
KHZ_API qboolean __cdecl Plugin_IsLoaded(char* name) {
    if (!ownerApi() || !name) return false; const int index = registry.FindPlugin(name); const auto* plugin = registry.Get(index);
    return plugin && (plugin->state == plug::State::Enabled || plugin->state == plug::State::Initializing);
}
KHZ_API version_t* __cdecl Plugin_GetVersion(char* name) {
    // OnInfoRequest is arbitrary DLL code. Query freshly as public CoD4x does,
    // but keep that metadata work outside the authoritative tick.
    if (!ownerApi(false) || !name) return nullptr; const int index = registry.FindPlugin(name); const auto* plugin = registry.Get(index);
    if (!plugin || plugin->state == plug::State::Empty || plugin->state == plug::State::UnloadPending) return nullptr;
    const auto information = reinterpret_cast<void (__cdecl*)(pluginInfo_t*)>(modules[index].callbacks[unsigned(KHZPluginEvent::InfoRequest)]);
    Scope scope(index, true); if (!scope.entered || !information) return nullptr;
    pluginInfo_t current{}; information(&current); modules[index].information.pluginVersion = current.pluginVersion;
    return &modules[index].information.pluginVersion;
}
KHZ_API unsigned int __cdecl Plugin_Scr_AllocArray() { return ownerApi() ? Scr_AllocArray() : 0; }
KHZ_API threadid_t __cdecl Plugin_GetCurrentThreadId() { return GetCurrentThreadId(); }
KHZ_API qboolean __cdecl Plugin_IsMainThread() { return Sys_IsMainThread(); }
KHZ_API qboolean __cdecl Plugin_ThreadisSame(threadid_t thread) { return GetCurrentThreadId() == thread; }
KHZ_API void __cdecl Plugin_SleepMSec(int milliseconds) {
    // No authoritative-owner sleep, including maintenance callbacks. Rebuild
    // callers to use Plugin_QueueJob or the adapted thread API.
    if (Sys_IsMainThread() || activePlugin < 0 || milliseconds < 0 || milliseconds > 60000) { ++apiRejected; return; }
    Sleep(static_cast<DWORD>(milliseconds));
}
KHZ_API void __cdecl Plugin_SleepSec(int seconds) {
    if (seconds < 0 || seconds > 60) { ++apiRejected; return; } Plugin_SleepMSec(seconds * 1000);
}
KHZ_API void __cdecl Plugin_EnableThreadDebug() { pluginThreadDebug.store(true); }
KHZ_API void __cdecl Plugin_DisableThreadDebug() { pluginThreadDebug.store(false); }
KHZ_API void __cdecl PHandler_Error(int target, int code, char* text) {
    if (!ownerApi() || target < 0 || target >= int(modules.size())) return;
    const auto* plugin = registry.Get(target); if (!plugin || plugin->state == plug::State::Empty) { ++apiRejected; return; }
    const char* message = text ? text : ""; journal(CON_CHANNEL_ERROR, message);
    if (plugin->state == plug::State::Disabled || plugin->state == plug::State::UnloadPending) return;
    if (code == P_ERROR_DISABLE) registry.Disable(target);
    else if (code == P_ERROR_TERMINATE) Com_Error(ERR_FATAL, "%s", message);
}
