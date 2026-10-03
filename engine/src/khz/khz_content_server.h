#pragma once
#include <cstdint>
struct client_t;
// Main server owner only; the worker receives copied paths/checksums and never
// reads native FS/searchpath, dvars, clients or script/gameplay state.
void KHZ_ContentServerInit();
void KHZ_ContentServerService();
void KHZ_ContentServerShutdown();
void KHZ_ContentServerErrorCleanup();
bool KHZ_ContentServerClientCommand(client_t* client, const char* text);
const char* KHZ_ContentServerStatsJSON();
