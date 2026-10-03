#pragma once

#include <cstdint>

struct cg_s;
struct cgs_t;
struct centity_s;
struct snapshot_s;
struct DObj_s;
struct XAnimTree_s;
struct XAnimParts;
struct XModelNameMap;

// Prepare/release only on main while renderer jobs and the simulation owner are
// quiescent. Mutable object, tree, controller and skeleton storage is private;
// immutable parsed models/animation parts remain pinned by the map barrier.
bool KHZ_ClientPosePrepare(cg_s* game, cgs_t* gameStatic, centity_s* entities, const snapshot_s* accepted);
// Extends composition/model maps under an acknowledged main barrier; existing
// mutable trees, weights, controller phases and skeleton ownership survive.
bool KHZ_ClientPoseRefresh(cgs_t* gameStatic, const snapshot_s* accepted);
const char* KHZ_ClientPoseFailureReason();
void KHZ_ClientPoseRelease();
bool KHZ_ClientPoseAccepts(const snapshot_s* accepted);
bool KHZ_ClientPoseAccept(const snapshot_s* accepted);
bool KHZ_ClientPoseTick();
void KHZ_ClientPoseResetAimTargets();
DObj_s* KHZ_ClientPoseGetDObj(unsigned handle);
bool KHZ_ClientPoseOwnsDObj(const DObj_s* object);
std::uint32_t KHZ_ClientPoseCachedAnimMap(const XAnimParts* parts, const XModelNameMap* map);

// Native XAnim uses its own bounded 4096-node arena for this domain, including
// independent allocation/free indices, usage accounting and notify scratch.
// Cloning is an explicit main-thread operation, never an assertion bypass.
void KHZ_XAnimOwnerReset();
void KHZ_XAnimOwnerCloneTree(const XAnimTree_s* source, XAnimTree_s* destination);
void KHZ_XAnimOwnerClearTree(XAnimTree_s* tree);

