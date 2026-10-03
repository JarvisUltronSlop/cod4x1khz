// END of physics/ode/error.cpp, COD4X1KHZ && KISAK_MP.
#include "khz_server_physics_checkpoint_native.h"
#include <qcommon/qcommon.h>

namespace khz_server_ode_errors_detail {
using namespace khz::server_physics_checkpoint;
struct Image {
    dMessageFunction *error, *debug, *message;
    KHZ_ServerCheckpointFrame frame;
    bool captured;
};
Image image;
bool registered;
bool nativeDefault(char* text, unsigned capacity) {
    // No mutable external callback context/OS ownership provider exists.
    // First stock fixture preserves the actual default NULL handler set.
    return (!error_function && !debug_function && !message_function) ? true :
        reject(text, capacity, "server_ode_custom_error_callback_context_unrestored");
}
bool capture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    image.captured = false;
    if (!context(text, capacity) || !KHZ_ServerPhysicsFenceIdle(f, text, capacity) || !nativeDefault(text, capacity)) return false;
    image.error = error_function; image.debug = debug_function; image.message = message_function;
    image.frame = f; image.captured = true; return true;
}
bool validate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity) || !image.captured || !khz::server_checkpoint::sameLifetime(image.frame, f))
        return reject(text, capacity, "server_ode_error_handlers_original_image_or_lifetime_missing");
    if (!KHZ_ServerPhysicsFenceIdle(f, text, capacity)) return false;
    return nativeDefault(text, capacity);
}
void restore(const KHZ_ServerCheckpointFrame& f) {
    KHZ_ServerPhysicsRequireCommitFence(f);
    error_function = image.error; debug_function = image.debug; message_function = image.message;
}
void discard() { image.captured = false; }
}
void KHZ_ServerODEErrorsRegisterCheckpoint() {
    using namespace khz_server_ode_errors_detail;
    const KHZ_ServerCheckpointProvider provider{KHZ_ServerCheckpointDomain::Physics, 5, sizeof(image),
        capture, validate, restore, discard};
    if (registered || !registerPart(provider) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, &error_function, sizeof(error_function)) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, &debug_function, sizeof(debug_function)) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, &message_function, sizeof(message_function)))
        Com_Error(ERR_FATAL, "Could not register actual ODE error-handler checkpoint");
    if (!KHZ_ServerPhysicsFenceEnroll(5))
        Com_Error(ERR_FATAL, "Could not enroll actual native Physics checkpoint fence part 5");
    registered = true;
}
