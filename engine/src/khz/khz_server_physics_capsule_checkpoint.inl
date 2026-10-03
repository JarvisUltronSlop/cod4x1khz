// END of physics/phys_coll_capsulebrush.cpp, COD4X1KHZ && KISAK_MP.
#include "khz_server_physics_checkpoint_native.h"
#include <qcommon/qcommon.h>

namespace khz_server_physics_capsule_detail {
using namespace khz::server_physics_checkpoint;
struct Image {
    Bytes<sizeof(localContacts)> contacts;
    KHZ_ServerCheckpointFrame frame;
    int count;
    bool captured;
};
Image image;
bool registered;
bool valid(char* text, unsigned capacity) {
    if (numLocalContacts < 0 || numLocalContacts > 32)
        return reject(text, capacity, "server_physics_capsule_scratch_count_outside_native_arena");
    for (int i = 0; i < numLocalContacts; ++i) {
        const auto& c = localContacts[i];
        if (!finite(c.depth) || (c.inUse != 0 && c.inUse != 1))
            return reject(text, capacity, "server_physics_capsule_scratch_contact_invalid");
        for (unsigned j = 0; j < 3; ++j) if (!finite(c.pos[j]) || !finite(c.normal[j]))
            return reject(text, capacity, "server_physics_capsule_scratch_vector_nonfinite");
    }
    return true;
}
bool capture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    image.captured = false;
    if (!context(text, capacity) || !KHZ_ServerPhysicsFenceIdle(f, text, capacity) || !valid(text, capacity)) return false;
    image.contacts.capture(localContacts); image.count = numLocalContacts;
    image.frame = f; image.captured = true; return true;
}
bool validate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity) || !image.captured || !khz::server_checkpoint::sameLifetime(image.frame, f))
        return reject(text, capacity, "server_physics_capsule_original_image_or_lifetime_missing");
    if (!KHZ_ServerPhysicsFenceIdle(f, text, capacity)) return false;
    return valid(text, capacity);
}
void restore(const KHZ_ServerCheckpointFrame& f) { KHZ_ServerPhysicsRequireCommitFence(f); image.contacts.restore(localContacts); numLocalContacts = image.count; }
void discard() { image.captured = false; }
}
void KHZ_ServerPhysicsCapsuleRegisterCheckpoint() {
    using namespace khz_server_physics_capsule_detail;
    const KHZ_ServerCheckpointProvider provider{KHZ_ServerCheckpointDomain::Physics, 4, sizeof(image),
        capture, validate, restore, discard};
    if (registered || !registerPart(provider) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, localContacts, sizeof(localContacts)) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, &numLocalContacts, sizeof(numLocalContacts)))
        Com_Error(ERR_FATAL, "Could not register native physics capsule scratch checkpoint");
    if (!KHZ_ServerPhysicsFenceEnroll(4))
        Com_Error(ERR_FATAL, "Could not enroll actual native Physics checkpoint fence part 4");
    registered = true;
}
