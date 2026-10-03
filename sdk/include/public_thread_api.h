/* Rebuilt public thread interfaces. Eight copied arguments match the pinned
 * callback contract. Mutable engine APIs remain authoritative-owner only. */
PAPI qboolean __cdecl Plugin_CreateNewThread(void* (*main)(void*), threadid_t* thread, void* argument);
PAPI qboolean __cdecl Plugin_CreateCallbackThread(void* main, ...);
PAPI qboolean __cdecl Plugin_SetupThreadCallback(void* callback, ...);
PAPI void __cdecl Plugin_ExitThread(int code);
PAPI void __cdecl Plugin_EnterCriticalSection(void);
PAPI void __cdecl Plugin_LeaveCriticalSection(void);
