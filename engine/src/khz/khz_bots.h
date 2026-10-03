#pragma once
#include <cstdint>

struct client_t;
struct usercmd_s;
struct gentity_s;
struct scr_entref_t;

// Native bots share the authoritative owner. They are world clients, never
// transport clients; the runtime supplies its monotonically increasing tick.
constexpr int KHZ_MaxMatchClients = 16;
void KHZ_BotInit();
void KHZ_BotTick(std::uint64_t tick);
void KHZ_BotPrepareMap();
void KHZ_BotResetAll();
void KHZ_BotResetWorkStats();
void KHZ_BotResetSlot(client_t* client);
void KHZ_BotShutdownFiles();
bool KHZ_BotsEnabled();
bool KHZ_IsNativeBot(const client_t* client);
// Current active bot admission only; no mutation or human-proof fallback.
std::uint64_t KHZ_BotClientGeneration(const client_t* client);
bool KHZ_BotBuildCommand(client_t* client, usercmd_s* command);
const char* KHZ_BotStatsJSON();
void KHZ_BotScriptStarted();
gentity_s* KHZ_AddBotClient(const char* name);
void KHZ_RemoveBotClients(int count);

void KHZ_ScrBotMoveTo(scr_entref_t ref);
void KHZ_ScrBotLookAt(scr_entref_t ref);
void KHZ_ScrBotStop(scr_entref_t ref);
void KHZ_ScrBotAction(scr_entref_t ref);
void KHZ_ScrBotLookAtPlayer(scr_entref_t ref);
void KHZ_ScrBotWeapon(scr_entref_t ref);
void KHZ_ScrBotAim(scr_entref_t ref);
void KHZ_ScrBotAimCancel(scr_entref_t ref);
void KHZ_ScrRemoveAllBots();
void KHZ_ScrRemoveBot();
void KHZ_ScrFileOpen();
void KHZ_ScrFileClose();
void KHZ_ScrFileCloseAll();
void KHZ_ScrFileReadLine();
void KHZ_ScrFileWriteLine();
void KHZ_ScrFileExists();
void KHZ_ScrFileRemove();
