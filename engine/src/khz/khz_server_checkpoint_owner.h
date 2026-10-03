#pragma once
#include "khz_server_checkpoint.h"

// Opt-in dedicated integration. These are source-owned implementations at the
// end of khz_host.cpp and com_memory.cpp, not substitutes for native hooks.
void KHZ_ServerCheckpointRegisterNativeOwner();
bool KHZ_ServerCheckpointBindNativeMap(char* reason, unsigned capacity);
bool KHZ_ServerNativeCheckpointCapture(std::uint64_t fixture);
bool KHZ_ServerNativeCheckpointValidateRestore();
bool KHZ_ServerNativeCheckpointRestore();
bool KHZ_ServerNativeCheckpointDiscard();
bool KHZ_ServerCheckpointObserveHunk(KHZ_ServerCheckpointFrame*, char*, unsigned);
bool KHZ_ServerCheckpointAllowHunkLifetimeChange(const char* operation);

// Must run while the authenticated host is still explicitly paused, after
// archive copy/restore and before admitting the next authoritative tick.
bool KHZ_ServerCheckpointPrepareNativeResume(char* reason, unsigned capacity);
