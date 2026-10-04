/* Adapted CoD4x public plugin API. Rebuild plugins for the host architecture.
 * Public names/signatures follow pinned CoD4x ddd65fc; public addresses use
 * its explicit32-byte layout, while native gameplay types/timing are adapted.
 * See sdk/README.md and upstream notices. */
#ifndef COD4X1KHZ_PINC_H
#define COD4X1KHZ_PINC_H
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#define PLUGIN_HANDLER_VERSION_MAJOR 4
#define PLUGIN_HANDLER_VERSION_MINOR 0
#if UINTPTR_MAX > UINT32_MAX
#define COD4X1KHZ_PLUGIN_SDK_VERSION 3
#else
#define COD4X1KHZ_PLUGIN_SDK_VERSION 2
#endif
#define COD4X1KHZ_PLUGIN_PROTOCOL 10002
#ifdef __cplusplus
#define KHZ_EXTERN extern "C"
#else
#define KHZ_EXTERN extern
#endif
#ifdef KHZ_PLUGIN_HOST
#define PAPI KHZ_EXTERN __declspec(dllexport)
#else
#define PAPI KHZ_EXTERN __declspec(dllimport)
#endif
#ifdef __cplusplus
#define PCL extern "C" __declspec(dllexport)
#else
#define PCL __declspec(dllexport)
#endif
#ifndef KHZ_PLUGIN_HOST
/* Header-emitted rebuilt-SDK marker. The host rejects legacy DLLs before
 * invoking their public lifecycle callbacks. selectany permits many units. */
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllexport) __declspec(selectany) uint32_t KHZ_PluginCompatibility[2] = {
    COD4X1KHZ_PLUGIN_PROTOCOL, COD4X1KHZ_PLUGIN_SDK_VERSION
};
#ifdef __cplusplus
}
#endif
#endif
#ifndef KHZ_PLUGIN_NATIVE_TYPES
typedef int qboolean;
enum { qfalse = 0, qtrue = 1 };
typedef unsigned char byte;
typedef int fileHandle_t;
typedef float vec3_t[3];
typedef struct { uint16_t entnum, classnum; } scr_entref_t;
typedef struct client_t client_t;
typedef struct gentity_s gentity_t;
typedef struct playerState_s playerState_t;
typedef struct level_locals_t level_locals_t;
typedef struct dvar_s CONVAR_T;
typedef struct usercmd_s usercmd_t;
typedef int hitLocation_t;
#include "public_net_address.h"
#else
typedef float vec3_t[3];
typedef gentity_s gentity_t;
typedef playerState_s playerState_t;
typedef dvar_s CONVAR_T;
typedef usercmd_s usercmd_t;
#define KHZ_PUBLIC_NET_NO_SHORT_NAMES
#include "public_net_address.h"
#undef KHZ_PUBLIC_NET_NO_SHORT_NAMES
#endif
typedef uint32_t threadid_t;
typedef void (*xcommand_t)(void);
typedef void (*xfunction_t)(void);
typedef struct { int major, minor; } version_t;
typedef struct { version_t handlerVersion, pluginVersion; char fullName[64], shortDescription[128], longDescription[1024]; } pluginInfo_t;
typedef struct { int score, deaths, kills, assists; } clientScoreboard_t;
enum Plugin_Err { P_ERROR_WARNING, P_ERROR_DISABLE, P_ERROR_TERMINATE };

