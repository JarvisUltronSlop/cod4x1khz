#pragma once

#include <cstdint>

struct snapshot_s;
struct trace_t;

namespace khz {

constexpr unsigned CollisionEntityCapacity = 1024;
constexpr unsigned CollisionDynamicBrushCapacity = 1024;
constexpr unsigned CollisionIssueCapacity = 32;

// The owner advances these identities at disconnect, restart, and map/asset load.
// A token is a lifetime contract, not a lock: quiesce readers before freeing CM.
struct CollisionMapToken {
    std::uint64_t session;
    std::uint64_t generation;
    std::uint32_t checksum;
    const void* models;
    const void* planes;
};

enum class DynamicBrushSource : unsigned {
    None,
    FixedGameplayOwner,
    DiagnosticMainThreadPose
};

// Supply EVERY map brush id, including unlinked/invisible brushes. Only the
// physics owner may label a sample FixedGameplayOwner. Snapshot trajectories do
// not describe client DynEnt physics; omission must never imply an empty world.
struct DynamicBrushSample {
    unsigned id;
    std::uint64_t generation;
    unsigned model;
    int contents;
    bool clipMove;
    bool visible;
    bool linked;
    float origin[3];
    float quat[4];
    float radius;
};

struct DynamicBrushSamples {
    const DynamicBrushSample* values;
    unsigned count;
    int sampleTime;
    DynamicBrushSource source;
};

enum class CollisionIssueCode : unsigned {
    NoMap, InvalidIdentity, MissingSnapshot, InvalidSnapshot,
    DuplicateEntity, InvalidEntity, UnsupportedTrajectory, InvalidTrajectory,
    MissingClientMetadata, VehicleSeatTransform, MissingMover,
    InvalidBrushModel, InvalidBounds, EntityCapacity,
    MissingDynamicSamples, InvalidDynamicSamples, DynamicCapacity,
    NonFiniteTransform
};

struct CollisionIssue {
    CollisionIssueCode code;
    int object;
    int detail;
};

struct CollisionEntityProxy {
    std::uint64_t generation; // locally observed lifecycle; MP has no useCount
    unsigned number;
    unsigned solid;
    unsigned model;
    int entityType;
    int contents;
    float origin[3];
    float angles[3];
    float mins[3];
    float maxs[3];
    float absMins[3];
    float absMaxs[3];
};

struct CollisionDynamicBrushProxy {
    std::uint64_t generation;
    unsigned id;
    unsigned model;
    int contents;
    float origin[3];
    float axis[3][3];
    float radius;
};

// Publish completed frames immutably. Traces read these records and immutable
// BSP geometry only; they never read CG poses, DObjs, or DynEnt mutable storage.
// CM_TempBoxModel/CM tracing still require a dedicated installed trace scratch.
struct CollisionFrame {
    CollisionMapToken map;
    std::uint64_t gameplayTick;
    int sampleTime;
    int currentSnapshotTime;
    int nextSnapshotTime;
    DynamicBrushSource dynamicSource;
    unsigned entityCount;
    unsigned dynamicBrushCount;
    unsigned issueCount; // total; details are bounded to CollisionIssueCapacity
    bool complete;
    CollisionIssue issues[CollisionIssueCapacity];
    CollisionEntityProxy entities[CollisionEntityCapacity];
    CollisionDynamicBrushProxy dynamicBrushes[CollisionDynamicBrushCapacity];
};

struct CollisionBuildState {
    CollisionMapToken map;
    std::uint64_t serial;
    std::uint64_t lastSeen[CollisionEntityCapacity];
    std::uint64_t entityGeneration[CollisionEntityCapacity];
    int entityType[CollisionEntityCapacity];
    int entityFlags[CollisionEntityCapacity];
};

// Preallocate frame/state outside gameplay. This builder does not allocate.
// Inputs are the accepted current/next snapshot pair at one fixed gameplay time,
// not presentation time or camera poses. It retains accepted snapshot order;
// legacy CG sector traversal can differ at equal-fraction contacts. Validate
// shadow parity before installing this handler in pmoveHandlers.
CollisionMapToken captureCollisionMapToken(std::uint64_t session, std::uint64_t generation);
bool buildCollisionFrame(CollisionFrame& frame, CollisionBuildState& state,
    const CollisionMapToken& map, const snapshot_s* current, const snapshot_s* next,
    int sampleTime, std::uint64_t gameplayTick, const DynamicBrushSamples* dynamic);
bool collisionFrameMatchesMap(const CollisionFrame& frame, const CollisionMapToken& active);
const char* collisionIssueName(CollisionIssueCode code);

// False means unsupported/stale input; output is untouched. No BSP-only fallback.
bool traceCollisionFrame(const CollisionFrame& frame, const CollisionMapToken& active,
    trace_t* result, const float* start, const float* mins, const float* maxs,
    const float* end, int passEntityNum, int contentMask);

} // namespace khz

// One thread pins a frame for a complete Pmove/replay job. Diagnostic pose samples
// are deliberately rejected here. The caller must also initialize independent CM
// scratch and respect map lifetime barriers before moving this onto a worker.
bool KHZ_BindPredictionCollisionFrame(const khz::CollisionFrame* frame,
    const khz::CollisionMapToken& activeMap);
void KHZ_UnbindPredictionCollisionFrame();
bool KHZ_HasPredictionCollisionFrame();

// Main-thread migration adapter: rebuild the preallocated owner-local frame at
// one fixed prediction tick, then pin it only if the complete world is supported.
// Always unbind the previous frame first, including on rejection. No DynEnt pose
// sampling occurs here: maps requiring those owner samples explicitly reject.
// The caller owns the prediction scope, calls Unbind when it ends, and counts or
// selects legacy fallback on false. This does not establish worker ownership.
bool KHZ_BeginClientCollisionFrame(std::uint64_t session, std::uint64_t gameplayTick,
    int sampleTime, const snapshot_s* current, const snapshot_s* next);

void __cdecl KHZ_PredictionTrace(trace_t* result, const float* start, const float* mins,
    const float* maxs, const float* end, int passEntityNum, int contentMask);

// Register once during KHZ_Init; command khz_collision_shadow writes a bounded
// report to khz_collision_shadow.json. It never changes the production handler.
void KHZ_InitCollisionDiagnostics();
void KHZ_RunCollisionShadow();
const char* KHZ_CollisionProbeJSON();
