/* Actual direct control-redirect output and worker/tick journal producers. */
static volatile LONG print_fixture_worker;
static int print_fixture_completions, print_fixture_owner, print_fixture_tick;
static void print_fixture_work(void* unused) {
    Plugin_Printf("KHZ_PLUGIN_PRINT_FIXTURE worker=1 main=%d\n", Plugin_IsMainThread());
    Plugin_PrintWarning("KHZ_PLUGIN_PRINT_FIXTURE worker_warning=1\n");
    Plugin_PrintError("KHZ_PLUGIN_PRINT_FIXTURE worker_error=1\n");
    InterlockedExchange(&print_fixture_worker, 1);
}
static void print_fixture_completion(void* unused) {
    ++print_fixture_completions; print_fixture_owner = Plugin_IsMainThread() && !Plugin_IsAuthoritativeTick();
}
static void print_fixture_tick_probe(void) {
    print_fixture_tick = Plugin_IsAuthoritativeTick();
    Plugin_Printf("KHZ_PLUGIN_PRINT_FIXTURE tick=1 authoritative=%d\n", print_fixture_tick);
    Plugin_PrintWarning("KHZ_PLUGIN_PRINT_FIXTURE tick_warning=1\n");
    Plugin_PrintError("KHZ_PLUGIN_PRINT_FIXTURE tick_error=1\n");
}
static void print_fixture_start(void) {
    Plugin_Printf("KHZ_PLUGIN_PRINT_FIXTURE direct=1 main=%d tick=%d\n", Plugin_IsMainThread(), Plugin_IsAuthoritativeTick());
    Plugin_PrintWarning("KHZ_PLUGIN_PRINT_FIXTURE direct_warning=1\n");
    Plugin_PrintError("KHZ_PLUGIN_PRINT_FIXTURE direct_error=1\n");
    if (!Plugin_QueueJob(print_fixture_work, NULL, print_fixture_completion)) Plugin_PrintError("KHZ_PLUGIN_PRINT_FIXTURE job_failed=1\n");
}
static void print_fixture_report(void) {
    Plugin_Printf("KHZ_PLUGIN_PRINT_FIXTURE worker=%ld completions=%d owner=%d tick=%d\n",
        InterlockedCompareExchange(&print_fixture_worker, 0, 0), print_fixture_completions, print_fixture_owner, print_fixture_tick);
}
