// END of physics/ode/collision_kernel.cpp, COD4X1KHZ && KISAK_MP.
#include "khz_server_physics_checkpoint_native.h"
#include <qcommon/qcommon.h>

namespace khz_server_ode_colliders_detail {
using namespace khz::server_physics_checkpoint;
struct Image {
    Bytes<sizeof(colliders)> dispatch;
    Bytes<sizeof(user_classes)> classes;
    KHZ_ServerCheckpointFrame frame;
    int initialized, count;
    bool captured;
};
Image image;
bool registered;
bool legal(char* text, unsigned capacity) {
    if ((colliders_initialized != 0 && colliders_initialized != 1) ||
        (num_user_classes != 0 && num_user_classes != 5) ||
        (KHZ_ServerPhysicsInitialized() && (num_user_classes != 5 || !colliders_initialized)))
        return reject(text, capacity, "server_ode_native_collider_initialization_or_custom_class_set_unsupported");
    for (unsigned i = 0; i < dGeomNumClasses; ++i) for (unsigned j = 0; j < dGeomNumClasses; ++j) {
        const auto& entry = colliders[i][j];
        if ((entry.reverse != 0 && entry.reverse != 1) ||
            (entry.fn && !executablePointer(reinterpret_cast<const void*>(entry.fn))))
            return reject(text, capacity, "server_ode_collider_dispatch_not_pinned_native_code");
    }
    for (unsigned i = 0; i < dMaxUserClasses; ++i) {
        const auto& c = user_classes[i];
        if (c.bytes < 0 || c.bytes > sizeof(odeGlob.worldGeom.user_data) ||
            (c.collider && !executablePointer(reinterpret_cast<const void*>(c.collider))) ||
            (c.aabb && !executablePointer(reinterpret_cast<const void*>(c.aabb))) ||
            (c.aabb_test && !executablePointer(reinterpret_cast<const void*>(c.aabb_test))))
            return reject(text, capacity, "server_ode_custom_class_heap_payload_or_foreign_callback");
        if (i < unsigned(num_user_classes) && (!c.collider || !c.aabb))
            return reject(text, capacity, "server_ode_registered_native_custom_class_callback_missing");
    }
    return true;
}
bool capture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    image.captured = false;
    if (!context(text, capacity) || !KHZ_ServerPhysicsFenceIdle(f, text, capacity) || !legal(text, capacity)) return false;
    image.dispatch.capture(colliders); image.classes.capture(user_classes);
    image.initialized = colliders_initialized; image.count = num_user_classes;
    image.frame = f; image.captured = true; return true;
}
bool validate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity) || !image.captured || !khz::server_checkpoint::sameLifetime(image.frame, f))
        return reject(text, capacity, "server_ode_collider_original_arena_or_saved_image_missing");
    if (!KHZ_ServerPhysicsFenceIdle(f, text, capacity)) return false;
    if (!legal(text, capacity)) return false;
    // Dispatch resolution cannot occur in this empty/no-collision branch.
    // Compare exact native callback registry and initialized metadata before
    // any provider writes; changed code/class lifetime is not repaired blindly.
    if (colliders_initialized != image.initialized || num_user_classes != image.count ||
        std::memcmp(colliders, image.dispatch.value, sizeof(colliders)) ||
        std::memcmp(user_classes, image.classes.value, sizeof(user_classes)))
        return reject(text, capacity, "server_ode_native_collider_callback_or_class_lifetime_changed");
    return true;
}
void restore(const KHZ_ServerCheckpointFrame& f) {
    KHZ_ServerPhysicsRequireCommitFence(f);
    image.dispatch.restore(colliders); image.classes.restore(user_classes);
    colliders_initialized = image.initialized; num_user_classes = image.count;
}
void discard() { image.captured = false; }
}
void KHZ_ServerODECollidersRegisterCheckpoint() {
    using namespace khz_server_ode_colliders_detail;
    const KHZ_ServerCheckpointProvider provider{KHZ_ServerCheckpointDomain::Physics, 2, sizeof(image),
        capture, validate, restore, discard};
    if (registered || !registerPart(provider) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, colliders, sizeof(colliders)) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, user_classes, sizeof(user_classes)) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, &colliders_initialized, sizeof(colliders_initialized)) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, &num_user_classes, sizeof(num_user_classes)))
        Com_Error(ERR_FATAL, "Could not register native ODE collider registry checkpoint");
    if (!KHZ_ServerPhysicsFenceEnroll(2))
        Com_Error(ERR_FATAL, "Could not enroll actual native Physics checkpoint fence part 2");
    registered = true;
}
