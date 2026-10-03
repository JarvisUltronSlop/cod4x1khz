#pragma once
#include <cstdint>

// Startup and control work is performed outside the steady gameplay tick.
// These entry points are called only by their process's main/gameplay owner.
void KHZ_InitHost();
void KHZ_HostPoll();
void KHZ_HostServerTick();
bool KHZ_IsManagedHostProcess();
void KHZ_ShutdownHost();
bool KHZ_StartLocalHost(int localClientNum, const char* map, const char* gametype, int dedicatedMode);
void KHZ_StopOwnedHost(bool explicitLeave);
bool KHZ_HostMap(const char* map, const char* gametype = nullptr);
bool KHZ_HostRestart();
bool KHZ_HostConsoleCommand(const char* command);
bool KHZ_HasOwnedHost();
bool KHZ_IsOwnedHostConnection();
const char* KHZ_HostStatusJSON();
// Private benchmark control uses the authenticated owned-process channel.
// A positive fence is an actual server-owner acknowledgement, never a dvar.
bool KHZ_HostCheckpointPause(std::uint64_t fixture, std::uint64_t session, unsigned timeoutMilliseconds = 5000);
bool KHZ_HostCheckpointResume(std::uint64_t fixture, unsigned timeoutMilliseconds = 5000);
bool KHZ_HostCheckpointFenced(std::uint64_t fixture, std::uint64_t session);
bool KHZ_ServerCheckpointPaused();
// Main-owned bounded diagnostic copied from the authenticated fence reply.
// A different fixture never exposes a stale rejection reason.
const char* KHZ_HostCheckpointFailure(std::uint64_t fixture);
