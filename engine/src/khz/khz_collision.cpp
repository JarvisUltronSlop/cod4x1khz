#include <universal/q_shared.h>
#include "khz_collision.h"
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/threads.h>
#include <bgame/bg_public.h>
#include <cgame_mp/cg_local_mp.h>
#include <DynEntity/DynEntity_client.h>
#include <core.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
constexpr unsigned BrushSolid = 0xFFFFFF;
thread_local const khz::CollisionFrame* pinnedFrame;
thread_local khz::CollisionMapToken pinnedMap;

struct ClientCollisionOwner {
    khz::CollisionFrame frame;
    khz::CollisionBuildState build;
    khz::CollisionMapToken previousMap;
    std::uint64_t previousTick;
    int previousSampleTime;
    int previousSnapshotFlags;
    bool seen;
};
// Allocate once, outside active ticks. Only the current main-thread prediction
// owner may rebuild this frame; consumers pin it for that owner's Pmove scope.
ClientCollisionOwner clientCollisionOwner;

bool sameMap(const khz::CollisionMapToken& a, const khz::CollisionMapToken& b) {
    return a.session == b.session && a.generation == b.generation &&
        a.checksum == b.checksum && a.models == b.models && a.planes == b.planes;
}
bool mapLoaded(const khz::CollisionMapToken& map) {
    return map.session && map.generation && cm.isInUse && cm.numNodes &&
        map.checksum == cm.checksum && map.models == cm.cmodels && map.planes == cm.planes;
}
bool finite3(const float* value) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}
void copy3(const float* from, float* to) {
    to[0] = from[0]; to[1] = from[1]; to[2] = from[2];
}
void issue(khz::CollisionFrame& frame, khz::CollisionIssueCode code, int object = -1, int detail = 0) {
    if (frame.issueCount < khz::CollisionIssueCapacity)
        frame.issues[frame.issueCount] = {code, object, detail};
    ++frame.issueCount;
    frame.complete = false;
}
bool trajectoryUsable(khz::CollisionFrame& frame, const trajectory_t& value, int number) {
    if (value.trType == TR_PHYSICS || Com_IsRagdollTrajectory(&value)) {
        issue(frame, khz::CollisionIssueCode::UnsupportedTrajectory, number, value.trType);
        return false;
    }
    if (value.trType < TR_STATIONARY || value.trType > TR_DECELERATE ||
        ((value.trType == TR_SINE || value.trType == TR_ACCELERATE || value.trType == TR_DECELERATE) && value.trDuration <= 0) ||
        !finite3(value.trBase) || !finite3(value.trDelta)) {
        issue(frame, khz::CollisionIssueCode::InvalidTrajectory, number, value.trType);
        return false;
    }
    return true;
}
const entityState_s* priorState(const snapshot_s& current, const entityState_s& next,
    const entityState_s* const* lookup, bool reset) {
    const auto* prior = lookup[next.number];
    // CG_ResetEntity copies next.lerp after spawn/teleport. MP's teleport bit is 2.
    if (!prior || reset || ((prior->lerp.eFlags ^ next.lerp.eFlags) & 2)) return &next;
    return prior;
}
bool evaluateEntity(khz::CollisionFrame& frame, const snapshot_s& current,
    const snapshot_s& next, const entityState_s& entity,
    const entityState_s* const* previous, const entityState_s* const* following,
    khz::CollisionEntityProxy& proxy) {
    const bool reset = ((current.snapFlags ^ next.snapFlags) & 4) != 0;
    const auto* prior = priorState(current, entity, previous, reset);
    if (!trajectoryUsable(frame, prior->lerp.pos, entity.number) ||
        !trajectoryUsable(frame, prior->lerp.apos, entity.number) ||
        !trajectoryUsable(frame, entity.lerp.pos, entity.number) ||
        !trajectoryUsable(frame, entity.lerp.apos, entity.number)) return false;

    if (entity.eType == ET_PLAYER) {
        const clientState_s* metadata = nullptr;
        for (int i = 0; i < next.numClients; ++i)
            if (next.clients[i].clientIndex == entity.clientNum) metadata = &next.clients[i];
        if (!metadata) {
            issue(frame, khz::CollisionIssueCode::MissingClientMetadata, entity.number, entity.clientNum);
            return false;
        }
        if (metadata->attachedVehEntNum != ENTITYNUM_NONE) {
            // Seat transforms use animated DObj tags, absent from trajectories.
            issue(frame, khz::CollisionIssueCode::VehicleSeatTransform, entity.number, metadata->attachedVehEntNum);
            return false;
        }
    }

    const bool interpolate = (prior->lerp.pos.trType == TR_INTERPOLATE && entity.lerp.pos.trType != TR_PHYSICS) ||
        (prior->lerp.pos.trType == TR_LINEAR_STOP && entity.number < 64);
    if (interpolate) {
        const int span = next.serverTime - current.serverTime;
        float f = span ? static_cast<float>(static_cast<double>(frame.sampleTime - current.serverTime) / span) : 0.0f;
        if (f < 0) f = 0; // CG_SetFrameInterpolation does not clamp the upper end.
        float a[3], b[3];
        BG_EvaluateTrajectory(&prior->lerp.pos, current.serverTime, a);
        BG_EvaluateTrajectory(&entity.lerp.pos, next.serverTime, b);
        Vec3Lerp(a, b, f, proxy.origin);
        BG_EvaluateTrajectory(&prior->lerp.apos, current.serverTime, a);
        BG_EvaluateTrajectory(&entity.lerp.apos, next.serverTime, b);
        for (unsigned axis = 0; axis < 3; ++axis) {
            const float turns = (b[axis] - a[axis]) * 0.002777777845039964f;
            const float rounded = static_cast<float>(std::floor(turns + 0.5f));
            const float delta = (turns - rounded) * 360.0f;
            proxy.angles[axis] = a[axis] + f * delta;
        }
    } else {
        BG_EvaluateTrajectory(&prior->lerp.pos, frame.sampleTime, proxy.origin);
        BG_EvaluateTrajectory(&prior->lerp.apos, frame.sampleTime, proxy.angles);
        // Mirror CG_AdjustPositionForMover: translation only, no angular rotation
        // of the passenger origin, and no adjustment on the interpolate branch.
        const int ground = entity.groundEntityNum;
        if (ground > 0 && ground < ENTITYNUM_WORLD) {
            const auto* mover = following[ground];
            if (!mover) {
                issue(frame, khz::CollisionIssueCode::MissingMover, entity.number, ground);
                return false;
            }
            if (mover->eType == ET_SCRIPTMOVER || mover->eType == ET_PLANE) {
                const auto* oldMover = priorState(current, *mover, previous, reset);
                if (!trajectoryUsable(frame, oldMover->lerp.pos, ground)) return false;
                float oldOrigin[3], newOrigin[3];
                BG_EvaluateTrajectory(&oldMover->lerp.pos, current.serverTime, oldOrigin);
                BG_EvaluateTrajectory(&oldMover->lerp.pos, frame.sampleTime, newOrigin);
                for (unsigned axis = 0; axis < 3; ++axis)
                    proxy.origin[axis] += newOrigin[axis] - oldOrigin[axis];
            }
        }
    }
    if (entity.eType == ET_PLAYER || (entity.eType == ET_PLAYER_CORPSE && !interpolate)) {
        proxy.angles[0] = 0; proxy.angles[2] = 0;
    }
    // Packed solids are axis-aligned regardless of the rendered entity angles.
    if (proxy.solid != BrushSolid) proxy.angles[0] = proxy.angles[1] = proxy.angles[2] = 0;
    if (!finite3(proxy.origin) || !finite3(proxy.angles)) {
        issue(frame, khz::CollisionIssueCode::NonFiniteTransform, entity.number);
        return false;
    }
    return true;
}
bool validSnapshot(const snapshot_s* snapshot) {
    return snapshot && snapshot->numEntities >= 0 && snapshot->numEntities <= 512 &&
        snapshot->numClients >= 0 && snapshot->numClients <= 64;
}
bool validTraceInput(const float* start, const float* mins, const float* maxs, const float* end) {
    return start && mins && maxs && end && finite3(start) && finite3(mins) &&
        finite3(maxs) && finite3(end) && maxs[0] >= mins[0] && maxs[1] >= mins[1] && maxs[2] >= mins[2];
}

