// END of physics/ode/ode.cpp, COD4X1KHZ && KISAK_MP.
#include "khz_server_physics_checkpoint_native.h"
#include <physics/phys_local.h>
#include <qcommon/qcommon.h>

namespace khz_server_ode_detail {
using namespace khz::server_physics_checkpoint;
struct Image {
    Bytes<sizeof(odeGlob)> state;
    KHZ_ServerCheckpointFrame frame;
    std::uintptr_t vtables[4];
    bool initialized, captured;
};
Image image; // Actual object bytes, never construct another odeGlob_t.
bool registered;
bool noLinks(const dxGeom& geom) {
    return !geom.data && !geom.body && !geom.body_next && !geom.pos && !geom.R &&
        !geom.next && !geom.tome && !geom.parent_space && !(geom.gflags & GEOM_PLACEABLE);
}
bool empty(char* text, unsigned capacity, std::uintptr_t (&vtables)[4]) {
    const bool initialized = KHZ_ServerPhysicsInitialized();
    if (!emptyPool(odeGlob.bodyPool, odeGlob.bodies, sizeof(dxBody), 512, !initialized) ||
        !emptyPool(odeGlob.geomPool, odeGlob.geoms, sizeof(dxGeomTransform), ODE_GEOM_POOL_COUNT, !initialized))
        return reject(text, capacity, "server_ode_active_or_corrupt_body_geom_pool_free_topology");
    for (unsigned i = 0; i < 3; ++i) {
        const auto& world = odeGlob.world[i]; const auto& space = odeGlob.space[i];
        if (world.nb || world.nj || world.firstbody || world.firstjoint || odeGlob.contactsGroup[i].num)
            return reject(text, capacity, "server_ode_active_bodies_joints_or_contact_group");
        if (space.type != dSimpleSpaceClass || space.count || space.first || space.current_geom || space.lock_count ||
            !noLinks(space) || !nativeVtable(&space, vtables[i]))
            return reject(text, capacity, "server_ode_space_is_busy_foreign_or_outside_original_object_lifetime");
        if (!finite(world.seconds) || !finite(world.stepInfo.global_erp) || !finite(world.stepInfo.global_cfm) ||
            !finite(world.stepInfo.qs.w) || !finite(world.stepInfo.contactp.max_vel) || !finite(world.stepInfo.contactp.min_depth) ||
            !finite(world.adis.linear_threshold) || !finite(world.adis.angular_threshold) || !finite(world.adis.idle_time))
            return reject(text, capacity, "server_ode_world_solver_metadata_nonfinite");
        for (float value : world.stepInfo.gravity)
            if (!finite(value)) return reject(text, capacity, "server_ode_world_gravity_nonfinite");
    }
    const auto& geom = odeGlob.worldGeom;
    if ((geom.type != dFirstUserClass && geom.type != GEOM_CLASS_WORLD) || !noLinks(geom) ||
        !zero(geom.user_data, sizeof(geom.user_data)) || !nativeVtable(&geom, vtables[3]))
        return reject(text, capacity, "server_ode_world_geom_has_foreign_links_payload_or_object_lifetime");
    if (initialized && geom.type != GEOM_CLASS_WORLD)
        return reject(text, capacity, "server_ode_initialized_world_geom_class_missing");
    return true;
}
bool capture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    image.captured = false;
    if (!context(text, capacity) || !KHZ_ServerPhysicsFenceIdle(f, text, capacity) || !empty(text, capacity, image.vtables)) return false;
    image.state.capture(&odeGlob); image.initialized = KHZ_ServerPhysicsInitialized();
    image.frame = f; image.captured = true; return true;
}
bool validate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity) || !image.captured || !khz::server_checkpoint::sameLifetime(image.frame, f))
        return reject(text, capacity, "server_ode_original_arena_or_saved_image_missing");
    if (!KHZ_ServerPhysicsFenceIdle(f, text, capacity)) return false;
    std::uintptr_t vtables[4];
    if (!empty(text, capacity, vtables)) return false;
    if (image.initialized != KHZ_ServerPhysicsInitialized() || std::memcmp(vtables, image.vtables, sizeof(vtables)))
        return reject(text, capacity, "server_ode_initialization_or_original_vtable_identity_changed");
    return true;
}
void restore(const KHZ_ServerCheckpointFrame& f) {
    KHZ_ServerPhysicsRequireCommitFence(f);
    iassert(KHZ_ServerCheckpointContext() && image.captured);
    // Same live constructed spaces/worldGeom, same vtables, no active graph or
    // heap ownership. Restore allocator bytes and solver metadata in place.
    image.state.restore(&odeGlob);
}
void discard() { image.captured = false; }
}
void KHZ_ServerODERegisterCheckpoint() {
    using namespace khz_server_ode_detail;
    const KHZ_ServerCheckpointProvider provider{KHZ_ServerCheckpointDomain::Physics, 1, sizeof(image),
        capture, validate, restore, discard};
    if (registered || !registerPart(provider) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, &odeGlob, sizeof(odeGlob)))
        Com_Error(ERR_FATAL, "Could not register original native empty ODE arena checkpoint");
    if (!KHZ_ServerPhysicsFenceEnroll(1))
        Com_Error(ERR_FATAL, "Could not enroll actual native Physics checkpoint fence part 1");
    registered = true;
}
