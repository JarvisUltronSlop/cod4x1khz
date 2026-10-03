#pragma once
#include "khz_input_owner.h"
#include <canonical.hpp>
#include <array>
#include <cstdint>

// Diagnostic private-owner state. The checkpoint controller separately proves
// matching session/map generation and complete server/world/script restoration.
struct KHZ_InputButtonCheckpoint {
    int down[2]; unsigned downtime, msec; int active, wasPressed;
};
struct KHZ_ClientInputCheckpoint {
    unsigned schema;
    KHZ_InputButtonCheckpoint buttons[30];
    std::array<bool,256> keyDown, awaitRelease;
    int locationSelection;
    bool initialized, typedInput, mainPending, allReleased;
    std::uint64_t controlEpoch, lastSequence, lastQpc, bindingRevision;
    khz::canonical::Digest bindingIdentity;
};
struct KHZ_ClientInputReceipt {
    // Ordered relative to the saved logical tick; sub-tick is0..999us.
    // QPC/message-time/epoch/sequence inside event are unused receipt metadata.
    std::uint64_t tickOffset;
    unsigned microseconds;
    KHZ_OwnedInputEvent event;
};
inline constexpr unsigned KHZ_MaxInputReplayReceipts=4096;
// These operations execute on CLIENT_SIM inside the controller's parked-worker
// callback. Hardware producer park must be acknowledged and its queue drained.
bool KHZ_ClientInputCheckpointCapture(KHZ_ClientInputCheckpoint* output);
bool KHZ_ClientInputCheckpointValidateRestore(const KHZ_ClientInputCheckpoint& checkpoint);
bool KHZ_ClientInputCheckpointRestore(const KHZ_ClientInputCheckpoint& checkpoint,
    std::uint64_t rebasedQpcEpoch);
void KHZ_ClientInputCheckpointDiscard();
// Explicit all-release reset is fixture-only and reports its actual state in
// Capture. It never rewinds or claims to restore the hardware producer queue.
bool KHZ_ClientInputCheckpointPrepareReleased();
bool KHZ_ClientInputReplayBegin(const KHZ_ClientInputReceipt* receipts,unsigned count,
    std::uint64_t qpcEpoch,std::uint64_t qpcFrequency,unsigned nativeMillisecondsEpoch);
// Actual worker resume boundary only, before consuming receipt0. Presentation
// rewind/IPC work between Restore and Resume must not create simulation debt.
bool KHZ_ClientInputReplayRebaseEpoch(std::uint64_t qpcEpoch);
// Actual parked worker only, with the hardware producer acknowledged/drained.
// Rebase synthetic receipt guards before the producer resumes real input.
void KHZ_ClientInputReplayEnd();
bool KHZ_ClientInputReplayActive();
bool KHZ_ClientInputReplayFailed();
void KHZ_ClientInputRegisterCheckpoint();