void traceFrame(const khz::CollisionFrame& frame, trace_t* result,
    const float* start, const float* mins, const float* maxs, const float* end,
    int passEntityNum, int mask, bool reverse = false) {
    CM_BoxTrace(result, start, end, mins, maxs, 0, mask);
    result->hitType = result->fraction == 1 ? TRACE_HITTYPE_NONE : TRACE_HITTYPE_ENTITY;
    result->hitId = result->fraction == 1 ? 0 : ENTITYNUM_WORLD;
    if (result->fraction == 0) return;

    const bool point = maxs[0] - mins[0] + maxs[1] - mins[1] + maxs[2] - mins[2] == 0;
    moveclip_t clip;
    clip.contentmask = mask; clip.passEntityNum = passEntityNum; clip.passOwnerNum = -1;
    for (unsigned axis = 0; axis < 3; ++axis) {
        clip.maxs[axis] = (maxs[axis] - mins[axis]) * 0.5f;
        clip.mins[axis] = -clip.maxs[axis];
        clip.outerSize[axis] = clip.maxs[axis] + 1;
        const float offset = point ? 0 : (maxs[axis] + mins[axis]) * 0.5f;
        clip.extents.start[axis] = start[axis] + offset;
        clip.extents.end[axis] = end[axis] + offset;
    }
    CM_CalcTraceExtents(&clip.extents);
    const float zero[3] = {};
    for (unsigned n = 0; n < frame.entityCount && result->fraction > 0; ++n) {
        const auto& proxy = frame.entities[reverse ? frame.entityCount - 1 - n : n];
        if (static_cast<int>(proxy.number) == passEntityNum || !(mask & proxy.contents)) continue;
        float absMins[3], absMaxs[3];
        for (unsigned axis = 0; axis < 3; ++axis) {
            absMins[axis] = proxy.absMins[axis] + (point ? 0 : clip.mins[axis]);
            absMaxs[axis] = proxy.absMaxs[axis] + (point ? 0 : clip.maxs[axis]);
        }
        if (CM_TraceBox(&clip.extents, absMins, absMaxs, result->fraction)) continue;
        const unsigned model = proxy.solid == BrushSolid ? proxy.model : CM_TempBoxModel(proxy.mins, proxy.maxs, proxy.contents);
        const auto modelAndMask = static_cast<std::int64_t>((std::uint64_t(static_cast<std::uint32_t>(mask)) << 32) | model);
        const float oldFraction = result->fraction;
        CM_TransformedBoxTrace(result, clip.extents.start, clip.extents.end,
            point ? zero : clip.mins, point ? zero : clip.maxs, modelAndMask, proxy.origin, proxy.angles);
        if (oldFraction > result->fraction) {
            result->modelIndex = result->partName = result->partGroup = 0;
            result->hitType = TRACE_HITTYPE_ENTITY; result->hitId = static_cast<std::uint16_t>(proxy.number);
            // CG's nonlocational point branch clears material and assigns model
            // contents; its capsule branch retains the actual brush material.
            if (point) { result->contents = proxy.contents; result->material = nullptr; }
        }
    }
    if (point) return; // CG_TraceCapsule does not test DynEnt brushes for points.
    for (unsigned n = 0; n < frame.dynamicBrushCount && result->fraction > 0; ++n) {
        const auto& proxy = frame.dynamicBrushes[reverse ? frame.dynamicBrushCount - 1 - n : n];
        if (!(mask & proxy.contents) || CM_TraceSphere(&clip.extents, proxy.origin, proxy.radius, result->fraction)) continue;
        float axis[3][3];
        std::memcpy(axis, proxy.axis, sizeof(axis)); // CM API lacks const qualifier.
        const float oldFraction = result->fraction;
        CM_TransformedBoxTraceRotated(result, clip.extents.start, clip.extents.end,
            clip.mins, clip.maxs, proxy.model, mask, proxy.origin, axis);
        if (oldFraction > result->fraction) {
            result->hitType = TRACE_HITTYPE_DYNENT_BRUSH;
            result->hitId = static_cast<std::uint16_t>(proxy.id);
        }
    }
}
} // namespace