PAPI char* __cdecl Plugin_Cmd_Argv(int arg);
PAPI int __cdecl Plugin_Cmd_Argc(void);
PAPI char* __cdecl Plugin_Cmd_Args(char* buffer, int length);
PAPI void __cdecl Plugin_Printf(const char* format, ...);
PAPI void __cdecl Plugin_PrintWarning(const char* format, ...);
PAPI void __cdecl Plugin_PrintError(const char* format, ...);
PAPI void __cdecl Plugin_DPrintf(const char* format, ...);
PAPI void __cdecl Plugin_G_LogPrintf(const char* format, ...);
PAPI void __cdecl Plugin_PrintAdministrativeLog(const char* format, ...);
PAPI void __cdecl Plugin_Cbuf_AddText(const char* text);
PAPI void __cdecl Plugin_AddCommand(const char* name, xcommand_t command, int defaultpower);
PAPI void __cdecl Plugin_RemoveCommand(char* name);
PAPI void* __cdecl Plugin_Malloc(size_t size);
PAPI void __cdecl Plugin_Free(void* pointer);
PAPI void __cdecl Plugin_Error(int code, const char* format, ...);
PAPI int __cdecl Plugin_GetPluginID(void);
PAPI int __cdecl Plugin_Milliseconds(void);
PAPI void __cdecl Plugin_RandomBytes(byte* buffer, int length);
PAPI int __cdecl Plugin_GetLevelTime(void);
PAPI int __cdecl Plugin_GetServerTime(void);
PAPI time_t __cdecl Plugin_GetRealtime(void);
PAPI int __cdecl Plugin_GetSlotCount(void);
PAPI qboolean __cdecl Plugin_IsSvRunning(void);
PAPI char* __cdecl Plugin_GetPlayerName(int slot);
PAPI int __cdecl Plugin_GetPlayerUid(int slot);
PAPI uint64_t __cdecl Plugin_GetPlayerID(unsigned int slot);
PAPI uint64_t __cdecl Plugin_GetPlayerSteamID(unsigned int slot);
PAPI clientScoreboard_t __cdecl Plugin_GetClientScoreboard(int slot);
PAPI client_t* __cdecl Plugin_GetClientForClientNum(int slot);
PAPI unsigned int __cdecl Plugin_GetClientNumForClient(client_t* client);
PAPI gentity_t* __cdecl Plugin_GetGentityForEntityNum(int entity);
PAPI playerState_t* __cdecl Plugin_SV_GameClientNum(int slot);
PAPI level_locals_t* __cdecl Plugin_GetLevelBase(void);
PAPI void __cdecl Plugin_ChatPrintf(int slot, const char* format, ...);
PAPI void __cdecl Plugin_BoldPrintf(int slot, const char* format, ...);
PAPI void __cdecl Plugin_DropClient(int slot, const char* reason);
PAPI void __cdecl Plugin_BanClient(unsigned int slot, int minutes, int invokerid, char* reason);
PAPI void __cdecl Plugin_UpdatePlayername(unsigned int slot, const char* name);
PAPI uint64_t __cdecl Plugin_SV_Cmd_GetPlayerSteamIDByHandle(const char* handle);
PAPI uint64_t __cdecl Plugin_SV_Cmd_GetPlayerIDByHandle(const char* handle);
PAPI const char* __cdecl Plugin_SV_Cmd_GetPlayerNameByHandle(const char* handle);
PAPI client_t* __cdecl Plugin_SV_Cmd_GetPlayerClByHandle(const char* handle);
PAPI int __cdecl Plugin_Cmd_GetInvokerSlot(void);
PAPI int __cdecl Plugin_Cmd_GetInvokerClnum(void);
PAPI int __cdecl Plugin_Cmd_GetInvokerUID(void);
PAPI int __cdecl Plugin_Cmd_GetInvokerPower(void);
PAPI uint64_t __cdecl Plugin_Cmd_GetInvokerSteamID(void);
PAPI const char* __cdecl Plugin_Cmd_GetInvokerName(char* buffer, int length);
PAPI qboolean __cdecl Plugin_CanPlayerUseCommand(int slot, const char* command);
PAPI void __cdecl Plugin_AddCommandForClientToWhitelist(int slot, const char* command);
PAPI void __cdecl Plugin_SetStat(int clientNum, int index, int value);
PAPI int __cdecl Plugin_GetStat(int clientNum, int index);

