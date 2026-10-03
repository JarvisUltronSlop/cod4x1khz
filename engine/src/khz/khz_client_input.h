#pragma once
#include <cstdint>
namespace khz::canonical { class Hash; }

// Main calls Freeze only while the simulation owner is stopped/parked. Binding
// names and stock keyboard-layout mapping remain immutable until next start.
bool KHZ_ClientInputFreezeBindings();
// Main-only UI/DevGui predicate for producer ownership transitions. Chat and
// every other non-gameplay catcher keep native WM_CHAR/button delivery active.
bool KHZ_ClientGameplayInputAllowed();
// These three calls belong exclusively to the private simulation owner.
void KHZ_ClientInputOwnerInit(std::uint64_t controlEpoch);
void KHZ_ClientInputOwnerResume(std::uint64_t controlEpoch);
void KHZ_ClientInputOwnerPoll(std::uint64_t qpcDeadline);
void KHZ_ClientInputOwnerStop();
bool KHZ_ClientInputSelfTest();
// Private owner observations only. Producer timeline/epoch and checkpoint
// restoration are separate replay guards, never inferred from this digest.
bool KHZ_ClientInputCanonical(khz::canonical::Hash& hash);

// Exported by the stock window mapper on main; the consumer never performs
// keyboard-layout or UI calls while processing gameplay.
void KHZ_ExportGameplayKeyMap(unsigned char (*keys)[2]);
// Native weapon adapter accepts typed stock slots, avoiding Cmd_Argv on owner.
void KHZ_ActionSlotDown(int oneBasedSlot);
void KHZ_ActionSlotUp(int oneBasedSlot);
