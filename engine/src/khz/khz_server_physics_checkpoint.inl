// END of physics/phys_ode.cpp, COD4X1KHZ && KISAK_MP.
#include "khz_server_physics_checkpoint_native.h"
#include "khz_server_physics_guards.h"
#include <qcommon/qcommon.h>
#include <cstdlib>

namespace khz_server_physics_detail {
using namespace khz::server_physics_checkpoint;
constexpr unsigned BindingCount = 38;
struct Image {
    Bytes<sizeof(physGlob)> state;
    KHZ_ServerCheckpointFrame frame;
    const dvar_t* bindings[BindingCount];
    bool initialized, captured;
};
Image image; // Raw BSS, no PhysGlob constructor or giant temporary.
bool registered;
void bindings(const dvar_t* (&out)[BindingCount]) {
    const dvar_t* const values[] = {
        phys_contact_erp, phys_autoDisableAngular, phys_drawcontacts, phys_bulletUpBias,
        phys_joint_stop_erp, phys_noIslands, phys_dragLinear, phys_joint_cfm,
        phys_gravityChangeWakeupRadius, phys_drawAwake, phys_contact_cfm,
        phys_narrowObjMaxLength, phys_minImpactMomentum, phys_autoDisableTime,
        phys_bulletSpinScale, phys_drawAwakeTooLong, phys_cfm, phys_drawCollisionWorld,
        phys_visibleTris, phys_gravity, phys_collUseEntities, phys_autoDisableLinear,
        phys_contact_cfm_ragdoll, phys_drawCollisionObj, phys_joint_stop_cfm, phys_erp,
        phys_dragAngular, phys_frictionScale, phys_dumpcontacts, phys_mcv_ragdoll,
        phys_csl, phys_contact_erp_ragdoll, phys_reorderConst, phys_interBodyCollision,
        phys_drawDebugInfo, phys_qsi, phys_jitterMaxMass, phys_mcv
    };
    static_assert(sizeof(values) == sizeof(out)); std::memcpy(out, values, sizeof(values));
}
bool empty(char* text, unsigned capacity) {
    if (physGlob.debugActiveObjCount || physGlob.triMeshInfo.verts || physGlob.triMeshInfo.indices ||
        physGlob.triMeshDataID || physGlob.visTrisGeom)
        return reject(text, capacity, "server_physics_active_debug_object_or_unrestored_heap_trimesh");
    for (unsigned i = 0; i < 3; ++i) {
        if (physGlob.world[i] != (physInited ? &odeGlob.world[i] : nullptr) ||
            physGlob.space[i] != (physInited ? &odeGlob.space[i] : nullptr) ||
            physGlob.contactgroup[i] != (physInited ? &odeGlob.contactsGroup[i] : nullptr))
            return reject(text, capacity, "server_physics_native_roots_foreign_or_initialization_inconsistent");
        const auto& data = physGlob.worldData[i];
        if (data.collisionCallback || data.numJitterRegions < 0 || data.numJitterRegions > 5 ||
            !finite(data.timeNowLerpFrac))
            return reject(text, capacity, "server_physics_external_callback_or_invalid_world_deadline_metadata");
        for (int j = 0; j < data.numJitterRegions; ++j) {
            const float* values = reinterpret_cast<const float*>(&data.jitterRegions[j]);
            for (unsigned n = 0; n < sizeof(Jitter) / sizeof(float); ++n)
                if (!finite(values[n])) return reject(text, capacity, "server_physics_jitter_metadata_nonfinite");
        }
    }
    if (physGlob.worldGeom && physGlob.worldGeom != &odeGlob.worldGeom)
        return reject(text, capacity, "server_physics_foreign_world_geom");
    if (physInited && physGlob.worldGeom != &odeGlob.worldGeom)
        return reject(text, capacity, "server_physics_initialized_world_geom_missing");
    if (!emptyPool(physGlob.userDataPool, physGlob.userData, sizeof(PhysObjUserData), 512, !physInited) ||
        !emptyIndices(physGlob.hingeArray.freeEntry, physGlob.hingeArray.entries, !physInited) ||
        !emptyIndices(physGlob.ballArray.freeEntry, physGlob.ballArray.entries, !physInited) ||
        !emptyIndices(physGlob.aMotorArray.freeEntry, physGlob.aMotorArray.entries, !physInited))
        return reject(text, capacity, "server_physics_active_or_corrupt_userdata_persistent_joint_free_topology");
    for (float f : physGlob.gravityDirection)
        if (!finite(f)) return reject(text, capacity, "server_physics_gravity_metadata_nonfinite");
    return true;
}
bool capture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    image.captured = false;
    if (!context(text, capacity) || !KHZ_ServerPhysicsFenceIdle(f, text, capacity) || !empty(text, capacity)) return false;
    bindings(image.bindings); image.state.capture(&physGlob);
    image.initialized = physInited; image.frame = f; image.captured = true; return true;
}
bool validate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity) || !image.captured || !khz::server_checkpoint::sameLifetime(image.frame, f))
        return reject(text, capacity, "server_physics_original_arena_or_saved_image_missing");
    if (!KHZ_ServerPhysicsFenceIdle(f, text, capacity)) return false;
    const dvar_t* live[BindingCount]; bindings(live);
    if (physInited != image.initialized || std::memcmp(live, image.bindings, sizeof(live)))
        return reject(text, capacity, "server_physics_initialization_or_native_dvar_binding_lifetime_changed");
    return empty(text, capacity);
}
void restore(const KHZ_ServerCheckpointFrame& f) {
    KHZ_ServerPhysicsRequireCommitFence(f);
    iassert(KHZ_ServerCheckpointContext() && image.captured);
    image.state.restore(&physGlob); physInited = image.initialized;
}
void discard() { image.captured = false; }
}

void KHZ_ServerPhysicsRegisterCheckpoint() {
    using namespace khz_server_physics_detail;
    const KHZ_ServerCheckpointProvider provider{KHZ_ServerCheckpointDomain::Physics, 0, sizeof(image),
        capture, validate, restore, discard};
    if (registered || !registerPart(provider) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, &physGlob, sizeof(physGlob)) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, &physInited, sizeof(physInited)))
        Com_Error(ERR_FATAL, "Could not register actual native empty physics checkpoint");
    if (!KHZ_ServerPhysicsFenceEnroll(0))
        Com_Error(ERR_FATAL, "Could not enroll actual native Physics checkpoint fence part 0");
    registered = true;
}
bool KHZ_ServerPhysicsInitialized() {
    iassert(KHZ_ServerCheckpointContext()); return physInited;
}
#include "khz_server_physics_fence.inl"
