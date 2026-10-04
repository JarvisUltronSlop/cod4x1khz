// The x64 build binds to Valve's native redistributable at runtime. Keep the
// SDK's interface version checks and callbacks; a missing runtime is an init
// failure, using the same offline path as SteamAPI_Init failure on Win32.
#if defined(_WIN64) && defined(STEAM_API_NODLL)
#include <Windows.h>
#include <steamsdk/steam/steam_api.h>
#include <array>
#include <filesystem>
#include <mutex>
#include <cstdio>

namespace {
struct SteamImports {
    HMODULE module{};
    decltype(&SteamInternal_SteamAPI_Init) init{};
    decltype(&SteamAPI_Shutdown) shutdown{};
    decltype(&SteamAPI_RunCallbacks) callbacks{};
    decltype(&SteamAPI_GetHSteamUser) user{};
    decltype(&SteamInternal_ContextInit) context{};
    decltype(&SteamInternal_CreateInterface) create{};
    decltype(&SteamInternal_FindOrCreateUserInterface) findUser{};
    decltype(&SteamAPI_RegisterCallback) registerCallback{};
    decltype(&SteamAPI_UnregisterCallback) unregisterCallback{};
};
SteamImports imports;
std::mutex loadMutex;

template<typename T> bool bind(HMODULE module, T &target, const char *name) {
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    return target != nullptr;
}

bool load(SteamErrMsg *error) {
    std::lock_guard<std::mutex> lock(loadMutex);
    if (imports.module) return true;
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return false;
    const auto path = std::filesystem::path(executable.data()).parent_path() / L"steam_api64.dll";
    SteamImports candidate{};
    candidate.module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!candidate.module) {
        if (error) std::snprintf(*error, sizeof(*error), "Could not load native Steam runtime beside the executable (Windows error %lu)", GetLastError());
        return false;
    }
    if (!bind(candidate.module, candidate.init, "SteamInternal_SteamAPI_Init") ||
        !bind(candidate.module, candidate.shutdown, "SteamAPI_Shutdown") ||
        !bind(candidate.module, candidate.callbacks, "SteamAPI_RunCallbacks") ||
        !bind(candidate.module, candidate.user, "SteamAPI_GetHSteamUser") ||
        !bind(candidate.module, candidate.context, "SteamInternal_ContextInit") ||
        !bind(candidate.module, candidate.create, "SteamInternal_CreateInterface") ||
        !bind(candidate.module, candidate.findUser, "SteamInternal_FindOrCreateUserInterface") ||
        !bind(candidate.module, candidate.registerCallback, "SteamAPI_RegisterCallback") ||
        !bind(candidate.module, candidate.unregisterCallback, "SteamAPI_UnregisterCallback")) {
        if (error) std::snprintf(*error, sizeof(*error), "Native Steam runtime does not provide the functions required by this SDK");
        FreeLibrary(candidate.module);
        return false;
    }
    // Like a regular import, retain the module for the process lifetime. SDK
    // inline accessors keep static context counters across shutdown/reinit.
    imports = candidate;
    return true;
}
}

ESteamAPIInitResult S_CALLTYPE SteamInternal_SteamAPI_Init(const char *versions, SteamErrMsg *error) {
    if (!load(error)) return k_ESteamAPIInitResult_FailedGeneric;
    return imports.init(versions, error);
}
void S_CALLTYPE SteamAPI_Shutdown() { if (imports.shutdown) imports.shutdown(); }
void S_CALLTYPE SteamAPI_RunCallbacks() { if (imports.callbacks) imports.callbacks(); }
HSteamUser S_CALLTYPE SteamAPI_GetHSteamUser() { return imports.user ? imports.user() : 0; }
void *S_CALLTYPE SteamInternal_ContextInit(void *data) {
    if (imports.context) return imports.context(data);
    // The accessor dereferences the returned address, whose third native slot
    // is the null interface pointer until the real runtime initializes it.
    return static_cast<void **>(data) + 2;
}
void *S_CALLTYPE SteamInternal_CreateInterface(const char *version) {
    return imports.create ? imports.create(version) : nullptr;
}
void *S_CALLTYPE SteamInternal_FindOrCreateUserInterface(HSteamUser user, const char *version) {
    return imports.findUser ? imports.findUser(user, version) : nullptr;
}
void S_CALLTYPE SteamAPI_RegisterCallback(CCallbackBase *callback, int id) {
    if (imports.registerCallback) imports.registerCallback(callback, id);
}
void S_CALLTYPE SteamAPI_UnregisterCallback(CCallbackBase *callback) {
    if (imports.unregisterCallback) imports.unregisterCallback(callback);
}
#endif
