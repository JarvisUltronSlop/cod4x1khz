/* Invoke the start command, let owner service run, then invoke report. These
 * checks use actual DLL worker callbacks; a report is not a native test run. */
static volatile LONG thread_raw_finished, thread_callback_finished, thread_callback_valid, thread_exit_reached;
static volatile LONG thread_worker_owner_valid, thread_setup_valid;
static int thread_owner_callbacks, thread_callback_owner_valid, thread_start_valid;
static int thread_tick_rejected;
static threadid_t thread_created_id;
static void* raw_thread(void* argument) {
    threadid_t id = Plugin_GetCurrentThreadId();
    Plugin_EnterCriticalSection();
    if (argument == (void*)(uintptr_t)123 && !Plugin_IsMainThread() && Plugin_ThreadisSame(id) && Plugin_GetPluginID() >= 0)
        InterlockedExchange(&thread_worker_owner_valid, 1);
    Plugin_SleepMSec(5); Plugin_LeaveCriticalSection();
    InterlockedExchange(&thread_raw_finished, 1); return NULL;
}
static void thread_callback(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    ++thread_owner_callbacks;
    thread_callback_owner_valid = Plugin_IsMainThread() && !Plugin_IsAuthoritativeTick() &&
        a == (void*)(uintptr_t)8 && b == (void*)(uintptr_t)7 && c == (void*)(uintptr_t)6 && d == (void*)(uintptr_t)5 &&
        e == (void*)(uintptr_t)4 && f == (void*)(uintptr_t)3 && g == (void*)(uintptr_t)2 && h == (void*)(uintptr_t)1;
}
static void callback_thread(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    int valid = a == (void*)(uintptr_t)1 && b == (void*)(uintptr_t)2 && c == (void*)(uintptr_t)3 && d == (void*)(uintptr_t)4 &&
        e == (void*)(uintptr_t)5 && f == (void*)(uintptr_t)6 && g == (void*)(uintptr_t)7 && h == (void*)(uintptr_t)8;
    InterlockedExchange(&thread_callback_valid, valid);
    valid = Plugin_SetupThreadCallback((void*)thread_callback, (void*)(uintptr_t)8, (void*)(uintptr_t)7,
        (void*)(uintptr_t)6, (void*)(uintptr_t)5, (void*)(uintptr_t)4, (void*)(uintptr_t)3, (void*)(uintptr_t)2, (void*)(uintptr_t)1);
    InterlockedExchange(&thread_setup_valid, valid);
    InterlockedExchange(&thread_callback_finished, 1);
    // Deliberately exit holding a private recursive lock. The adapted host
    // must release it and publish completion without killing a pool worker.
    Plugin_EnterCriticalSection(); Plugin_EnterCriticalSection();
    Plugin_ExitThread(7); InterlockedExchange(&thread_exit_reached, 1);
}
static void thread_start(void) {
    int raw, callback;
    if (thread_start_valid) { Plugin_Printf("KHZ_PLUGIN_THREAD_FIXTURE already started\n"); return; }
    Plugin_EnableThreadDebug();
    raw = Plugin_CreateNewThread(raw_thread, &thread_created_id, (void*)(uintptr_t)123);
    callback = Plugin_CreateCallbackThread((void*)callback_thread, (void*)(uintptr_t)1, (void*)(uintptr_t)2,
        (void*)(uintptr_t)3, (void*)(uintptr_t)4, (void*)(uintptr_t)5, (void*)(uintptr_t)6, (void*)(uintptr_t)7, (void*)(uintptr_t)8);
    thread_start_valid = raw && callback && thread_created_id != 0;
    Plugin_DisableThreadDebug();
    Plugin_Printf("KHZ_PLUGIN_THREAD_FIXTURE start=%d id=%u\n", thread_start_valid, thread_created_id);
}
static void thread_report(void) {
    Plugin_Printf("KHZ_PLUGIN_THREAD_FIXTURE start=%d raw=%ld worker_owner=%ld arguments=%ld setup=%ld finished=%ld callbacks=%d callback_owner=%d exit_unreachable=%d tick_rejected=%d\n",
        thread_start_valid, InterlockedCompareExchange(&thread_raw_finished,0,0), InterlockedCompareExchange(&thread_worker_owner_valid,0,0),
        InterlockedCompareExchange(&thread_callback_valid,0,0), InterlockedCompareExchange(&thread_setup_valid,0,0),
        InterlockedCompareExchange(&thread_callback_finished,0,0), thread_owner_callbacks, thread_callback_owner_valid,
        InterlockedCompareExchange(&thread_exit_reached,0,0) == 0, thread_tick_rejected);
}
static void alias_probe(void) {
    version_t* version;
    char* name = "khz_regression";
    int deprecated, loaded, conversion, missing, refreshed;
    char invoker[128];
    int before = fixture_information_queries;
    Plugin_SetPlayerGUID(0, "old"); Plugin_SetPlayerUID(0, 42); Plugin_SetPlayerNoPB(0); Plugin_SetServerToUseUids(1);
    deprecated = Plugin_GetPlayerUID(0) == 0 && Plugin_GetPlayerUid(0) == 0 && !Plugin_GetPlayerGUID(0)[0] && !Plugin_DoesServerUseUids();
    loaded = Plugin_IsLoaded(name); version = Plugin_GetVersion(name);
    refreshed = fixture_information_queries == before + 1 && version && version->major == 1;
    missing = !Plugin_IsLoaded("khz_fixture_absent") && !Plugin_GetVersion("khz_fixture_absent");
    conversion = Plugin_ClientToSlot(NULL) == -1;
    if (Plugin_GetSlotCount() > 0) conversion &= Plugin_ClientToSlot(Plugin_GetClientForClientNum(0)) == 0;
    Plugin_Error(P_ERROR_WARNING, "KHZ_PLUGIN_ALIAS_WARNING_FIXTURE");
    Plugin_Printf("KHZ_PLUGIN_ALIAS_FIXTURE deprecated=%d loaded=%d refreshed=%d missing=%d clienttoslot=%d invoker_name=%s steam_alias=%d main=%d\n",
        deprecated, loaded, refreshed, missing, conversion, Plugin_Cmd_GetInvokerName(invoker, sizeof(invoker)), Plugin_Cmd_GetSteamID() == Plugin_Cmd_GetInvokerSteamID(), Plugin_IsMainThread());
}