PAPI CONVAR_T* __cdecl Plugin_Cvar_RegisterString(const char* name, const char* value, int flags, const char* description);
PAPI CONVAR_T* __cdecl Plugin_Cvar_RegisterBool(const char* name, qboolean value, int flags, const char* description);
PAPI CONVAR_T* __cdecl Plugin_Cvar_RegisterInt(const char* name, int value, int minimum, int maximum, int flags, const char* description);
PAPI CONVAR_T* __cdecl Plugin_Cvar_RegisterFloat(const char* name, float value, float minimum, float maximum, int flags, const char* description);
PAPI void __cdecl Plugin_Cvar_SetInt(const CONVAR_T* variable, int value);
PAPI void __cdecl Plugin_Cvar_SetBool(const CONVAR_T* variable, qboolean value);
PAPI void __cdecl Plugin_Cvar_SetFloat(const CONVAR_T* variable, float value);
PAPI void __cdecl Plugin_Cvar_SetString(const CONVAR_T* variable, const char* value);
PAPI int __cdecl Plugin_Cvar_GetInteger(const CONVAR_T* variable);
PAPI qboolean __cdecl Plugin_Cvar_GetBoolean(const CONVAR_T* variable);
PAPI float __cdecl Plugin_Cvar_GetValue(const CONVAR_T* variable);
PAPI const char* __cdecl Plugin_Cvar_GetString(const CONVAR_T* variable, char* buffer, int length);
PAPI void __cdecl Plugin_Cvar_VariableStringBuffer(const char* name, char* buffer, size_t length);
PAPI int __cdecl Plugin_Cvar_VariableIntegerValue(const char* name);
PAPI int __cdecl Plugin_Cvar_VariableBooleanValue(const char* name);
PAPI float __cdecl Plugin_Cvar_VariableValue(const char* name);
PAPI void __cdecl Plugin_Cvar_Set(const char* name, const char* value);

PAPI void __cdecl Plugin_ScrAddFunction(char* name, void (*function)(void));
PAPI void __cdecl Plugin_ScrAddMethod(char* name, void (*method)(scr_entref_t));
PAPI void __cdecl Plugin_ScrReplaceFunction(char* name, xfunction_t function);
PAPI void __cdecl Plugin_ScrReplaceMethod(char* name, xfunction_t function);
PAPI void __cdecl Plugin_Scr_AddEntity(gentity_t* entity);
PAPI int __cdecl Plugin_Scr_GetNumParam(void);
PAPI int __cdecl Plugin_Scr_GetInt(unsigned int parameter);
PAPI float __cdecl Plugin_Scr_GetFloat(unsigned int parameter);
PAPI char* __cdecl Plugin_Scr_GetString(unsigned int parameter);
PAPI gentity_t* __cdecl Plugin_Scr_GetEntity(unsigned int parameter);
PAPI short __cdecl Plugin_Scr_GetConstString(unsigned int parameter);
PAPI unsigned int __cdecl Plugin_Scr_GetType(unsigned int parameter);
PAPI void __cdecl Plugin_Scr_GetVector(unsigned int parameter, vec3_t* vector);
PAPI int __cdecl Plugin_Scr_GetFunc(unsigned int parameter);
PAPI void __cdecl Plugin_Scr_Error(const char* message);
PAPI void __cdecl Plugin_Scr_ParamError(int parameter, const char* message);
PAPI void __cdecl Plugin_Scr_ObjectError(const char* message);
PAPI void __cdecl Plugin_Scr_AddInt(int value);
PAPI void __cdecl Plugin_Scr_AddFloat(float value);
PAPI void __cdecl Plugin_Scr_AddBool(qboolean value);
PAPI void __cdecl Plugin_Scr_AddString(const char* value);
PAPI void __cdecl Plugin_Scr_AddUndefined(void);
PAPI void __cdecl Plugin_Scr_AddVector(vec3_t vector);
PAPI void __cdecl Plugin_Scr_AddArray(void);
PAPI void __cdecl Plugin_Scr_MakeArray(void);
PAPI void __cdecl Plugin_Scr_AddArrayKeys(int object);
PAPI void __cdecl Plugin_Scr_AddArrayStringIndexed(int string);
PAPI short __cdecl Plugin_Scr_ExecEntThread(gentity_t* entity, int function, unsigned int arguments);
PAPI short __cdecl Plugin_Scr_ExecThread(int function, unsigned int arguments);
PAPI void __cdecl Plugin_Scr_FreeThread(short thread);
PAPI void __cdecl Plugin_Scr_NotifyLevel(int string, unsigned int arguments);
PAPI void __cdecl Plugin_Scr_NotifyNum(int entity, unsigned int type, unsigned int string, unsigned int arguments);
PAPI void __cdecl Plugin_Scr_Notify(gentity_t* entity, unsigned short string, unsigned int arguments);
PAPI int __cdecl Plugin_Scr_AllocString(const char* value);
PAPI const char* __cdecl Plugin_SL_ConvertToString(int string);
PAPI void __cdecl Plugin_SV_SetConfigstring(int index, const char* value);
PAPI void __cdecl Plugin_SV_GetConfigstring(int index, char* buffer, int length);
PAPI void __cdecl Plugin_SteamIDToString(uint64_t id, char* buffer, int length);
PAPI void __cdecl Plugin_SteamIDTo64String(uint64_t id, char* buffer, int length);
PAPI uint64_t __cdecl Plugin_StringToSteamID(const char* value);
PAPI qboolean __cdecl Plugin_SteamIDIsIndividual(uint64_t id);
PAPI qboolean __cdecl Plugin_SteamIDIsIndividualAndSteamAccount(uint64_t id);
PAPI uint64_t __cdecl Plugin_GUID2PlayerID(const char* guid);
PAPI void __cdecl Plugin_RemoveBanByip(khz_public_netadr_t* address);
PAPI void __cdecl Plugin_PlayerAddBanByip(khz_public_netadr_t* address, char* message, int expires);
PAPI char* __cdecl Plugin_PlayerBannedByip(khz_public_netadr_t* address, char* buffer, int length);
PAPI const char* __cdecl Plugin_FormatBanMessage(int remaining, char* buffer, int length, const char* format, ...);
PAPI const char* __cdecl Plugin_WriteBanTimelimit(int remaining, char* buffer, int length);
PAPI const char* __cdecl Plugin_GetCommonVersionString(void);
PAPI qboolean __cdecl Plugin_ExportFunction(const char* name, void* (*function)(void));
PAPI void* __cdecl Plugin_ImportFunction(const char* plugin, const char* name);

