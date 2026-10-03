#pragma once

struct cg_s;
struct cgs_t;
struct centity_s;
struct FxSystem;
struct FxSystemBuffers;
struct FxEffectDef;

// Main prepares immutable filtered asset views and migrates active gameplay FX
// while both owners/render jobs are parked. Tick/events are simulation-only.
// Native particle lifetime, velocity, size/alpha and deterministic seed logic
// is reused; no render vertices, marks, sound or presentation camera is used.
bool KHZ_ClientVisibilityPrepare(cg_s* game, cgs_t* gameStatic);
// Main-only cache extension; preserves live particles, native seeds, lifetimes,
// entity references and emission residuals under an acknowledged owner park.
bool KHZ_ClientVisibilityRefresh(cgs_t* gameStatic);
const char* KHZ_ClientVisibilityFailureReason();
unsigned KHZ_ClientVisibilityMigratedBolts();
void KHZ_ClientVisibilityRelease();
bool KHZ_ClientVisibilityTick();
void KHZ_ClientVisibilityEntityEvent(const centity_s* entity, int event);
FxSystem* KHZ_ClientVisibilitySystem();
FxSystemBuffers* KHZ_ClientVisibilityBuffers();
bool KHZ_ClientVisibilityIsSystem(const FxSystem* system);
const FxEffectDef* KHZ_ClientVisibilityEffectDef(const FxEffectDef* original);
bool KHZ_ClientVisibilityWantsElement(const FxEffectDef* definition, unsigned element);

