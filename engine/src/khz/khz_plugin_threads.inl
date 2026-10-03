// Included by the native host. Descriptor lifetime and Registry mutations have
// one owner; workers publish completion with release/acquire synchronization.
namespace {
using ThreadCallback = void (__cdecl*)(void*, void*, void*, void*, void*, void*, void*, void*);
struct PluginThread {
    std::atomic<int> state{Free};
    int owner = -1;
    std::uint64_t generation = 0;
    HANDLE handle = nullptr;
    DWORD id = 0;
    void* (__cdecl* raw)(void*) = nullptr;
    ThreadCallback main = nullptr, completion = nullptr;
    void* argument = nullptr;
    std::array<void*, 8> arguments{}, callbackArguments{};
    bool success = false;
    int exitCode = 0;
};
std::array<PluginThread, plug::MaxPlugins * 8> pluginThreads{};
thread_local PluginThread* currentPluginThread = nullptr;
thread_local unsigned pluginCriticalDepth = 0;
std::recursive_mutex pluginCritical;
std::uint64_t threadRejected = 0, threadCompletions = 0;
constexpr DWORD PluginThreadExitException = 0xE04B485Au;
int pluginExitFilter(EXCEPTION_POINTERS* exception, int* exitCode) {
    if (!exception || !exception->ExceptionRecord || exception->ExceptionRecord->ExceptionCode != PluginThreadExitException ||
        exception->ExceptionRecord->NumberParameters != 1) return EXCEPTION_CONTINUE_SEARCH;
    *exitCode = int(exception->ExceptionRecord->ExceptionInformation[0]); return EXCEPTION_EXECUTE_HANDLER;
}
void invokePluginWorker(void (*work)(void*), void* argument, int* exitCode) {
    // No C++ objects or destructors in this SEH boundary. The public extern-C
    // ExitThread must not throw a C++ exception across MSVC /EHc assumptions.
    __try { work(argument); }
    __except (pluginExitFilter(GetExceptionInformation(), exitCode)) {}
}
void leaveWorkerCritical() {
    while (pluginCriticalDepth) { --pluginCriticalDepth; pluginCritical.unlock(); }
}
void threadDiagnostic(const char* operation) {
    if (!pluginThreadDebug.load(std::memory_order_relaxed)) return;
    char text[160]; std::snprintf(text, sizeof(text), "Plugin %d thread %lu %s depth=%u\n", activePlugin, GetCurrentThreadId(), operation, pluginCriticalDepth);
    journal(CON_CHANNEL_SERVER, text);
}
DWORD WINAPI runPluginThread(void* pointer) {
    auto& thread = *static_cast<PluginThread*>(pointer);
    activePlugin = thread.owner; activeGeneration = thread.generation; currentPluginThread = &thread;
    threadDiagnostic("started");
    try {
        invokePluginWorker([](void* opaque) {
            auto& value = *static_cast<PluginThread*>(opaque);
            if (value.raw) value.raw(value.argument);
            else if (value.main) value.main(value.arguments[0], value.arguments[1], value.arguments[2], value.arguments[3],
                value.arguments[4], value.arguments[5], value.arguments[6], value.arguments[7]);
        }, &thread, &thread.exitCode);
        thread.success = true;
    }
    catch (...) { ++workerFailures; journal(CON_CHANNEL_ERROR, "Plugin worker threw an exception; completion suppressed.\n"); }
    leaveWorkerCritical(); threadDiagnostic("finished");
    currentPluginThread = nullptr; activePlugin = -1; activeGeneration = 0;
    thread.state.store(Complete, std::memory_order_release); return DWORD(thread.exitCode);
}
bool hasPluginThreads(int plugin) {
    for (unsigned i = unsigned(plugin) * 8; i != unsigned(plugin) * 8 + 8; ++i)
        if (pluginThreads[i].state.load(std::memory_order_acquire) != Free) return true;
    return false;
}
PluginThread* reservePluginThread() {
    if (shuttingDown) return nullptr;
    const unsigned begin = unsigned(activePlugin) * 8;
    for (unsigned i = begin; i != begin + 8; ++i) if (pluginThreads[i].state.load(std::memory_order_acquire) == Free && registry.PinImport(activePlugin)) {
        auto& thread = pluginThreads[i]; thread.owner = activePlugin; thread.generation = registry.Get(activePlugin)->generation;
        thread.handle = nullptr; thread.id = 0; thread.raw = nullptr; thread.main = thread.completion = nullptr;
        thread.argument = nullptr; thread.arguments = {}; thread.callbackArguments = {}; thread.success = false; thread.exitCode = 0;
        thread.state.store(Pending, std::memory_order_release); return &thread;
    }
    ++threadRejected; return nullptr;
}
bool startPluginThread(PluginThread& thread) {
    thread.state.store(Running, std::memory_order_release);
    thread.handle = CreateThread(nullptr, 0, runPluginThread, &thread, 0, &thread.id);
    if (thread.handle) return true;
    registry.ReleaseImport(thread.owner); thread.owner = -1; thread.state.store(Free, std::memory_order_release); ++threadRejected; return false;
}
void servicePluginThreads() {
    for (auto& thread : pluginThreads) {
        if (thread.state.load(std::memory_order_acquire) != Complete) continue;
        // Complete is published before the wrapper returns. Do not reuse a
        // descriptor or unload its DLL while Windows still runs that wrapper.
        if (thread.handle && WaitForSingleObject(thread.handle, 0) != WAIT_OBJECT_0) continue;
        if (thread.handle) { CloseHandle(thread.handle); thread.handle = nullptr; }
        const auto* plugin = registry.Get(thread.owner);
        if (plugin && plugin->generation == thread.generation && thread.success && thread.completion) {
            const auto completion = thread.completion; thread.completion = nullptr;
            Scope scope(thread.owner);
            if (scope.entered) {
                ++threadCompletions;
                completion(thread.callbackArguments[0], thread.callbackArguments[1], thread.callbackArguments[2], thread.callbackArguments[3],
                    thread.callbackArguments[4], thread.callbackArguments[5], thread.callbackArguments[6], thread.callbackArguments[7]);
            }
        }
        registry.ReleaseImport(thread.owner); thread.owner = -1; thread.state.store(Free, std::memory_order_release);
    }
}
const char* pluginThreadStats() {
    unsigned active = 0; for (const auto& thread : pluginThreads) active += thread.state.load(std::memory_order_acquire) != Free;
    static char text[192]; std::snprintf(text, sizeof(text), "{\"active\":%u,\"rejected\":%llu,\"owner_completions\":%llu}", active, threadRejected, threadCompletions); return text;
}
}
KHZ_API qboolean __cdecl Plugin_CreateNewThread(void* (*main)(void*), threadid_t* id, void* argument) {
    if (id) *id = 0;
    if (!ownerApi(false) || !main) return false;
    auto* thread = reservePluginThread(); if (!thread) return false;
    thread->raw = main; thread->argument = argument;
    if (!startPluginThread(*thread)) return false; if (id) *id = thread->id; return true;
}
KHZ_API qboolean __cdecl Plugin_CreateCallbackThread(void* main, ...) {
    if (!ownerApi(false) || !main) return false;
    auto* thread = reservePluginThread(); if (!thread) return false;
    thread->main = reinterpret_cast<ThreadCallback>(main);
    va_list args; va_start(args, main); for (auto& argument : thread->arguments) argument = va_arg(args, void*); va_end(args);
    return startPluginThread(*thread);
}
KHZ_API qboolean __cdecl Plugin_SetupThreadCallback(void* callback, ...) {
    if (Sys_IsMainThread() || !currentPluginThread || !callback || currentPluginThread->completion) { ++apiRejected; return false; }
    va_list args; va_start(args, callback); for (auto& argument : currentPluginThread->callbackArguments) argument = va_arg(args, void*); va_end(args);
    currentPluginThread->completion = reinterpret_cast<ThreadCallback>(callback); return true;
}
KHZ_API void __cdecl Plugin_ExitThread(int code) {
    // Preserve the host descriptor, import pin and completion. Like the pinned
    // ExitThread API, this does not promise arbitrary plugin C++ destructors.
    // Return normally when plugin-local RAII must run.
    if (Sys_IsMainThread() || activePlugin < 0 || !activeGeneration) { ++apiRejected; return; }
    const ULONG_PTR parameter = ULONG_PTR(std::uint32_t(code));
    RaiseException(PluginThreadExitException, EXCEPTION_NONCONTINUABLE, 1, &parameter);
}
KHZ_API void __cdecl Plugin_EnterCriticalSection() {
    // Plugin synchronization cannot block the authoritative owner. Worker
    // locks protect plugin-private state only, never mutable engine pointers.
    if (Sys_IsMainThread() || activePlugin < 0 || !activeGeneration) { ++apiRejected; return; }
    pluginCritical.lock(); ++pluginCriticalDepth; threadDiagnostic("locked");
}
KHZ_API void __cdecl Plugin_LeaveCriticalSection() {
    if (Sys_IsMainThread() || activePlugin < 0 || !pluginCriticalDepth) { ++apiRejected; return; }
    --pluginCriticalDepth; pluginCritical.unlock(); threadDiagnostic("unlocked");
}
