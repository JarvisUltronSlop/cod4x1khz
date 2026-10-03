#pragma once

#include <cstdint>
#include <cstdarg>

struct cg_s;
struct cgs_t;
struct centity_s;
struct weaponInfo_s;
struct clientActive_t;
struct clientConnection_t;
struct clientUIActive_t;
struct clientStatic_t;
struct snapshot_s;
struct playerState_s;
struct transPlayerState_t;
struct AimAssistGlobals;
struct snd_alias_list_t;
struct usercmd_s;
struct bgs_t;
namespace khz { struct GameplayRandomState; }

// Start only after the main thread has completed map/CG initialization. The
// callbacks run on the simulation owner. They must not render, pump the main
// window, execute Cbuf, or perform asset/lifecycle work. A packet which requires
// one of those operations requests a park before its payload is applied.
struct KHZ_ClientOwnerHooks {
    void (*pumpTransport)();
    void (*tick)(std::uint64_t tick, std::uint64_t inputDeadline);
};

bool KHZ_ClientOwnerStart(std::uint64_t session, std::uint64_t mapGeneration,
    const KHZ_ClientOwnerHooks& hooks);
// Main checks this before changing the Raw Input epoch or attempting preparation.
// A failed preparation retries at most twice per second and retains its reason.
bool KHZ_ClientOwnerCanAttemptStart();
bool KHZ_ClientOwnerPark(unsigned timeoutMilliseconds = 5000);
// These are for gameplay controls under an acknowledged park, after root has
// classified the command as retaining map/asset lifetime. Begin temporarily
// routes native CL/CG control access to the owner; End restores presentation.
bool KHZ_ClientOwnerBeginControls();
void KHZ_ClientOwnerEndControls();
bool KHZ_ClientOwnerResumeControls(unsigned timeoutMilliseconds = 5000);
bool KHZ_ClientOwnerControlsActive();
const char* KHZ_ClientOwnerBarrierFailure();
// Main control scopes may acknowledge a worker park that already belongs to
// reliable metadata or map/asset lifetime work. Its original barrier must drain
// before a generic console/connection scope can resume the worker.
bool KHZ_ClientOwnerNativeBarrierPending();
void KHZ_ClientOwnerStop();
bool KHZ_ClientOwnerRunning();
bool KHZ_IsClientSimulationThread();
bool KHZ_ClientOwnerOwnsAnimationState(const bgs_t* state);
// Worker or acknowledged main controls select the same bounded owner state.
// Other contexts return null and retain their native thread-local streams.
khz::GameplayRandomState* KHZ_ClientOwnerRandomState();
std::uint64_t KHZ_ClientOwnerTickNumber();
std::uint64_t KHZ_ClientOwnerInputDeadline();
unsigned KHZ_ClientOwnerMilliseconds();
unsigned KHZ_ClientOwnerMillisecondsAt(std::uint64_t counter);
void KHZ_ClientOwnerRequestMainKey(int key, int down, unsigned milliseconds);
bool KHZ_ClientOwnerPopMainKey(int* key, int* down, unsigned* milliseconds);
void KHZ_ClientOwnerPublishKeyCatchers(int catchers);
// Input-only synthetic blockers (for example DevGui) never become UI state.
void KHZ_ClientOwnerPublishInputCatchers(int catchers);
int KHZ_ClientOwnerKeyCatchers();
void KHZ_ClientOwnerSetLocationSelection(bool enabled);
void KHZ_ClientOwnerRequestPark(const char* reason);
void KHZ_ClientOwnerRequestAssetRebuild(const char* reason);
bool KHZ_ClientOwnerNeedsAssetRebuild();
enum class KHZ_ClientMainBarrierResult { None, Resume, MapLifecycle };
// Called on main after Park + ConsumePresentation. Native reliable handlers
// run once in presentation; only immutable/config metadata returns to owner.
KHZ_ClientMainBarrierResult KHZ_ClientOwnerServiceMainBarrier();
bool KHZ_ClientOwnerNeedsMainThread();
bool KHZ_ClientOwnerHadFault();
int KHZ_ClientOwnerErrorCode();
std::uint64_t KHZ_ClientOwnerGeneration();
const char* KHZ_ClientOwnerParkReason();
const char* KHZ_ClientOwnerStatusJSON();
// Main-only native pre-owner/presentation scanner expiry, distinct from
// intentional coalescing of immutable presentation publications.
void KHZ_ClientSnapshotExpiryCounters(std::uint64_t* messages, std::uint64_t* scans);

