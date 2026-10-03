#pragma once
#include <server_checkpoint.hpp>

using KHZ_ServerCheckpointFrame = khz::server_checkpoint::Frame;
using KHZ_ServerCheckpointDomain = khz::server_checkpoint::Domain;
using KHZ_ServerCheckpointProvider = khz::server_checkpoint::Provider;

// Real dedicated owner only. Observer must verify the authenticated managed
// host fixture/session, completed tick, actual pause and drained transport,
// loaded map/hunk identities, and fixture eligibility again on every operation.
using KHZ_ServerCheckpointObserver = bool (*)(std::uint64_t fixture,
    KHZ_ServerCheckpointFrame*, char* reason, unsigned capacity);
bool KHZ_InitServerCheckpoint(KHZ_ServerCheckpointObserver observer);
bool KHZ_ServerCheckpointRegister(const KHZ_ServerCheckpointProvider&);
bool KHZ_ServerCheckpointRequireParts(KHZ_ServerCheckpointDomain, unsigned mask);
// Register only live spans that a concrete native provider actually restores.
// Pointer-bearing graph providers reject references outside these spans.
bool KHZ_ServerCheckpointRegisterRange(KHZ_ServerCheckpointDomain, const void*, std::size_t bytes);
// Loaded-map allocations are separate from static arenas. Replacement validates
// every exact span before retiring a provider's previous map bindings.
bool KHZ_ServerCheckpointReplaceMapRanges(KHZ_ServerCheckpointDomain,
    const void* const* bases, const std::size_t* bytes, unsigned count);
bool KHZ_ServerCheckpointResetMapRanges();
bool KHZ_ServerCheckpointRestoresRange(KHZ_ServerCheckpointDomain, const void*, std::size_t bytes);
bool KHZ_ServerCheckpointRestoresAddress(const void*, std::size_t bytes);
bool KHZ_ServerCheckpointCapture(std::uint64_t fixture);
bool KHZ_ServerCheckpointValidateRestore();
bool KHZ_ServerCheckpointRestore();
bool KHZ_ServerCheckpointDiscard();
bool KHZ_ServerCheckpointPinned();
bool KHZ_ServerCheckpointContext();
bool KHZ_ServerCheckpointCanReleaseAssets();
const char* KHZ_ServerCheckpointStatusJSON();
// Latches an unsupported branch effect without dropping the pinned image.
// Call before the attempted I/O/admission/mutation; fixture replay must reject
// the branch, but may restore its saved baseline for controlled cleanup.
void KHZ_ServerCheckpointRejectExternalEffect(const char* reason);
bool KHZ_ServerCheckpointBranchEligible();

// Include the matching source-owned fragment at the END of its native .cpp
// under COD4X1KHZ && KISAK_MP. Registration runs on main before gameplay.
void KHZ_ServerScriptVMRegisterCheckpoint();       // scr_vm.cpp, ScriptVM part0
bool KHZ_ServerScriptCheckpointIdle(char* reason, unsigned capacity);
void KHZ_ServerScriptVariablesRegisterCheckpoint();// scr_variable.cpp, ScriptVM part1
void KHZ_ServerMTRegisterCheckpoint();             // scr_memorytree.cpp, Allocators part0
void KHZ_ServerSLRegisterCheckpoint();             // scr_stringlist.cpp, Allocators part1
void KHZ_ServerEntHandlesRegisterCheckpoint();     // enthandle.cpp, Graphs part0
void KHZ_ServerDObjRegisterCheckpoint();           // dobj_management.cpp, Graphs part1
