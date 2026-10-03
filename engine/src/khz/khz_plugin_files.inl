// Included only by khz_plugins.cpp after its host/TLS ownership helpers.
// Blocking disk/decoder work is explicit pinned-job/thread work. Main-owner
// callers fail before opening a file; the fixed gameplay tick never waits.
namespace {
bool pluginFileContext(void* caller, bool blocking, int* plugin, std::uint64_t* generation) {
    if (Sys_IsMainThread()) {
        if (blocking || !ownerApiCaller(caller, false)) { ++apiRejected; return false; }
        const auto* entry = registry.Get(activePlugin); if (!entry) return false;
        // FreeFile must remain available during OnUnload. Final Discard is a
        // separate host action after that callback and all worker pins finish.
        *plugin = activePlugin; *generation = entry->generation; return true;
    }
    return pluginHttpContext(caller, false, plugin, generation);
}
}
KHZ_API long __cdecl Plugin_FS_SV_FOpenFileRead(const char* path, fileHandle_t* handle) try {
    if (handle) *handle = 0; int plugin; std::uint64_t generation;
    if (!pluginFileContext(_ReturnAddress(), true, &plugin, &generation)) return -1;
    return khz::plugin_fs::OpenRead(plugin, generation, path, handle, khz::plugin_fs::ReadScope::Server);
} catch (...) { ++apiRejected; return -1; }
KHZ_API long __cdecl Plugin_FS_FOpenFileRead(const char* path, fileHandle_t* handle) try {
    if (handle) *handle = 0; int plugin; std::uint64_t generation;
    if (!pluginFileContext(_ReturnAddress(), true, &plugin, &generation)) return -1;
    return khz::plugin_fs::OpenRead(plugin, generation, path, handle);
} catch (...) { ++apiRejected; return -1; }
KHZ_API fileHandle_t __cdecl Plugin_FS_SV_FOpenFileWrite(const char* path) try {
    int plugin; std::uint64_t generation;
    return pluginFileContext(_ReturnAddress(), true, &plugin, &generation) ? khz::plugin_fs::OpenWrite(plugin, generation, path, khz::plugin_fs::WriteScope::Home) : 0;
} catch (...) { ++apiRejected; return 0; }
KHZ_API fileHandle_t __cdecl Plugin_FS_SV_FOpenFileAppend(const char* path) try {
    int plugin; std::uint64_t generation;
    return pluginFileContext(_ReturnAddress(), true, &plugin, &generation) ? khz::plugin_fs::OpenWrite(plugin, generation, path, khz::plugin_fs::WriteScope::Home, true) : 0;
} catch (...) { ++apiRejected; return 0; }
KHZ_API fileHandle_t __cdecl Plugin_FS_FOpenFileWrite(const char* path) try {
    int plugin; std::uint64_t generation;
    return pluginFileContext(_ReturnAddress(), true, &plugin, &generation) ? khz::plugin_fs::OpenWrite(plugin, generation, path) : 0;
} catch (...) { ++apiRejected; return 0; }
KHZ_API fileHandle_t __cdecl Plugin_FS_FOpenFileAppend(const char* path) try {
    int plugin; std::uint64_t generation;
    return pluginFileContext(_ReturnAddress(), true, &plugin, &generation) ? khz::plugin_fs::OpenWrite(plugin, generation, path, khz::plugin_fs::WriteScope::Game, true) : 0;
} catch (...) { ++apiRejected; return 0; }
KHZ_API int __cdecl Plugin_FS_Read(void* destination, int bytes, fileHandle_t handle) try {
    int plugin; std::uint64_t generation;
    return pluginFileContext(_ReturnAddress(), true, &plugin, &generation) ? khz::plugin_fs::Read(plugin, generation, destination, bytes, handle) : -1;
} catch (...) { ++apiRejected; return -1; }
KHZ_API int __cdecl Plugin_FS_Write(const void* source, int bytes, fileHandle_t handle) try {
    int plugin; std::uint64_t generation;
    return pluginFileContext(_ReturnAddress(), true, &plugin, &generation) ? khz::plugin_fs::Write(plugin, generation, source, bytes, handle) : -1;
} catch (...) { ++apiRejected; return -1; }
KHZ_API int __cdecl Plugin_FS_ReadLine(void* destination, int capacity, fileHandle_t handle) try {
    int plugin; std::uint64_t generation;
    return pluginFileContext(_ReturnAddress(), true, &plugin, &generation) ? khz::plugin_fs::ReadLine(plugin, generation, destination, capacity, handle) : -1;
} catch (...) { ++apiRejected; return -1; }
KHZ_API qboolean __cdecl Plugin_FS_FCloseFile(fileHandle_t handle) try {
    int plugin; std::uint64_t generation;
    return pluginFileContext(_ReturnAddress(), true, &plugin, &generation) && khz::plugin_fs::Close(plugin, generation, handle) ? qtrue : qfalse;
} catch (...) { ++apiRejected; return 0; }
KHZ_API int __cdecl Plugin_FS_ReadFile(const char* path, void** destination) try {
    if (destination) *destination = nullptr; int plugin; std::uint64_t generation;
    return pluginFileContext(_ReturnAddress(), true, &plugin, &generation) ? khz::plugin_fs::ReadWhole(plugin, generation, path, destination) : -1;
} catch (...) { ++apiRejected; return -1; }
KHZ_API void __cdecl Plugin_FS_FreeFile(void* buffer) try {
    int plugin; std::uint64_t generation;
    if (pluginFileContext(_ReturnAddress(), false, &plugin, &generation)) khz::plugin_fs::FreeBuffer(plugin, generation, buffer);
} catch (...) { ++apiRejected; return; }
KHZ_API int __cdecl Plugin_FS_WriteFile(const char* path, const void* source, int bytes) try {
    int plugin; std::uint64_t generation;
    return pluginFileContext(_ReturnAddress(), true, &plugin, &generation) ? khz::plugin_fs::WriteWhole(plugin, generation, path, source, bytes) : -1;
} catch (...) { ++apiRejected; return -1; }
KHZ_API int __cdecl Plugin_FS_SV_HomeWriteFile(const char* path, const void* source, int bytes) try {
    int plugin; std::uint64_t generation;
    return pluginFileContext(_ReturnAddress(), true, &plugin, &generation) ? khz::plugin_fs::WriteWhole(plugin, generation, path, source, bytes, khz::plugin_fs::WriteScope::Home) : -1;
} catch (...) { ++apiRejected; return -1; }
KHZ_API int __cdecl Plugin_FS_SV_BaseWriteFile(const char* path, const void* source, int bytes) try {
    int plugin; std::uint64_t generation;
    return pluginFileContext(_ReturnAddress(), true, &plugin, &generation) ? khz::plugin_fs::WriteWhole(plugin, generation, path, source, bytes, khz::plugin_fs::WriteScope::Base) : -1;
} catch (...) { ++apiRejected; return -1; }
KHZ_API int __cdecl Plugin_FS_SV_WriteFile(const char* path, const void* source, int bytes) try {
    // Same home destination as the public pinned alias, not fs_game.
    return Plugin_FS_SV_HomeWriteFile(path, source, bytes);
} catch (...) { ++apiRejected; return -1; }
KHZ_API void __cdecl Plugin_FS_SV_HomeCopyFile(char* source, char* destination) try {
    int plugin; std::uint64_t generation;
    if (pluginFileContext(_ReturnAddress(), true, &plugin, &generation)) khz::plugin_fs::CopyHome(plugin, generation, source, destination);
} catch (...) { ++apiRejected; return; }
KHZ_API void* __cdecl Plugin_FS_GetZipHandle(fileHandle_t handle) try {
    int plugin; std::uint64_t generation;
    return pluginFileContext(_ReturnAddress(), true, &plugin, &generation) ? khz::plugin_fs::ZipCookie(plugin, generation, handle) : nullptr;
} catch (...) { ++apiRejected; return nullptr; }
KHZ_API int __cdecl Plugin_unzSetPassword(void* zip, const char* password) try {
    int plugin; std::uint64_t generation;
    return pluginFileContext(_ReturnAddress(), true, &plugin, &generation) ? khz::plugin_fs::SetZipPassword(plugin, generation, zip, password) : -102;
} catch (...) { ++apiRejected; return -102; }