/* Adapted extensions. Work runs off the owner; completion runs in service.
 * SDK pointer APIs and scripts are forbidden in work. Jobs pin module lifetime. */
PAPI qboolean __cdecl Plugin_QueueJob(void (*work)(void*), void* argument, void (*completion)(void*));
PAPI uint64_t __cdecl Plugin_GetAuthoritativeTick(void);
#include "public_legacy_api.h"
#include "public_thread_api.h"
#include "public_http_api.h"
#include "public_file_api.h"
#include "public_tcp_api.h"
#include "public_net_api.h"
PAPI qboolean __cdecl Plugin_IsAuthoritativeTick(void);
#ifndef KHZ_PLUGIN_HOST
PCL void OnInfoRequest(pluginInfo_t* info);
PCL int OnInit(void);
PCL void OnUnload(void);
PCL void OnFrame(void);
PCL void OnTick(uint64_t tick, double milliseconds);
PCL void OnOneSecond(void);
PCL void OnTenSeconds(void);
PCL void OnPlayerConnect(int slot, khz_public_netadr_t* address, char* guid, char* userinfo, int authstatus, char* denied, int capacity);
PCL void OnPlayerDC(client_t* client, const char* reason);
PCL void OnClientSpawn(gentity_t* entity);
PCL void OnClientEnterWorld(client_t* client);
PCL void OnClientUserinfoChanged(client_t* client);
PCL void OnClientCommand(client_t* client, const char* command);
PCL void OnClientMoveCommand(client_t* client, usercmd_t* command);
PCL void OnPlayerKilled(gentity_t* entity, gentity_t* inflictor, gentity_t* attacker, int damage, int meansOfDeath, int weapon, hitLocation_t hitLocation);
PCL void OnPlayerWantReservedSlot(khz_public_netadr_t* address, char* guid, char* userinfo, int authstatus, qboolean* allowed);
PCL void OnSpawnServer(void);
PCL void OnPreFastRestart(void);
PCL void OnPostFastRestart(void);
PCL void OnPreGameRestart(int savepersist);
PCL void OnPostGameRestart(int savepersist);
PCL void OnExitLevel(void);
PCL void OnMessageSent(char* message, int slot, qboolean* show, int mode);
PCL void OnScrUsercallFunction(const char* name);
PCL void OnScrUsercallMethod(const char* name, int slot);
PCL void OnTerminate(void);
#endif
#endif
