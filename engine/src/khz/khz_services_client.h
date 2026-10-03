#pragma once
// Init/service/disconnect/shutdown run on the acknowledged main control owner.
// Service is never called from gameplay ticks. Browser queries and content I/O
// have private bounded workers; callbacks only publish copied metadata.
void KHZ_InitServicesClientFeatures();
void KHZ_ServicesClientService();
void KHZ_ServicesClientDisconnect();
void KHZ_ShutdownServicesClientFeatures();
bool KHZ_ServicesClientBeginDownload(const char* localName, const char* remoteName);
// Accepted native reliable-command dispatch calls this before legacy handlers.
// It enqueues bounded text only and is safe on the simulation packet owner.
bool KHZ_ServicesClientServerCommand(const char* command);
// Pure replay classifier; does not enqueue, change the reliable ring or read
// mutable service state. Bare native Cmd_Argv0 tokens are accepted here.
bool KHZ_ServicesClientIsServerCommand(const char* command);
void KHZ_ServicesClientLocalServers();
void KHZ_ServicesClientGlobalServers();
void KHZ_ServicesClientPing(const char* endpoint);
int KHZ_ServicesClientUpdateDirtyPings(int source);
const char* KHZ_ServicesClientStatusJSON();