namespace khz {
CollisionMapToken captureCollisionMapToken(std::uint64_t session, std::uint64_t generation) {
    return {session, generation, cm.checksum, cm.cmodels, cm.planes};
}
bool collisionFrameMatchesMap(const CollisionFrame& frame, const CollisionMapToken& active) {
    return frame.complete && sameMap(frame.map, active) && mapLoaded(active);
}
const char* collisionIssueName(CollisionIssueCode code) {
    switch (code) {
    case CollisionIssueCode::NoMap: return "no_map";
    case CollisionIssueCode::InvalidIdentity: return "invalid_map_identity";
    case CollisionIssueCode::MissingSnapshot: return "missing_snapshot";
    case CollisionIssueCode::InvalidSnapshot: return "invalid_snapshot";
    case CollisionIssueCode::DuplicateEntity: return "duplicate_entity";
    case CollisionIssueCode::InvalidEntity: return "invalid_entity";
    case CollisionIssueCode::UnsupportedTrajectory: return "physics_or_ragdoll_trajectory_requires_owner_sample";
    case CollisionIssueCode::InvalidTrajectory: return "invalid_trajectory";
    case CollisionIssueCode::MissingClientMetadata: return "missing_client_metadata";
    case CollisionIssueCode::VehicleSeatTransform: return "vehicle_seat_requires_owner_transform";
    case CollisionIssueCode::MissingMover: return "missing_mover";
    case CollisionIssueCode::InvalidBrushModel: return "invalid_brush_model";
    case CollisionIssueCode::InvalidBounds: return "invalid_packed_or_brush_bounds";
    case CollisionIssueCode::EntityCapacity: return "entity_capacity";
    case CollisionIssueCode::MissingDynamicSamples: return "dynamic_brush_owner_samples_required";
    case CollisionIssueCode::InvalidDynamicSamples: return "invalid_dynamic_brush_sample";
    case CollisionIssueCode::DynamicCapacity: return "dynamic_brush_capacity";
    case CollisionIssueCode::NonFiniteTransform: return "nonfinite_transform";
    }
    return "unknown";
}

bool buildCollisionFrame(CollisionFrame& frame, CollisionBuildState& state,
    const CollisionMapToken& map, const snapshot_s* current, const snapshot_s* next,
    int sampleTime, std::uint64_t tick, const DynamicBrushSamples* dynamic) {
    frame.map = map; frame.gameplayTick = tick; frame.sampleTime = sampleTime;
    frame.currentSnapshotTime = current ? current->serverTime : 0;
    frame.nextSnapshotTime = next ? next->serverTime : 0;
    frame.entityCount = frame.dynamicBrushCount = frame.issueCount = 0;
    frame.dynamicSource = DynamicBrushSource::None; frame.complete = true;
    if (!cm.isInUse || !cm.numNodes) { issue(frame, CollisionIssueCode::NoMap); return false; }
    if (!mapLoaded(map)) { issue(frame, CollisionIssueCode::InvalidIdentity); return false; }
    if (!current || !next) { issue(frame, CollisionIssueCode::MissingSnapshot); return false; }
    if (!validSnapshot(current) || !validSnapshot(next)) { issue(frame, CollisionIssueCode::InvalidSnapshot); return false; }
    if (!sameMap(state.map, map)) {
        // Small owner-local bookkeeping reset; do not memset/copy the full frame.
        std::memset(&state, 0, sizeof(state)); state.map = map;
    }
    ++state.serial;
    const entityState_s* previous[CollisionEntityCapacity] = {};
    const entityState_s* following[CollisionEntityCapacity] = {};
    for (int which = 0; which < 2; ++which) {
        const auto& snapshot = which ? *next : *current;
        auto** lookup = which ? following : previous;
        for (int n = 0; n < snapshot.numEntities; ++n) {
            const auto& entity = snapshot.entities[n];
            if (entity.number < 0 || entity.number >= ENTITYNUM_WORLD) {
                issue(frame, CollisionIssueCode::InvalidEntity, entity.number); continue;
            }
            if (lookup[entity.number]) issue(frame, CollisionIssueCode::DuplicateEntity, entity.number, which);
            lookup[entity.number] = &entity;
        }
    }
    for (int n = 0; n < next->numEntities; ++n) {
        const auto& entity = next->entities[n];
        if (entity.number < 0 || entity.number >= ENTITYNUM_WORLD) continue;
        const unsigned id = static_cast<unsigned>(entity.number);
        if (!state.entityGeneration[id] || state.lastSeen[id] + 1 != state.serial || state.entityType[id] != entity.eType ||
            ((state.entityFlags[id] ^ entity.lerp.eFlags) & 2)) ++state.entityGeneration[id];
        state.lastSeen[id] = state.serial; state.entityType[id] = entity.eType; state.entityFlags[id] = entity.lerp.eFlags;
        if (!entity.solid || (static_cast<unsigned>(entity.solid) == BrushSolid && (entity.lerp.eFlags & 1))) continue;
        if (entity.eType >= ET_EVENTS || entity.eType < ET_GENERAL) {
            issue(frame, CollisionIssueCode::InvalidEntity, entity.number, entity.eType); continue;
        }
        if (frame.entityCount == CollisionEntityCapacity) { issue(frame, CollisionIssueCode::EntityCapacity); break; }
        auto& proxy = frame.entities[frame.entityCount];
        proxy.generation = state.entityGeneration[id]; proxy.number = id;
        proxy.solid = static_cast<unsigned>(entity.solid); proxy.entityType = entity.eType;
        proxy.model = proxy.solid == BrushSolid ? static_cast<unsigned>(entity.index.brushmodel) : 0;
        if (proxy.solid == BrushSolid) {
            if (!proxy.model || proxy.model >= cm.numSubModels) {
                issue(frame, CollisionIssueCode::InvalidBrushModel, entity.number, entity.index.brushmodel); continue;
            }
            proxy.contents = CM_ContentsOfModel(proxy.model);
            CM_ModelBounds(proxy.model, proxy.mins, proxy.maxs);
        } else {
            proxy.contents = entity.eType == ET_PLAYER ? CONTENTS_PLAYER : CONTENTS_SOLID;
            const float x = static_cast<float>(proxy.solid & 255);
            proxy.mins[0] = proxy.mins[1] = 1 - x;
            proxy.maxs[0] = proxy.maxs[1] = x - 1;
            proxy.mins[2] = 2 - static_cast<float>((proxy.solid >> 8) & 255);
            proxy.maxs[2] = static_cast<float>((proxy.solid >> 16) & 255) - 33;
        }
        if (!finite3(proxy.mins) || !finite3(proxy.maxs) || proxy.mins[0] > proxy.maxs[0] ||
            proxy.mins[1] > proxy.maxs[1] || proxy.mins[2] > proxy.maxs[2]) {
            issue(frame, CollisionIssueCode::InvalidBounds, entity.number, entity.solid); continue;
        }
        if (!evaluateEntity(frame, *current, *next, entity, previous, following, proxy)) continue;
        const float radius = RadiusFromBounds(proxy.mins, proxy.maxs);
        for (unsigned axis = 0; axis < 3; ++axis) {
            proxy.absMins[axis] = proxy.origin[axis] - radius;
            proxy.absMaxs[axis] = proxy.origin[axis] + radius;
        }
        ++frame.entityCount;
    }

    const unsigned expected = cm.dynEntCount[DYNENT_COLL_CLIENT_BRUSH];
    if (!expected) return frame.complete;
    if (expected > CollisionDynamicBrushCapacity) {
        issue(frame, CollisionIssueCode::DynamicCapacity, -1, expected); return false;
    }
    if (!dynamic || !dynamic->values || dynamic->source == DynamicBrushSource::None) {
        issue(frame, CollisionIssueCode::MissingDynamicSamples, -1, expected); return false;
    }
    frame.dynamicSource = dynamic->source;
    if ((dynamic->source != DynamicBrushSource::FixedGameplayOwner && dynamic->source != DynamicBrushSource::DiagnosticMainThreadPose) ||
        dynamic->count != expected || dynamic->sampleTime != sampleTime || !cm.dynEntDefList[DYNENT_DRAW_BRUSH]) {
        issue(frame, CollisionIssueCode::InvalidDynamicSamples, -1, dynamic->count); return false;
    }
    bool seen[CollisionDynamicBrushCapacity] = {};
    for (unsigned n = 0; n < dynamic->count; ++n) {
        const auto& sample = dynamic->values[n];
        if (sample.id >= expected || seen[sample.id]) {
            issue(frame, CollisionIssueCode::InvalidDynamicSamples, sample.id, 1); continue;
        }
        seen[sample.id] = true;
        const auto& definition = cm.dynEntDefList[DYNENT_DRAW_BRUSH][sample.id];
        if (definition.type <= DYNENT_TYPE_INVALID || definition.type >= DYNENT_TYPE_COUNT ||
            sample.model != definition.brushModel || sample.contents != definition.contents ||
            sample.clipMove != DynEnt_GetEntityProps(definition.type)->clipMove ||
            (sample.linked && !sample.visible)) {
            issue(frame, CollisionIssueCode::InvalidDynamicSamples, sample.id, 2); continue;
        }
        if (!sample.linked || !sample.visible || !sample.clipMove) continue;
        if (!sample.generation || !sample.model || sample.model >= cm.numSubModels || !finite3(sample.origin) ||
            !finite3(sample.quat) || !std::isfinite(sample.quat[3]) || !std::isfinite(sample.radius) || sample.radius < 0) {
            issue(frame, CollisionIssueCode::InvalidDynamicSamples, sample.id, 3); continue;
        }
        if (!Vec4IsNormalized(sample.quat)) {
            issue(frame, CollisionIssueCode::InvalidDynamicSamples, sample.id, 4); continue;
        }
        auto& proxy = frame.dynamicBrushes[frame.dynamicBrushCount++];
        proxy.generation = sample.generation; proxy.id = sample.id; proxy.model = sample.model;
        proxy.contents = sample.contents; proxy.radius = sample.radius;
        copy3(sample.origin, proxy.origin); UnitQuatToAxis(sample.quat, proxy.axis);
    }
    return frame.complete;
}

bool traceCollisionFrame(const CollisionFrame& frame, const CollisionMapToken& active,
    trace_t* result, const float* start, const float* mins, const float* maxs,
    const float* end, int passEntityNum, int mask) {
    if (!result || !collisionFrameMatchesMap(frame, active) || !validTraceInput(start, mins, maxs, end)) return false;
    traceFrame(frame, result, start, mins, maxs, end, passEntityNum, mask);
    return true;
}
} // namespace khz

