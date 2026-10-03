#pragma once
#include "khz_identity.h"
#include <cstddef>
#include <cstdint>

struct netadr_t;
struct client_t;
struct scr_entref_t;

void KHZ_AdminInit();
void KHZ_AdminService();
void KHZ_AdminErrorCleanup();
void KHZ_AdminResetClient(client_t* client);
bool KHZ_AdminChallengeResponse(const netadr_t& from, int challenge, char* text, std::size_t capacity);
bool KHZ_AdminValidateConnect(const netadr_t& from, int challenge, int qport, const char* userinfo,
    khz::identity::Fingerprint& identity, char* denial, std::size_t capacity);
void KHZ_AdminAdmitClient(client_t* client, const khz::identity::Fingerprint& identity);
void KHZ_AdminSetConnectingIdentity(client_t* client, const khz::identity::Fingerprint& identity);
void KHZ_AdminRecordCommand(client_t* client, std::uint64_t commandTick);
int KHZ_AdminClientPower(const client_t* client);
bool KHZ_AdminClientUndercover(const client_t* client);
const khz::identity::Fingerprint* KHZ_AdminClientIdentity(const client_t* client);
// Monotonic admission/slot lifecycle serial, distinct from the map serverId.
std::uint64_t KHZ_AdminClientGeneration(const client_t* client);
bool KHZ_AdminReservedSlot(const khz::identity::Fingerprint& identity, const char* password);
bool KHZ_AdminAddIpBan(const netadr_t& from, const char* reason, std::int64_t expires);
void KHZ_AdminRemoveIpBan(const netadr_t& from);
const char* KHZ_AdminIpBanMessage(const netadr_t& from);
bool KHZ_AdminFormatIpBanMessage(const netadr_t& from, char* output, std::size_t capacity);
bool KHZ_AdminTakeNextRotation(char* map, std::size_t mapCapacity, char* gametype, std::size_t gametypeCapacity);
bool KHZ_AdminClientCommand(client_t* client, const char* text);
// 0=valid,1=bad password,2=disabled,3=limited. Caller redirects only1/2;
// limited packets have no response and never enter command execution.
int KHZ_AdminCheckRcon(const netadr_t& from, const char* password);
void KHZ_AdminAuditRcon(const netadr_t& from, const char* command);
void KHZ_AdminSetRemoteConsoleContext(bool active);
const char* KHZ_AdminStatsJSON();
bool KHZ_AdminRegisterCommand(const char* name, int defaultPower);
bool KHZ_AdminCanPlayerUseCommand(int slot, const char* name);
bool KHZ_AdminCanInvokerUseCommand(const char* name);
bool KHZ_AdminWhitelistCommand(int slot, const char* name);
// Trusted plugin privilege, copied subject/issuer, bounded service queue.
bool KHZ_AdminQueuePluginBan(int slot, int minutes, int invokerUid, const char* reason, const char* plugin);
void KHZ_AdminPluginLog(const char* text, const char* plugin);
void KHZ_AdminFormatBanMessage(int remainingSeconds, const char* reason, char* text, std::size_t capacity);
int KHZ_AdminInvokerSlot();
int KHZ_AdminInvokerPower();
const char* KHZ_AdminInvokerName(char* output, int capacity);
int KHZ_AdminClientUid(const client_t* client);
client_t* KHZ_AdminFindClient(const char* handle);
bool KHZ_AdminIsOwnedConsoleCommand(const char* name);

void KHZ_ScrGetPlayerId(scr_entref_t ref);
void KHZ_ScrGetPlayerId64(scr_entref_t ref);
void KHZ_ScrGetSteamId(scr_entref_t ref);
void KHZ_ScrGetSteamId64(scr_entref_t ref);
void KHZ_ScrGetUid(scr_entref_t ref);
void KHZ_ScrSetUid(scr_entref_t ref);
void KHZ_ScrGetPower(scr_entref_t ref);
void KHZ_ScrSetPower(scr_entref_t ref);
void KHZ_ScrGetUserinfo(scr_entref_t ref);
void KHZ_ScrGetPing(scr_entref_t ref);
void KHZ_ScrGetCountedFps(scr_entref_t ref);
void KHZ_ScrGetGeolocation(scr_entref_t ref);
void KHZ_ScrSteamGroupMembershipQuery(scr_entref_t ref);
