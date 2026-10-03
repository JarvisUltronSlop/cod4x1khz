#pragma once
#include <canonical.hpp>

struct playerState_s;
struct usercmd_s;
struct entityState_s;
struct clientState_s;
struct clientInfo_t;
struct cg_s;
struct clientActive_t;
struct AimAssistGlobals;
struct FxSystem;

// Explicit, reviewed native field serializers shared by replay diagnostics.
// No object padding, pointer value, unused history tail or renderer cache is
// serialized. These are owner-only observations; they do not restore a world.
void KHZ_CanonicalPlayer(khz::canonical::Hash& hash, const playerState_s& state);
void KHZ_CanonicalCommand(khz::canonical::Hash& hash, const usercmd_s& command);
void KHZ_CanonicalEntity(khz::canonical::Hash& hash, const entityState_s& state);
void KHZ_CanonicalClient(khz::canonical::Hash& hash, const clientState_s& client);
void KHZ_CanonicalController(khz::canonical::Hash& hash, const clientInfo_t& client);
void KHZ_CanonicalClientFeedback(khz::canonical::Hash& hash, const cg_s& game);
void KHZ_CanonicalClientInput(khz::canonical::Hash& hash, const clientActive_t& client);
void KHZ_CanonicalAim(khz::canonical::Hash& hash, const AimAssistGlobals& aim);
// Returns false for an invalid native pool/list/reference. Bounds are checked
// before walking linked storage; rejected observations cannot pass replay.
bool KHZ_CanonicalGameplayFX(khz::canonical::Hash& hash, const FxSystem& system);
// Main, acknowledged controls barrier, loaded native player. Exhaustively
// perturb each native PS byte in a private copy to verify serializer coverage;
// no live gameplay field is changed. This is a bounded diagnostic, not a tick.
const char* KHZ_ClientCanonicalGuardsJSON();