bool KHZ_BindPredictionCollisionFrame(const khz::CollisionFrame* frame, const khz::CollisionMapToken& active) {
    pinnedFrame = nullptr;
    if (!frame || !khz::collisionFrameMatchesMap(*frame, active) ||
        frame->dynamicSource == khz::DynamicBrushSource::DiagnosticMainThreadPose) return false;
    pinnedMap = active; pinnedFrame = frame;
    return true;
}
void KHZ_UnbindPredictionCollisionFrame() { pinnedFrame = nullptr; }
bool KHZ_HasPredictionCollisionFrame() {
    return pinnedFrame && khz::collisionFrameMatchesMap(*pinnedFrame, pinnedMap);
}
bool KHZ_BeginClientCollisionFrame(std::uint64_t session, std::uint64_t tick,
    int sampleTime, const snapshot_s* current, const snapshot_s* next) {
    iassert(Sys_IsMainThread());
    // Never leave the last successful world bound after an unsupported tick.
    KHZ_UnbindPredictionCollisionFrame();
    auto& owner = clientCollisionOwner;
    auto map = khz::captureCollisionMapToken(session, owner.seen ? owner.previousMap.generation : 1);
    const int flags = next ? next->snapFlags : current ? current->snapFlags : owner.previousSnapshotFlags;
    const bool changed = owner.seen &&
        (map.session != owner.previousMap.session || map.checksum != owner.previousMap.checksum ||
            map.models != owner.previousMap.models || map.planes != owner.previousMap.planes ||
            tick < owner.previousTick || sampleTime < owner.previousSampleTime ||
            ((flags ^ owner.previousSnapshotFlags) & 4));
    if (changed) {
        ++map.generation;
        if (!map.generation) map.generation = 1;
    }
    owner.previousMap = map; owner.previousTick = tick;
    owner.previousSampleTime = sampleTime; owner.previousSnapshotFlags = flags; owner.seen = true;
    // Snapshot trajectories do not contain dynamic brush physics. Deliberately
    // pass no owner samples; the builder records rejection instead of omitting
    // those brushes or taking presentation poses as authoritative collision.
    if (!khz::buildCollisionFrame(owner.frame, owner.build, map, current, next, sampleTime, tick, nullptr))
        return false;
    return KHZ_BindPredictionCollisionFrame(&owner.frame, map);
}
void __cdecl KHZ_PredictionTrace(trace_t* result, const float* start, const float* mins,
    const float* maxs, const float* end, int passEntityNum, int mask) {
    if (!pinnedFrame || !khz::traceCollisionFrame(*pinnedFrame, pinnedMap, result, start, mins, maxs, end, passEntityNum, mask))
        Com_Error(ERR_DROP, "KHZ prediction collision frame is unbound, incomplete, or stale");
}

