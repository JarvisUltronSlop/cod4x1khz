#pragma once
#include <client_checkpoint.hpp>
#include <cstddef>
#include <cstdint>

using KHZ_ClientCheckpointFrame = khz::checkpoint::Frame;
using KHZ_ClientCheckpointDomain = khz::checkpoint::Domain;

struct KHZ_ClientCheckpointProvider {
    KHZ_ClientCheckpointDomain domain;
    std::size_t storageBytes;
    // All callbacks execute on the real simulation worker while it remains
    // parked. ValidateRestore must never change live state. Restore is called
    // only after EVERY provider passed validation under the same pinned assets.
    bool (*capture)(const KHZ_ClientCheckpointFrame&, char* reason, unsigned capacity);
    bool (*validateRestore)(const KHZ_ClientCheckpointFrame&, char* reason, unsigned capacity);
    bool (*restore)(const KHZ_ClientCheckpointFrame&, std::uint64_t rebasedQpcEpoch,
        char* reason, unsigned capacity);
    void (*discard)();
};

// Register all domains on main before starting an owner. Duplicate/missing
// providers reject. NativeClient is registered by this module; the owner, input,
// native pool and transport/runtime modules register their actual mementos.
bool KHZ_ClientCheckpointRegister(const KHZ_ClientCheckpointProvider& provider);
void KHZ_InitClientCheckpoint();

// Main caller must first fence server/client transport, park Raw Input and the
// simulation owner, consume presentation journals, and pin the loaded map.
// The worker verifies its actual domain state again; these calls never infer
// quiescence or restore success from an external Boolean or a hash comparison.
bool KHZ_ClientCheckpointCapture(std::uint64_t fixture, unsigned timeoutMilliseconds = 5000);
bool KHZ_ClientCheckpointValidateRestore(unsigned timeoutMilliseconds = 5000);
bool KHZ_ClientCheckpointRestore(unsigned timeoutMilliseconds = 5000);
bool KHZ_ClientCheckpointDiscard(unsigned timeoutMilliseconds = 5000);
const char* KHZ_ClientCheckpointStatusJSON();
bool KHZ_ClientCheckpointPinned();

// Called at joined/stopped lifetime boundaries only after the worker discarded
// checkpoint-owned native references. Prevent unloading pinned map assets.
bool KHZ_ClientCheckpointCanReleaseAssets();

// Owner integration: synchronous acknowledged parked-worker requests; the
// callback runs under its native error boundary with bgs/collision unbound.
// A timeout may NOT resume or unload the still-pinned owner/map.
bool KHZ_ClientOwnerRunParkedTask(bool (*callback)(void*), void* context,
    unsigned timeoutMilliseconds);
bool KHZ_ClientOwnerParkCheckpoint(unsigned timeoutMilliseconds = 5000);
bool KHZ_ClientOwnerCheckpointContext();
bool KHZ_ClientOwnerParkedTaskOutstanding();
// Export logical clock/input epoch and verify empty event/reliable/navigation
// journals, no pending config/lifecycle transaction, and actual map identity.
bool KHZ_ClientOwnerCheckpointObserve(KHZ_ClientCheckpointFrame* frame,
    char* reason, unsigned capacity);
void KHZ_ClientOwnerRegisterCheckpoint();
bool KHZ_ClientOwnerCheckpointResetPresentation();
bool KHZ_ClientOwnerResumeCheckpoint(unsigned timeoutMilliseconds = 5000);
// Real worker exit/error cleanup, while its TLS checkpoint context is active.
void KHZ_ClientCheckpointWorkerExit();
