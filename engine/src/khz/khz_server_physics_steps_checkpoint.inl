// END of physics/phys_contacts.cpp, COD4X1KHZ && KISAK_MP.
#include "khz_server_physics_checkpoint_native.h"
#include <qcommon/qcommon.h>

namespace khz_server_physics_steps_detail {
using namespace khz::server_physics_checkpoint;
struct Image {
    int current[3], minimum[3], maximum[3];
    KHZ_ServerCheckpointFrame frame;
    bool captured;
};
Image image;
bool registered;
bool legal(char* text, unsigned capacity) {
    for (unsigned i = 0; i < 3; ++i)
        if (g_phys_minMsecStep[i] <= 0 || g_phys_maxMsecStep[i] < g_phys_minMsecStep[i] ||
            g_phys_msecStep[i] < g_phys_minMsecStep[i] || g_phys_msecStep[i] > g_phys_maxMsecStep[i])
            return reject(text, capacity, "server_physics_native_step_arrays_invalid");
    return true;
}
bool capture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    image.captured = false;
    if (!context(text, capacity) || !KHZ_ServerPhysicsFenceIdle(f, text, capacity) || !legal(text, capacity)) return false;
    std::memcpy(image.current, g_phys_msecStep, sizeof(image.current));
    std::memcpy(image.minimum, g_phys_minMsecStep, sizeof(image.minimum));
    std::memcpy(image.maximum, g_phys_maxMsecStep, sizeof(image.maximum));
    image.frame = f; image.captured = true; return true;
}
bool validate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity) || !image.captured || !khz::server_checkpoint::sameLifetime(image.frame, f))
        return reject(text, capacity, "server_physics_step_original_image_or_lifetime_missing");
    if (!KHZ_ServerPhysicsFenceIdle(f, text, capacity)) return false;
    return legal(text, capacity);
}
void restore(const KHZ_ServerCheckpointFrame& f) {
    KHZ_ServerPhysicsRequireCommitFence(f);
    std::memcpy(g_phys_msecStep, image.current, sizeof(image.current));
    std::memcpy(g_phys_minMsecStep, image.minimum, sizeof(image.minimum));
    std::memcpy(g_phys_maxMsecStep, image.maximum, sizeof(image.maximum));
}
void discard() { image.captured = false; }
}
void KHZ_ServerPhysicsStepsRegisterCheckpoint() {
    using namespace khz_server_physics_steps_detail;
    const KHZ_ServerCheckpointProvider provider{KHZ_ServerCheckpointDomain::Physics, 7, sizeof(image),
        capture, validate, restore, discard};
    if (registered || !registerPart(provider) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, g_phys_msecStep, sizeof(g_phys_msecStep)) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, g_phys_minMsecStep, sizeof(g_phys_minMsecStep)) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, g_phys_maxMsecStep, sizeof(g_phys_maxMsecStep)))
        Com_Error(ERR_FATAL, "Could not register actual native physics step-array checkpoint");
    if (!KHZ_ServerPhysicsFenceEnroll(7))
        Com_Error(ERR_FATAL, "Could not enroll actual native Physics checkpoint fence part 7");
    registered = true;
}
