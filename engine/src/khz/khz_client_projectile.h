#pragma once

struct cg_s;
struct centity_s;
struct snapshot_s;
struct DObj_s;

// Immutable native single-model descriptors are prepared under the map asset
// barrier. Each missile owns its mutable DObj/skeleton fields in fixed storage;
// no object allocation, renderer lookup, or visual physics runs on its tick.
bool KHZ_ClientProjectilePrepare(cg_s* game, centity_s* entities);
bool KHZ_ClientProjectileRefresh();
void KHZ_ClientProjectileRelease();
bool KHZ_ClientProjectileAccepts(const snapshot_s* snapshot);
bool KHZ_ClientProjectileTick();
DObj_s* KHZ_ClientProjectileGetDObj(unsigned handle);
bool KHZ_ClientProjectileOwnsDObj(const DObj_s* object);
const char* KHZ_ClientProjectileFailureReason();
unsigned KHZ_ClientProjectileModelCount();
bool KHZ_ClientProjectileTrailPending(unsigned handle);
void KHZ_ClientProjectileMarkTrail(unsigned handle);
