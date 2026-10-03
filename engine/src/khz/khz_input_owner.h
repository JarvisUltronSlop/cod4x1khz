#pragma once

#include <cstdint>

enum class KHZ_OwnedInputKind : unsigned { Key, Mouse, Reset };
enum class KHZ_InputResetReason : unsigned { Control, FocusLost, FocusGained, Park, Overflow, Stop };
struct KHZ_OwnedInputEvent {
    KHZ_OwnedInputKind kind{};
    std::uint64_t qpc = 0, sequence = 0, controlEpoch = 0;
    unsigned messageTimeMs = 0;
    unsigned virtualKey = 0, scanCode = 0, keyboardFlags = 0;
    bool keyDown = false;
    int dx = 0, dy = 0;
    unsigned mouseButtonFlags = 0;
    int wheelDelta = 0;
    KHZ_InputResetReason resetReason{};
};
struct KHZ_InputOwnerStats {
    std::uint64_t produced = 0, consumed = 0, dropped = 0, resets = 0, focusChanges = 0;
    std::uint64_t windowRebinds = 0, windowRebindFailures = 0;
    unsigned queued = 0, queueCapacity = 4096, lastError = 0, producerThreadId = 0;
    bool running = false, gameplay = false, focused = false, parked = true;
};

// Frontend/control owner calls lifecycle APIs at a simulation barrier. Pop has
// exactly one consumer: the private simulation owner. Never reset/free its state
// until that consumer is quiescent. Events use QPC receipt timestamps and carry
// the caller's control epoch; discard obsolete epochs before applying bindings.
// After the simulation consumer stops, a changed valid HWND recycles only an
// acknowledged parked producer. Failure retains live producer state/handles.
bool KHZ_InputOwnerStart(std::uintptr_t mainWindow);
void KHZ_InputOwnerSetGameplay(bool enabled, std::uint64_t controlEpoch);
void KHZ_InputOwnerPark();
void KHZ_InputOwnerResume();
bool KHZ_InputOwnerStop();
bool KHZ_InputOwnerPop(KHZ_OwnedInputEvent* event);
bool KHZ_InputOwnerPopDue(KHZ_OwnedInputEvent* event, std::uint64_t qpcDeadline);
// Sole simulation consumer only, before resuming gameplay after a barrier.
void KHZ_InputOwnerDiscardPending();
bool KHZ_InputOwnerOwnsGameplay();
bool KHZ_InputOwnerOwnsRawDevices();
// Actual producer acknowledgement, not the requested Park flag. True only
// after control changes are applied and the sole consumer drained its queue.
bool KHZ_InputOwnerCheckpointParked();
bool KHZ_InputOwnerParkAcknowledged();
// Main-thread legacy keyboard/button/wheel/character filtering. MSG.time is
// retained through park/stop so queued gameplay cannot reach menu bindings.
// Owner navigation requests use their separate bounded queue and bypass this.
bool KHZ_InputOwnerSuppressLegacy(unsigned messageTimeMs);
KHZ_InputOwnerStats KHZ_GetInputOwnerStats();
bool KHZ_InputOwnerSelfTest();
