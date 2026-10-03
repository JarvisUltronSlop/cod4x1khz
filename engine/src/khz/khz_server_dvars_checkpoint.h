#pragma once
#include <universal/q_shared.h>

// Source-owned provider, included at the end of universal/dvar.cpp. Replaying
// numeric values is supported; strings/registration need an allocator provider
// before their mutation can be allowed on an eligible replay branch.
void KHZ_ServerDvarsRegisterCheckpoint();
bool KHZ_ServerDvarsAllowValueMutation(const dvar_s*, DvarValue);
bool KHZ_ServerDvarsAllowTopologyMutation();