// Called before Com_Error touches its process-global error/UI state. Returns
// false off the owner. On the owner it formats a bounded fault and jumps only to
// this thread's guarded callback boundary; it never enters the main error path.
bool KHZ_ClientOwnerCatchError(int code, const char* format, va_list arguments);

// Header accessors route EVERY direct array access, including legacy code that
// bypasses CL/CG getters. Owner CL storage is the original engine storage; main
// presentation sees independent mirrors without network/command history copies.
clientActive_t (&KHZ_ClientActiveArray())[1];
clientConnection_t (&KHZ_ClientConnectionArray())[1];
clientUIActive_t (&KHZ_ClientUIArray())[1];
clientStatic_t& KHZ_ClientStaticState();
cg_s (&KHZ_CGameArray())[1];
cgs_t (&KHZ_CGameStaticArray())[1];
centity_s (&KHZ_CEntityArray())[1][1024];
weaponInfo_s (&KHZ_CWeaponArray())[1][128];
float (&KHZ_CEntityOriginArray())[1][1024][3];

// Gameplay replaces the visual CG_ProcessSnapshots path with this accepted
// snapshot transition. Private gameplay DObj/XAnim/opacity scratch is owned by
// simulation; renderer links, visual DObjs/FX and sound remain main-owned.
// Snapshot collision has no unsupported-world fallback on a worker.
void KHZ_ClientOwnerProcessSnapshots();
bool KHZ_ClientOwnerBeginCollisionFrame();
void KHZ_ClientOwnerPublishPrediction();
void KHZ_ClientOwnerConsumePresentation();

// A main-thread snapshot provider reads stable published state rather than CL's
// concurrently changing baseline/parse rings. Return false when not owning.
bool KHZ_ClientPresentationSnapshotNumber(int* number, int* serverTime);
bool KHZ_ClientPresentationSnapshot(int number, snapshot_s* output, bool* found);
bool KHZ_ClientPresentationUserCommand(int number, usercmd_s* output, bool* found);
bool KHZ_ClientPresentationServerCommand(int number, char* output, unsigned capacity, bool* found);

// Main -> owner reliable commands are bounded and ordered. Return true when
// intercepted, including overflow (which parks with an explicit fault).
bool KHZ_ClientOwnerQueueReliable(const char* text);
// Bounded main/service ingress with backpressure; never faults or parks when
// the queue is full. The service retains its transfer cursor until true.
bool KHZ_ClientOwnerTryQueueReliable(const char* text);
bool KHZ_ClientOwnerPopReliable(char* text, unsigned capacity);

// Sound aliases and accepted/predicted events form an ordered visual journal.
// The engine event split applies input/recoil/state feedback on the owner and
// queues only the presentation side effects here.
void KHZ_ClientOwnerQueueEntityEvent(const centity_s* entity, int event);
int KHZ_ClientOwnerQueueAnimationSound(int entity, snd_alias_list_t* alias);
void KHZ_ClientOwnerQueueMenuNotify(int menu);
bool KHZ_ClientOwnerQueuePlayerTransition(playerState_s* state,
    const transPlayerState_t* previous, int* previousSequence);
bool KHZ_ClientOwnerReplayingVisualEvent();

// Native gameplay pose/opacity/aim runs on the simulation owner. Presentation
// computes separate HUD targets and never changes gameplay melee transitions.
AimAssistGlobals* KHZ_ClientOwnerAimContext(int localClientNum,
    AimAssistGlobals* presentation);
void KHZ_ClientOwnerPublishAimTargets(const AimAssistGlobals* presentation);
void KHZ_ClientOwnerReadAimTargets();