namespace {
constexpr unsigned SweepCapacity = 96;
constexpr unsigned DetailCapacity = 16;
enum CollisionGuard : unsigned {
    GuardUnbound = 1,
    GuardIncomplete = 2,
    GuardStaleGeneration = 4,
    GuardStaleSession = 8,
    GuardStaleCMIdentity = 16,
    GuardDiagnosticPose = 32,
    GuardInvalidTraceInput = 64,
    GuardValidBindTraceUnbind = 128,
    GuardRestoredBinding = 256
};
constexpr unsigned RequiredGuards = 511;
struct ShadowSweep {
    float start[3], end[3], mins[3], maxs[3];
    int mask, pass;
};
struct ShadowFailure {
    unsigned sweep, difference, orderingDifference;
    trace_t legacy, proxy, reverse;
};
struct ShadowPose {
    unsigned entity;
    float legacyOrigin[3], proxyOrigin[3], legacyAngles[3], proxyAngles[3];
};
struct ShadowCoverage { unsigned entity; bool linked, present; };
struct ShadowProbe {
    khz::CollisionFrame frame;
    khz::CollisionBuildState build;
    khz::DynamicBrushSample dynamic[khz::CollisionDynamicBrushCapacity];
    ShadowSweep sweeps[SweepCapacity];
    ShadowFailure failures[DetailCapacity];
    ShadowPose poses[DetailCapacity];
    ShadowCoverage coverage[DetailCapacity];
    unsigned sweepCount, traceMismatches, orderingAmbiguities, poseMismatches, coverageMismatches, failureDetails;
    unsigned guardsPassed, guardsFailed, guardsSkipped;
    std::uint64_t elapsedUs;
    bool guardsRun;
    bool passed;
    const char* error;
};
// These records are preallocated; neither build nor trace allocates. Probe output
// and the small sweep loop are explicit diagnostics, outside steady gameplay.
ShadowProbe shadow;
char probeJSON[2048] = "{\"completed\":false,\"passed\":false,\"error\":\"not_run\"}";
cmd_function_s shadowCommand;
bool diagnosticRegistered;
khz::CollisionMapToken diagnosticMap;

const char* dynamicSourceName(khz::DynamicBrushSource source) {
    if (source == khz::DynamicBrushSource::DiagnosticMainThreadPose) return "diagnostic_main_thread_pose";
    if (source == khz::DynamicBrushSource::FixedGameplayOwner) return "fixed_gameplay_owner";
    return "none";
}
bool different(float a, float b, float epsilon) {
    return !std::isfinite(a) || !std::isfinite(b) || std::fabs(a - b) > epsilon;
}
unsigned traceDifference(const trace_t& a, const trace_t& b) {
    unsigned bits = 0;
    if (different(a.fraction, b.fraction, 0.00001f)) bits |= 1;
    for (unsigned axis = 0; axis < 3; ++axis)
        if (different(a.normal[axis], b.normal[axis], 0.0001f)) bits |= 2;
    if (a.surfaceFlags != b.surfaceFlags) bits |= 4;
    if (a.contents != b.contents) bits |= 8;
    if (a.material != b.material) bits |= 16;
    if (a.hitType != b.hitType) bits |= 32;
    if (a.hitId != b.hitId) bits |= 64;
    if (a.modelIndex != b.modelIndex || a.partName != b.partName || a.partGroup != b.partGroup) bits |= 128;
    if (a.allsolid != b.allsolid || a.startsolid != b.startsolid) bits |= 256;
    if (a.walkable != b.walkable) bits |= 512;
    return bits;
}
void runCollisionGuards() {
    auto& frame = shadow.frame;
    // Do not manufacture a supposedly authoritative frame from incomplete world
    // data or relabel presentation-derived DynEnt poses. The positive bind test
    // requires the actual complete production-bindable accepted snapshot frame.
    if (!frame.complete || frame.dynamicSource == khz::DynamicBrushSource::DiagnosticMainThreadPose || !shadow.sweepCount)
        return;
    shadow.guardsRun = true; shadow.guardsSkipped = 0;
    const auto* savedBinding = pinnedFrame;
    const auto savedBindingMap = pinnedMap;
    const bool savedBindingValid = KHZ_HasPredictionCollisionFrame();
    const auto map = frame.map;
    const auto source = frame.dynamicSource;
    const auto& sweep = shadow.sweeps[0];
    auto record = [](unsigned bit, bool passed) {
        if (passed) shadow.guardsPassed |= bit;
        else shadow.guardsFailed |= bit;
    };
    trace_t seed{};
    seed.fraction = 0.375f; seed.normal[0] = 0.125f; seed.normal[1] = -0.25f; seed.normal[2] = 0.5f;
    seed.surfaceFlags = 123; seed.contents = 456; seed.material = "khz_guard_sentinel";
    seed.hitType = TRACE_HITTYPE_ENTITY; seed.hitId = 17; seed.modelIndex = 19;
    seed.partName = 23; seed.partGroup = 29; seed.allsolid = seed.walkable = true;
    auto rejectsUnchanged = [&](const khz::CollisionMapToken& active, const float* start,
        const float* mins, const float* maxs, const float* end) {
        trace_t output;
        std::memcpy(&output, &seed, sizeof(seed));
        const bool rejected = !khz::traceCollisionFrame(frame, active, &output,
            start, mins, maxs, end, sweep.pass, sweep.mask);
        return rejected && std::memcmp(&output, &seed, sizeof(seed)) == 0;
    };

    KHZ_UnbindPredictionCollisionFrame();
    record(GuardUnbound, !KHZ_HasPredictionCollisionFrame() &&
        !KHZ_BindPredictionCollisionFrame(nullptr, map) && !KHZ_HasPredictionCollisionFrame());

    frame.complete = false;
    record(GuardIncomplete, !KHZ_BindPredictionCollisionFrame(&frame, map) && !KHZ_HasPredictionCollisionFrame() &&
        rejectsUnchanged(map, sweep.start, sweep.mins, sweep.maxs, sweep.end));
    frame.complete = true;

    auto stale = map; ++stale.generation;
    record(GuardStaleGeneration, !KHZ_BindPredictionCollisionFrame(&frame, stale) && !KHZ_HasPredictionCollisionFrame() &&
        rejectsUnchanged(stale, sweep.start, sweep.mins, sweep.maxs, sweep.end));
    stale = map; ++stale.session;
    record(GuardStaleSession, !KHZ_BindPredictionCollisionFrame(&frame, stale) && !KHZ_HasPredictionCollisionFrame() &&
        rejectsUnchanged(stale, sweep.start, sweep.mins, sweep.maxs, sweep.end));

    // Matching frame/token metadata must still reject a token inconsistent with
    // the currently loaded BSP. Null model identity is never dereferenced here.
    stale = map; stale.models = nullptr; frame.map = stale;
    record(GuardStaleCMIdentity, !KHZ_BindPredictionCollisionFrame(&frame, stale) && !KHZ_HasPredictionCollisionFrame() &&
        rejectsUnchanged(stale, sweep.start, sweep.mins, sweep.maxs, sweep.end));
    frame.map = map;

    frame.dynamicSource = khz::DynamicBrushSource::DiagnosticMainThreadPose;
    record(GuardDiagnosticPose, !KHZ_BindPredictionCollisionFrame(&frame, map) && !KHZ_HasPredictionCollisionFrame());
    frame.dynamicSource = source;

    float invertedMaxs[3]; copy3(sweep.maxs, invertedMaxs); invertedMaxs[0] = sweep.mins[0] - 1;
    float nonfinite[3]; copy3(sweep.start, nonfinite); nonfinite[1] = std::numeric_limits<float>::quiet_NaN();
    record(GuardInvalidTraceInput,
        rejectsUnchanged(map, nullptr, sweep.mins, sweep.maxs, sweep.end) &&
        rejectsUnchanged(map, sweep.start, nullptr, sweep.maxs, sweep.end) &&
        rejectsUnchanged(map, sweep.start, sweep.mins, nullptr, sweep.end) &&
        rejectsUnchanged(map, sweep.start, sweep.mins, sweep.maxs, nullptr) &&
        rejectsUnchanged(map, sweep.start, sweep.mins, invertedMaxs, sweep.end) &&
        rejectsUnchanged(map, nonfinite, sweep.mins, sweep.maxs, sweep.end) &&
        rejectsUnchanged(map, sweep.start, nonfinite, sweep.maxs, sweep.end) &&
        rejectsUnchanged(map, sweep.start, sweep.mins, nonfinite, sweep.end) &&
        rejectsUnchanged(map, sweep.start, sweep.mins, sweep.maxs, nonfinite) &&
        !khz::traceCollisionFrame(frame, map, nullptr, sweep.start, sweep.mins, sweep.maxs, sweep.end, sweep.pass, sweep.mask));

    const bool bound = KHZ_BindPredictionCollisionFrame(&frame, map) && KHZ_HasPredictionCollisionFrame();
    bool sameTrace = false;
    if (bound) {
        trace_t direct, callback;
        if (khz::traceCollisionFrame(frame, map, &direct, sweep.start, sweep.mins, sweep.maxs, sweep.end, sweep.pass, sweep.mask)) {
            // This is a verified valid callback invocation against real loaded CM
            // geometry. Never invoke the ERR_DROP callback on invalid test data.
            KHZ_PredictionTrace(&callback, sweep.start, sweep.mins, sweep.maxs, sweep.end, sweep.pass, sweep.mask);
            sameTrace = traceDifference(direct, callback) == 0;
        }
    }
    KHZ_UnbindPredictionCollisionFrame();
    record(GuardValidBindTraceUnbind, bound && sameTrace && !KHZ_HasPredictionCollisionFrame());

    // Restore exactly the caller's TLS scope, including its prior validity. No
    // clientCollisionOwner frame or builder bookkeeping was touched by this test.
    pinnedMap = savedBindingMap; pinnedFrame = savedBinding;
    record(GuardRestoredBinding, pinnedFrame == savedBinding && sameMap(pinnedMap, savedBindingMap) &&
        KHZ_HasPredictionCollisionFrame() == savedBindingValid);
}
void addSweep(const float* center, const float* offset, float height, int mask, int pass) {
    if (shadow.sweepCount == SweepCapacity) return;
    auto& sweep = shadow.sweeps[shadow.sweepCount++];
    copy3(center, sweep.start);
    for (unsigned axis = 0; axis < 3; ++axis) {
        sweep.end[axis] = center[axis] + offset[axis];
        sweep.mins[axis] = 0; sweep.maxs[axis] = 0;
    }
    // PM_CheckDuck stock standing/crouch/prone sizes. Height zero is a point.
    if (height) {
        sweep.mins[0] = sweep.mins[1] = -15;
        sweep.maxs[0] = sweep.maxs[1] = 15; sweep.maxs[2] = height;
    }
    sweep.mask = mask; sweep.pass = pass;
}
void buildSweeps(const playerState_s& player) {
    constexpr float offsets[7][3] = {
        {128, 0, 0}, {-128, 0, 0}, {0, 128, 0}, {0, -128, 0},
        {0, 0, 72}, {0, 0, -96}, {0, 0, 0}
    };
    constexpr float heights[4] = {70, 50, 30, 0};
    for (float height : heights)
        for (const auto& offset : offsets) addSweep(player.origin, offset, height, MASK_PLAYERSOLID, player.clientNum);
    for (unsigned n = 0; n < 6; ++n) {
        addSweep(player.origin, offsets[n], 30, MASK_DEADSOLID, player.clientNum);
        addSweep(player.origin, offsets[n], 70, CONTENTS_MANTLE, player.clientNum);
        addSweep(player.origin, offsets[n], 0, CONTENTS_SOLID, player.clientNum);
    }
    // Probe nearby accepted solids from three axes, so player bodies and movers
    // get coverage even when the local player does not currently touch them.
    unsigned targeted = 0;
    for (unsigned n = 0; n < shadow.frame.entityCount && targeted < 16; ++n) {
        const auto& entity = shadow.frame.entities[n];
        if (static_cast<int>(entity.number) == player.clientNum) continue;
        float separation[3]; Vec3Sub(entity.origin, player.origin, separation);
        if (Vec3LengthSq(separation) > 512.0f * 512.0f) continue;
        ++targeted;
        for (unsigned axis = 0; axis < 3; ++axis) {
            float start[3]; copy3(entity.origin, start);
            const float distance = entity.absMaxs[axis] - entity.origin[axis] + 64;
            start[axis] -= distance;
            if (entity.solid != BrushSolid) start[2] += (entity.mins[2] + entity.maxs[2]) * 0.5f;
            float offset[3] = {}; offset[axis] = distance * 2;
            addSweep(start, offset, axis == 2 ? 30 : 0, MASK_PLAYERSOLID | CONTENTS_MANTLE, player.clientNum);
        }
    }
}
void compareCoverageAndPoses(int localClientNum, int player) {
    const khz::CollisionEntityProxy* proxies[khz::CollisionEntityCapacity] = {};
    for (unsigned n = 0; n < shadow.frame.entityCount; ++n) {
        const auto& proxy = shadow.frame.entities[n]; proxies[proxy.number] = &proxy;
    }
    for (unsigned n = 0; n < ENTITYNUM_WORLD; ++n) {
        if (static_cast<int>(n) == player) continue;
        const auto* entity = CG_GetEntity(localClientNum, n);
        const bool hasSolid = entity->nextValid && entity->nextState.solid &&
            (static_cast<unsigned>(entity->nextState.solid) != BrushSolid || !(entity->nextState.lerp.eFlags & 1));
        const bool linked = hasSolid && CG_IsEntityLinked(localClientNum, n);
        const auto* proxy = proxies[n];
        if (linked != (proxy != nullptr)) {
            if (shadow.coverageMismatches < DetailCapacity)
                shadow.coverage[shadow.coverageMismatches] = {n, linked, proxy != nullptr};
            ++shadow.coverageMismatches;
        }
        if (!linked || !proxy) continue;
        bool mismatch = false;
        for (unsigned axis = 0; axis < 3; ++axis) {
            if (different(entity->pose.origin[axis], proxy->origin[axis], 0.001f)) mismatch = true;
            if (proxy->solid == BrushSolid && different(entity->pose.angles[axis], proxy->angles[axis], 0.001f)) mismatch = true;
        }
        if (!mismatch) continue;
        if (shadow.poseMismatches < DetailCapacity) {
            auto& detail = shadow.poses[shadow.poseMismatches]; detail.entity = n;
            copy3(entity->pose.origin, detail.legacyOrigin); copy3(proxy->origin, detail.proxyOrigin);
            copy3(entity->pose.angles, detail.legacyAngles); copy3(proxy->angles, detail.proxyAngles);
        }
        ++shadow.poseMismatches;
    }
}
void jsonString(std::FILE* file, const char* value) {
    if (!value) { std::fputs("null", file); return; }
    std::fputc('"', file);
    for (unsigned n = 0; value[n] && n < 256; ++n) {
        const unsigned char character = static_cast<unsigned char>(value[n]);
        if (character == '"' || character == '\\') std::fputc('\\', file);
        if (character < 32) std::fprintf(file, "\\u%04x", character);
        else std::fputc(character, file);
    }
    std::fputc('"', file);
}
void jsonVector(std::FILE* file, const float* value) {
    std::fputc('[', file);
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (axis) std::fputc(',', file);
        if (std::isfinite(value[axis])) std::fprintf(file, "%.9g", value[axis]);
        else std::fputs("null", file);
    }
    std::fputc(']', file);
}
void jsonTrace(std::FILE* file, const trace_t& trace) {
    std::fputs("{\"fraction\":", file);
    if (std::isfinite(trace.fraction)) std::fprintf(file, "%.9g", trace.fraction);
    else std::fputs("null", file);
    std::fputs(",\"normal\":", file); jsonVector(file, trace.normal);
    std::fprintf(file, ",\"surface_flags\":%d,\"contents\":%d,\"material\":", trace.surfaceFlags, trace.contents);
    jsonString(file, trace.material);
    std::fprintf(file, ",\"hit_type\":%d,\"hit_id\":%u,\"model_index\":%u,\"part_name\":%u,\"part_group\":%u,\"allsolid\":%s,\"startsolid\":%s,\"walkable\":%s}",
        trace.hitType, trace.hitId, trace.modelIndex, trace.partName, trace.partGroup,
        trace.allsolid ? "true" : "false", trace.startsolid ? "true" : "false", trace.walkable ? "true" : "false");
}
void formatSummary(bool reportWritten) {
    const auto& frame = shadow.frame;
    std::snprintf(probeJSON, sizeof(probeJSON),
        "{\"completed\":true,\"passed\":%s,\"frame_complete\":%s,\"map_checksum\":%u,\"sample_time\":%d,\"current_snapshot_time\":%d,\"next_snapshot_time\":%d,\"entities\":%u,\"dynamic_brushes\":%u,\"dynamic_source\":\"%s\",\"build_issues\":%u,\"sweeps\":%u,\"trace_mismatches\":%u,\"ordering_ambiguities\":%u,\"pose_mismatches\":%u,\"coverage_mismatches\":%u,\"guards_run\":%s,\"guards_pass\":%s,\"guards_required_mask\":%u,\"guards_passed_mask\":%u,\"guards_failed_mask\":%u,\"guards_skipped_mask\":%u,\"elapsed_us\":%llu,\"report_written\":%s,\"report\":\"khz_collision_shadow.json\",\"error\":\"%s\",\"scope\":\"bounded CG capsule shadow sweeps and guards at fixed gameplay time; diagnostic DynEnt poses are not worker ownership; production handler unchanged\"}",
        shadow.passed ? "true" : "false", frame.complete ? "true" : "false", frame.map.checksum,
        frame.sampleTime, frame.currentSnapshotTime, frame.nextSnapshotTime, frame.entityCount, frame.dynamicBrushCount,
        dynamicSourceName(frame.dynamicSource), frame.issueCount, shadow.sweepCount, shadow.traceMismatches,
        shadow.orderingAmbiguities, shadow.poseMismatches, shadow.coverageMismatches,
        shadow.guardsRun ? "true" : "false", shadow.guardsRun && shadow.guardsPassed == RequiredGuards && !shadow.guardsFailed && !shadow.guardsSkipped ? "true" : "false",
        RequiredGuards, shadow.guardsPassed, shadow.guardsFailed, shadow.guardsSkipped, shadow.elapsedUs,
        reportWritten ? "true" : "false", shadow.error ? shadow.error : "");
}
bool writeReport() {
    auto* file = std::fopen("khz_collision_shadow.json", "wb");
    if (!file) return false;
    // Embed precisely the same status fields as the endpoint summary. Change the
    // report_written bit only after fclose confirms this complete report exists.
    const auto length = std::strlen(probeJSON);
    std::fwrite(probeJSON, 1, length - 1, file);
    std::fputs(",\"difference_bits\":{\"fraction\":1,\"normal\":2,\"surface_flags\":4,\"contents\":8,\"material_pointer\":16,\"hit_type\":32,\"hit_id\":64,\"part_metadata\":128,\"solid_flags\":256,\"walkable\":512},\"guard_bits\":{\"unbound\":1,\"incomplete\":2,\"stale_generation\":4,\"stale_session\":8,\"stale_cm_identity\":16,\"diagnostic_pose\":32,\"invalid_trace_input\":64,\"valid_bind_trace_unbind\":128,\"restored_binding\":256},\"issues\":[", file);
    for (unsigned n = 0; n < shadow.frame.issueCount && n < khz::CollisionIssueCapacity; ++n) {
        const auto& detail = shadow.frame.issues[n];
        if (n) std::fputc(',', file);
        std::fputs("{\"code\":", file); jsonString(file, khz::collisionIssueName(detail.code));
        std::fprintf(file, ",\"object\":%d,\"detail\":%d}", detail.object, detail.detail);
    }
    std::fputs("],\"failures\":[", file);
    for (unsigned n = 0; n < shadow.failureDetails; ++n) {
        const auto& detail = shadow.failures[n]; const auto& sweep = shadow.sweeps[detail.sweep];
        if (n) std::fputc(',', file);
        std::fprintf(file, "{\"sweep\":%u,\"mask\":%d,\"pass_entity\":%d,\"difference_bits\":%u,\"ordering_difference_bits\":%u,\"start\":", detail.sweep, sweep.mask, sweep.pass, detail.difference, detail.orderingDifference);
        jsonVector(file, sweep.start); std::fputs(",\"end\":", file); jsonVector(file, sweep.end);
        std::fputs(",\"mins\":", file); jsonVector(file, sweep.mins); std::fputs(",\"maxs\":", file); jsonVector(file, sweep.maxs);
        std::fputs(",\"legacy\":", file); jsonTrace(file, detail.legacy);
        std::fputs(",\"proxy\":", file); jsonTrace(file, detail.proxy);
        std::fputs(",\"reverse\":", file); jsonTrace(file, detail.reverse); std::fputc('}', file);
    }
    std::fputs("],\"poses\":[", file);
    for (unsigned n = 0; n < shadow.poseMismatches && n < DetailCapacity; ++n) {
        const auto& detail = shadow.poses[n]; if (n) std::fputc(',', file);
        std::fprintf(file, "{\"entity\":%u,\"legacy_origin\":", detail.entity); jsonVector(file, detail.legacyOrigin);
        std::fputs(",\"proxy_origin\":", file); jsonVector(file, detail.proxyOrigin);
        std::fputs(",\"legacy_angles\":", file); jsonVector(file, detail.legacyAngles);
        std::fputs(",\"proxy_angles\":", file); jsonVector(file, detail.proxyAngles); std::fputc('}', file);
    }
    std::fputs("],\"coverage\":[", file);
    for (unsigned n = 0; n < shadow.coverageMismatches && n < DetailCapacity; ++n) {
        const auto& detail = shadow.coverage[n]; if (n) std::fputc(',', file);
        std::fprintf(file, "{\"entity\":%u,\"legacy_linked\":%s,\"proxy_present\":%s}", detail.entity, detail.linked ? "true" : "false", detail.present ? "true" : "false");
    }
    std::fprintf(file, "],\"details_truncated\":%s}",
        shadow.frame.issueCount > khz::CollisionIssueCapacity || shadow.traceMismatches + shadow.orderingAmbiguities > DetailCapacity ||
        shadow.poseMismatches > DetailCapacity || shadow.coverageMismatches > DetailCapacity ? "true" : "false");
    const bool good = std::ferror(file) == 0;
    return std::fclose(file) == 0 && good;
}
} // namespace

