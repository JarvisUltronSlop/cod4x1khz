// END of physics/ode/timer.cpp, COD4X1KHZ && KISAK_MP.
#include "khz_server_physics_checkpoint_native.h"
#include <qcommon/qcommon.h>

namespace khz_server_ode_timers_detail {
using namespace khz::server_physics_checkpoint;
struct Image {
    Bytes<sizeof(event)> events;
    KHZ_ServerCheckpointFrame frame;
    int count;
    bool captured;
};
Image image;
bool registered;
double pinnedFrequency;
bool idle(char* text, unsigned capacity) {
    // The compiled Win32 solver has TIMING disabled. Any native timer start
    // makes num nonzero permanently, so it also exposes initSlots' otherwise
    // inaccessible function-static initialization. No fake timer reset.
    if (num != 0 || !zero(event, sizeof(event)))
        return reject(text, capacity, "server_ode_debug_timers_used_or_pending_external_report");
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        pinnedFrequency != double(frequency.QuadPart))
        return reject(text, capacity, "server_ode_qpc_calibration_identity_changed");
    return true;
}
bool capture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    image.captured = false;
    if (!context(text, capacity) || !KHZ_ServerPhysicsFenceIdle(f, text, capacity) || !idle(text, capacity)) return false;
    image.events.capture(event); image.count = num; image.frame = f; image.captured = true; return true;
}
bool validate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity) || !image.captured || !khz::server_checkpoint::sameLifetime(image.frame, f))
        return reject(text, capacity, "server_ode_timer_original_arena_or_saved_image_missing");
    if (!KHZ_ServerPhysicsFenceIdle(f, text, capacity)) return false;
    return idle(text, capacity);
}
void restore(const KHZ_ServerCheckpointFrame& f) { KHZ_ServerPhysicsRequireCommitFence(f); image.events.restore(event); num = image.count; }
void discard() { image.captured = false; }
}
void KHZ_ServerODETimersRegisterCheckpoint() {
    using namespace khz_server_ode_timers_detail;
    // Prewarm the real immutable hardware calibration outside capture/ticks.
    // Its local query/hz cache is never a gameplay clock or mutable deadline.
    pinnedFrequency = dTimerTicksPerSecond();
    const KHZ_ServerCheckpointProvider provider{KHZ_ServerCheckpointDomain::Physics, 3, sizeof(image),
        capture, validate, restore, discard};
    if (registered || !std::isfinite(pinnedFrequency) || pinnedFrequency <= 0 || !registerPart(provider) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, event, sizeof(event)) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, &num, sizeof(num)))
        Com_Error(ERR_FATAL, "Could not register real compiled ODE timer metadata checkpoint");
    if (!KHZ_ServerPhysicsFenceEnroll(3))
        Com_Error(ERR_FATAL, "Could not enroll actual native Physics checkpoint fence part 3");
    registered = true;
}
