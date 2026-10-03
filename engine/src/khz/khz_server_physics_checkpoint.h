#pragma once
#include "khz_server_checkpoint.h"

// Source-owned fragments are included at the END of the named native .cpp,
// under COD4X1KHZ_SERVER_CHECKPOINT && KISAK_DEDICATED (default OFF).
// All eight actual native hook parts are mandatory (mask 0xff).
// This first provider restores disabled or initialized-but-empty native ODE;
// it deliberately rejects active bodies, geoms, joints and heap trimeshes.
void KHZ_ServerPhysicsRegisterCheckpoint();       // phys_ode.cpp, part 0
void KHZ_ServerODERegisterCheckpoint();           // ode/ode.cpp, part 1
void KHZ_ServerODECollidersRegisterCheckpoint();  // ode/collision_kernel.cpp, part 2
void KHZ_ServerODETimersRegisterCheckpoint();     // ode/timer.cpp, part 3
void KHZ_ServerPhysicsCapsuleRegisterCheckpoint();// phys_coll_capsulebrush.cpp, part 4
void KHZ_ServerODEErrorsRegisterCheckpoint();     // ode/error.cpp, part 5
void KHZ_ServerODEQuickstepRegisterCheckpoint();  // ode/quickstep.cpp, part 6
void KHZ_ServerPhysicsStepsRegisterCheckpoint();  // phys_contacts.cpp, part 7

// Before init/shutdown, allocation/free, object lifetime changes, native
// stepping/collision callbacks, class/handler changes or debug timer I/O.
// A refusal latches branch ineligibility and preserves the saved image.
bool KHZ_ServerPhysicsAllowUnsupportedMutation(const char* operation);
// Callback-only bridge; part 0 owns the real native flag.
bool KHZ_ServerPhysicsInitialized();

// Actual paused dedicated owner observer: close BEFORE ANY of the fourteen
// domains captures/validates/writes. Keep it through the full pinned lifetime
// and ALL cleanup, then explicitly release only after coordinator Empty.
// Each native hook holds a seq_cst entry lease for its complete function.
bool KHZ_ServerPhysicsFenceEnter(const KHZ_ServerCheckpointFrame&, char*, unsigned);
bool KHZ_ServerPhysicsFenceIdle(const KHZ_ServerCheckpointFrame&, char*, unsigned);
bool KHZ_ServerPhysicsFenceRelease(char*, unsigned);
bool KHZ_ServerPhysicsFenceEnroll(unsigned part);
// Provider commit checks identity without consuming diagnostic failures after
// all-provider validation. A foreign refusal cannot write native state.
bool KHZ_ServerPhysicsFenceCommitHeld(const KHZ_ServerCheckpointFrame&);
// Commit is infallible only under the verified fence. Treat a violated
// coordinator contract as fatal in Release too, before touching live bytes.
void KHZ_ServerPhysicsRequireCommitFence(const KHZ_ServerCheckpointFrame&);
// Main owner drains the atomically recorded foreign refusal; foreign threads
// never access coordinator state or call its main-only rejection API.
void KHZ_ServerPhysicsDrainGuardRefusals();

// Legacy ode/memory.cpp and odemisc.cpp are excluded by common_files.cmake;
// deps/ode/memory.h disables its handlers under #if0. They are not runtime
// providers. Native pool storage is restored by parts 0/1; the actual solver
// RNG fields/alias are restored by parts 1/6.
