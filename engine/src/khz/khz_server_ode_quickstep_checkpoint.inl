// END of physics/ode/quickstep.cpp, COD4X1KHZ && KISAK_MP.
#include "khz_server_physics_checkpoint_native.h"
#include <physics/ode/odeext.h>
#include <qcommon/qcommon.h>

namespace khz_server_ode_quickstep_detail {
using namespace khz::server_physics_checkpoint;
struct Image {
    unsigned int* seedAlias;
    KHZ_ServerCheckpointFrame frame;
    bool captured;
};
Image image;
bool registered;
bool nativeAlias(char* text, unsigned capacity) {
    if (!g_holdrand) return true; // Actual startup state.
    for (unsigned i = 0; i < 3; ++i)
        if (g_holdrand == &odeGlob.world[i].stepInfo.holdrand &&
            KHZ_ServerCheckpointRestoresRange(KHZ_ServerCheckpointDomain::Physics, g_holdrand, sizeof(*g_holdrand)))
            return true;
    return reject(text, capacity, "server_ode_quickstep_rng_alias_outside_restored_native_world");
}
bool capture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    image.captured = false;
    if (!context(text, capacity) || !KHZ_ServerPhysicsFenceIdle(f, text, capacity) || !nativeAlias(text, capacity)) return false;
    image.seedAlias = g_holdrand; image.frame = f; image.captured = true; return true;
}
bool validate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity) || !image.captured || !khz::server_checkpoint::sameLifetime(image.frame, f))
        return reject(text, capacity, "server_ode_solver_rng_original_image_or_lifetime_missing");
    if (!KHZ_ServerPhysicsFenceIdle(f, text, capacity)) return false;
    return nativeAlias(text, capacity);
}
void restore(const KHZ_ServerCheckpointFrame& f) {
    KHZ_ServerPhysicsRequireCommitFence(f);
    // Part 1 already restored each actual world.stepInfo.holdrand LCG seed.
    // No rand/srand wrapper or newly invented ODE RNG state is substituted.
    g_holdrand = image.seedAlias;
}
void discard() { image.captured = false; }
}
void KHZ_ServerODEQuickstepRegisterCheckpoint() {
    using namespace khz_server_ode_quickstep_detail;
    const KHZ_ServerCheckpointProvider provider{KHZ_ServerCheckpointDomain::Physics, 6, sizeof(image),
        capture, validate, restore, discard};
    if (registered || !registerPart(provider) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, &g_holdrand, sizeof(g_holdrand)))
        Com_Error(ERR_FATAL, "Could not register actual ODE quickstep RNG alias checkpoint");
    if (!KHZ_ServerPhysicsFenceEnroll(6))
        Com_Error(ERR_FATAL, "Could not enroll actual native Physics checkpoint fence part 6");
    registered = true;
}
