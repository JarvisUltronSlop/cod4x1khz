/* Included by the real C DLL fixture after pinc.h/Windows/string headers.
 * Requires a controlled home/main IWD containing khz_fs_fixture/{plain,
 * deflated,encrypted}.dat; commands create files only below a unique supplied
 * prefix. All file I/O runs in a pinned host job, never OnFrame.
 */
static volatile LONG file_fixture_active, file_fixture_finished;
static char file_fixture_prefix[96];
static unsigned file_fixture_cases, file_fixture_failures;
static void* file_fixture_retained;
static int file_fixture_owner, file_fixture_completion_count, file_fixture_freed;
static int file_fixture_main_rejected, file_fixture_tick_rejected;
static void file_check(int okay) { ++file_fixture_cases; if (!okay) ++file_fixture_failures; }
static void file_path(char* destination, const char* suffix) {
    snprintf(destination, 192, "%s/%s", file_fixture_prefix, suffix);
}
static int file_whole_matches(const char* path, const void* expected, int length) {
    void* buffer = NULL; int received = Plugin_FS_ReadFile(path, &buffer);
    int okay = received == length && buffer && !memcmp(buffer, expected, length) && !((char*)buffer)[length];
    if (buffer) Plugin_FS_FreeFile(buffer); return okay;
}
static void file_fixture_work(void* unused) {
    char path[192], text[128]; fileHandle_t handle = 0; void* zip;
    file_check(!Plugin_IsMainThread() && Plugin_GetPluginID() >= 0);
    file_check(Plugin_FS_FOpenFileRead("khz_fs_fixture/plain.dat", &handle) == 15 && handle > 0);
    file_check(handle && Plugin_FS_Read(text, sizeof(text), handle) == 15 && !memcmp(text, "native-zip-data", 15));
    file_check(handle && Plugin_FS_FCloseFile(handle));
    file_check(file_whole_matches("khz_fs_fixture/deflated.dat", "native-deflate-data", 19));
    file_check(Plugin_FS_FOpenFileRead("khz_fs_fixture/encrypted.dat", &handle) == 18 && handle > 0);
    zip = Plugin_FS_GetZipHandle(handle);
    file_check(zip && Plugin_unzSetPassword(zip, "native-secret") == 0);
    file_check(handle && Plugin_FS_Read(text, sizeof(text), handle) == 18 && !memcmp(text, "native-secret-data", 18));
    file_check(handle && Plugin_FS_FCloseFile(handle));
    file_path(path, "game.bin"); handle = Plugin_FS_FOpenFileWrite(path);
    file_check(handle && Plugin_FS_Write("A\0B", 3, handle) == 3 && Plugin_FS_FCloseFile(handle));
    handle = Plugin_FS_FOpenFileAppend(path);
    file_check(handle && Plugin_FS_Write("C", 1, handle) == 1 && Plugin_FS_FCloseFile(handle));
    file_check(file_whole_matches(path, "A\0BC", 4));
    file_path(path, "whole.dat");
    file_check(Plugin_FS_WriteFile(path, "whole", 5) == 5 && file_whole_matches(path, "whole", 5));
    file_path(path, "root.dat"); handle = Plugin_FS_SV_FOpenFileWrite(path);
    file_check(handle && Plugin_FS_Write("first\nlast", 10, handle) == 10 && Plugin_FS_FCloseFile(handle));
    handle = Plugin_FS_SV_FOpenFileAppend(path);
    file_check(handle && Plugin_FS_Write("+", 1, handle) == 1 && Plugin_FS_FCloseFile(handle));
    file_check(Plugin_FS_SV_FOpenFileRead(path, &handle) == 11 && handle > 0);
    file_check(handle && Plugin_FS_ReadLine(text, sizeof(text), handle) == 1 && !strcmp(text, "first\n"));
    file_check(handle && Plugin_FS_ReadLine(text, sizeof(text), handle) == 1 && !strcmp(text, "last+"));
    file_check(handle && Plugin_FS_ReadLine(text, sizeof(text), handle) == 0 && Plugin_FS_FCloseFile(handle));
    file_path(path, "home.dat");
    file_check(Plugin_FS_SV_HomeWriteFile(path, "home", 4) == 4);
    { char copied[192]; file_path(copied, "copied.dat"); Plugin_FS_SV_HomeCopyFile(path, copied);
      file_check(Plugin_FS_SV_FOpenFileRead(copied, &handle) == 4 && handle > 0);
      file_check(handle && Plugin_FS_Read(text, sizeof(text), handle) == 4 && !memcmp(text, "home", 4) && Plugin_FS_FCloseFile(handle)); }
    file_path(path, "base.dat");
    file_check(Plugin_FS_SV_BaseWriteFile(path, "base", 4) == 4 && Plugin_FS_SV_FOpenFileRead(path, &handle) == 4 && handle > 0);
    file_check(handle && Plugin_FS_Read(text, sizeof(text), handle) == 4 && !memcmp(text, "base", 4) && Plugin_FS_FCloseFile(handle));
    file_path(path, "alias.dat");
    file_check(Plugin_FS_SV_WriteFile(path, "alias", 5) == 5 && Plugin_FS_SV_FOpenFileRead(path, &handle) == 5 && handle > 0);
    file_check(handle && Plugin_FS_Read(text, sizeof(text), handle) == 5 && !memcmp(text, "alias", 5) && Plugin_FS_FCloseFile(handle));
    file_check(Plugin_FS_ReadFile("../escape", &file_fixture_retained) == -1 && !file_fixture_retained && !Plugin_FS_FOpenFileWrite("../escape"));
    file_check(Plugin_FS_ReadFile("khz_fs_fixture/plain.dat", &file_fixture_retained) == 15 && file_fixture_retained);
    InterlockedExchange(&file_fixture_finished, 1);
}
static void file_fixture_completion(void* unused) {
    file_fixture_owner = Plugin_IsMainThread() && !Plugin_IsAuthoritativeTick(); ++file_fixture_completion_count;
    file_fixture_freed = file_fixture_retained && !memcmp(file_fixture_retained, "native-zip-data", 15);
    if (file_fixture_retained) Plugin_FS_FreeFile(file_fixture_retained);
    file_fixture_retained = NULL; InterlockedExchange(&file_fixture_active, 0);
}
static void file_fixture_start(void) {
    const char* prefix = Plugin_Cmd_Argv(1); unsigned i;
    if (Plugin_Cmd_Argc() != 2 || strlen(prefix) < 16 || strlen(prefix) >= sizeof(file_fixture_prefix) || strncmp(prefix, "khz_sdk_fixture_", 16)) {
        Plugin_Printf("Usage: khz_fixture_files_start khz_sdk_fixture_<unique suffix>\n"); return;
    }
    for (i = 16; prefix[i]; ++i) if (!((prefix[i] >= 'a' && prefix[i] <= 'z') || (prefix[i] >= '0' && prefix[i] <= '9') || prefix[i] == '_')) return;
    if (InterlockedCompareExchange(&file_fixture_active, 1, 0)) return;
    strcpy(file_fixture_prefix, prefix); file_fixture_cases = file_fixture_failures = 0;
    file_fixture_freed = file_fixture_owner = 0; InterlockedExchange(&file_fixture_finished, 0);
    { void* buffer = (void*)1; fileHandle_t handle = 123;
      file_fixture_main_rejected = Plugin_FS_ReadFile("khz_fs_fixture/plain.dat", &buffer) == -1 && !buffer &&
          Plugin_FS_FOpenFileRead("khz_fs_fixture/plain.dat", &handle) == -1 && !handle; }
    if (!Plugin_QueueJob(file_fixture_work, NULL, file_fixture_completion)) InterlockedExchange(&file_fixture_active, 0);
}
static void file_fixture_tick_probe(void) {
    static int done = 0; fileHandle_t handle = 123;
    if (done) return; done = 1;
    file_fixture_tick_rejected = Plugin_FS_FOpenFileRead("khz_fs_fixture/plain.dat", &handle) == -1 && !handle && Plugin_IsAuthoritativeTick();
}
static void file_fixture_report(void) {
    LONG active = InterlockedCompareExchange(&file_fixture_active, 0, 0);
    LONG finished = InterlockedCompareExchange(&file_fixture_finished, 0, 0);
    if (active) { Plugin_Printf("KHZ_PLUGIN_FILE_FIXTURE active=1\n"); return; }
    Plugin_Printf("KHZ_PLUGIN_FILE_FIXTURE active=%ld finished=%ld cases=%u failures=%u completions=%d owner=%d retained_free=%d main_rejected=%d tick_rejected=%d\n",
        active, finished, file_fixture_cases, file_fixture_failures, file_fixture_completion_count, file_fixture_owner,
        file_fixture_freed, file_fixture_main_rejected, file_fixture_tick_rejected);
}