const char* KHZ_CollisionProbeJSON() { return probeJSON; }

void KHZ_RunCollisionShadow() {
    iassert(Sys_IsMainThread());
    const auto started = khz::counter();
    shadow.sweepCount = shadow.traceMismatches = shadow.orderingAmbiguities = 0;
    shadow.poseMismatches = shadow.coverageMismatches = shadow.failureDetails = 0;
    shadow.guardsRun = false; shadow.guardsPassed = shadow.guardsFailed = 0;
    shadow.guardsSkipped = RequiredGuards;
    shadow.error = nullptr; shadow.passed = false;
    auto& frame = shadow.frame;
    frame.complete = false; frame.issueCount = frame.entityCount = frame.dynamicBrushCount = 0;
    frame.map = {}; frame.sampleTime = frame.currentSnapshotTime = frame.nextSnapshotTime = 0;
    frame.dynamicSource = khz::DynamicBrushSource::None;
    if (CL_GetLocalClientConnectionState(0) != CA_ACTIVE || !cm.isInUse) {
        shadow.error = "active_rendered_client_required";
    } else {
        const auto* game = CG_GetLocalClientGlobals(0);
        if (!game->snap || !game->nextSnap) shadow.error = "accepted_snapshot_pair_required";
        else {
            auto map = khz::captureCollisionMapToken(1, diagnosticMap.generation ? diagnosticMap.generation : 1);
            if (diagnosticMap.models != map.models || diagnosticMap.planes != map.planes || diagnosticMap.checksum != map.checksum)
                ++map.generation;
            diagnosticMap = map;
            const unsigned dynamicCount = cm.dynEntCount[DYNENT_COLL_CLIENT_BRUSH];
            khz::DynamicBrushSamples samples{shadow.dynamic, dynamicCount, game->time, khz::DynamicBrushSource::DiagnosticMainThreadPose};
            if (dynamicCount <= khz::CollisionDynamicBrushCapacity) {
                for (unsigned n = 0; n < dynamicCount; ++n) {
                    const auto* definition = DynEnt_GetEntityDef(static_cast<std::uint16_t>(n), DYNENT_DRAW_BRUSH);
                    const auto* pose = DynEnt_GetClientPose(static_cast<std::uint16_t>(n), DYNENT_DRAW_BRUSH);
                    const auto* client = DynEnt_GetClientEntity(static_cast<std::uint16_t>(n), DYNENT_DRAW_BRUSH);
                    const auto* coll = DynEnt_GetEntityColl(DYNENT_COLL_CLIENT_BRUSH, static_cast<std::uint16_t>(n));
                    auto& sample = shadow.dynamic[n]; sample.id = n; sample.generation = 1;
                    sample.model = definition->brushModel; sample.contents = definition->contents;
                    sample.clipMove = definition->type > DYNENT_TYPE_INVALID && definition->type < DYNENT_TYPE_COUNT &&
                        DynEnt_GetEntityProps(definition->type)->clipMove;
                    sample.visible = (client->flags & DYNENT_CL_VISIBLE) != 0; sample.linked = coll->sector != 0;
                    copy3(pose->pose.origin, sample.origin); std::memcpy(sample.quat, pose->pose.quat, sizeof(sample.quat));
                    sample.radius = pose->radius;
                }
            }
            khz::buildCollisionFrame(frame, shadow.build, map, game->snap, game->nextSnap,
                game->time, static_cast<std::uint64_t>(static_cast<std::uint32_t>(game->time)), &samples);
            compareCoverageAndPoses(0, game->predictedPlayerState.clientNum);
            if (frame.complete) {
                buildSweeps(game->predictedPlayerState);
                for (unsigned n = 0; n < shadow.sweepCount; ++n) {
                    const auto& sweep = shadow.sweeps[n]; trace_t legacy, proxy, reverse;
                    CG_TraceCapsule(&legacy, sweep.start, sweep.mins, sweep.maxs, sweep.end, sweep.pass, sweep.mask);
                    if (!khz::traceCollisionFrame(frame, map, &proxy, sweep.start, sweep.mins, sweep.maxs, sweep.end, sweep.pass, sweep.mask)) {
                        shadow.error = "frame_became_stale_during_probe"; break;
                    }
                    traceFrame(frame, &reverse, sweep.start, sweep.mins, sweep.maxs, sweep.end, sweep.pass, sweep.mask, true);
                    const unsigned difference = traceDifference(legacy, proxy);
                    const unsigned ordering = !different(proxy.fraction, reverse.fraction, 0.00001f) ? traceDifference(proxy, reverse) : 0;
                    if (difference) ++shadow.traceMismatches;
                    if (ordering) ++shadow.orderingAmbiguities;
                    if ((difference || ordering) && shadow.failureDetails < DetailCapacity)
                        shadow.failures[shadow.failureDetails++] = {n, difference, ordering, legacy, proxy, reverse};
                }
            }
            runCollisionGuards();
            shadow.passed = frame.complete && !shadow.error && shadow.sweepCount && !shadow.traceMismatches &&
                !shadow.orderingAmbiguities && !shadow.poseMismatches && !shadow.coverageMismatches &&
                shadow.guardsRun && shadow.guardsPassed == RequiredGuards && !shadow.guardsFailed && !shadow.guardsSkipped;
        }
    }
    shadow.elapsedUs = (khz::counter() - started) * 1000000 / khz::frequency();
    formatSummary(false);
    const bool reportWritten = writeReport();
    formatSummary(reportWritten);
    Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_COLLISION %s\n", probeJSON);
}
void KHZ_InitCollisionDiagnostics() {
    if (diagnosticRegistered) return;
    diagnosticRegistered = true;
    Cmd_AddCommandInternal("khz_collision_shadow", KHZ_RunCollisionShadow, &shadowCommand);
}
