#pragma once
#include "khz_server_checkpoint.h"

// Include khz_server_animation_checkpoint.inl at the END of xanim/xanim.cpp
// under COD4X1KHZ && KISAK_MP. Registration is separate from map binding:
// the 72 native player/corpse trees do not exist during initial KHZ_Init.
void KHZ_ServerAnimationRegisterCheckpoint();

// Dedicated/main owner, after G_LoadAnimTreeInstances and before capture.
// Advertises only the exact 72 real tree allocations this provider restores.
// Requires coordinator map-range lifetime handling before rebinding a new map.
bool KHZ_ServerAnimationBindMapTrees(char* reason, unsigned capacity);

// Call BEFORE unsupported definition/tree/asset lifetime side effects. Normal
// XAnimInfo allocation, release, state updates and XAnimClearTree are restored
// by this provider and do not use this guard. Refusal keeps the saved baseline
// available for restore/cleanup and latches branch ineligibility.
bool KHZ_ServerAnimationAllowAssetMutation(const char* operation);

struct XAnimTree_s;
// At XAnimInfo allocate/free/clone/clear entry, before changing its allocator or
// references. A pinned branch may mutate only one of the bound native trees.
bool KHZ_ServerAnimationAllowTreeMutation(const XAnimTree_s*, const char* operation);
