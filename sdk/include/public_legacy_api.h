/* Included by pinc.h at the next SDK checkpoint. Active pinned exports below
 * include explicitly deprecated public APIs; those warnings/noops are not a
 * provider capability. Rebuilt source must use asynchronous jobs for sleeps. */
PAPI int __cdecl Plugin_ClientToSlot(client_t* client);
PAPI uint64_t __cdecl Plugin_Cmd_GetSteamID(void);
PAPI void __cdecl Plugin_SetPlayerGUID(unsigned int slot, const char* guid);
PAPI void __cdecl Plugin_SetPlayerUID(unsigned int slot, unsigned int uid);
PAPI unsigned int __cdecl Plugin_GetPlayerUID(unsigned int slot);
PAPI const char* __cdecl Plugin_GetPlayerGUID(unsigned int slot);
PAPI void __cdecl Plugin_SetPlayerNoPB(unsigned int slot);
PAPI int __cdecl Plugin_DoesServerUseUids(void);
PAPI void __cdecl Plugin_SetServerToUseUids(int enabled);
PAPI qboolean __cdecl Plugin_IsLoaded(char* name);
PAPI version_t* __cdecl Plugin_GetVersion(char* name);
PAPI unsigned int __cdecl Plugin_Scr_AllocArray(void);
PAPI threadid_t __cdecl Plugin_GetCurrentThreadId(void);
PAPI qboolean __cdecl Plugin_IsMainThread(void);
PAPI qboolean __cdecl Plugin_ThreadisSame(threadid_t thread);
PAPI void __cdecl Plugin_SleepMSec(int milliseconds);
PAPI void __cdecl Plugin_SleepSec(int seconds);
PAPI void __cdecl Plugin_EnableThreadDebug(void);
PAPI void __cdecl Plugin_DisableThreadDebug(void);
PAPI void __cdecl PHandler_Error(int plugin, int code, char* text);
