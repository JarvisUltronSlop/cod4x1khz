#pragma once
#include "khz_server_checkpoint.h"

// Opt-in native fragments live at the END of cm_load.cpp, cm_world.cpp and
// server/sv_world.cpp. COD4X1KHZ_SERVER_CHECKPOINT is OFF by default and requires
// custom MP/dedicated. Register all three parts before capture; enroll reviewed
// actual native entry guards separately. Inclusion alone does not enroll them.
inline constexpr unsigned KHZ_ServerCollisionRequiredParts = 7;
enum KHZServerCollisionQueryGroup : unsigned {
    KHZ_COLLISION_CM_SCRATCH = 1,
    KHZ_COLLISION_CM_WORLD = 2,
    KHZ_COLLISION_SV_WORLD = 4
};
void KHZ_ServerCollisionLoadRegisterCheckpoint();
void KHZ_ServerCollisionWorldRegisterCheckpoint();
void KHZ_ServerCollisionQueryRegisterCheckpoint();

// Dedicated/main owner after the real CM/G_Init map initialization and before
// capture. Replace only exact live map spans which these providers restore.
bool KHZ_ServerCollisionBindMap(char* reason, unsigned capacity);
// Before CM map/scratch lifetime changes; retires only Collision's exact map
// range set. Other native domain bindings remain the coordinator's concern.
bool KHZ_ServerCollisionInvalidateMapBinding();
void KHZ_ServerCollisionEnrollQueryHooks(unsigned groups);
bool KHZ_ServerCollisionIdle(char* reason, unsigned capacity);
const char* KHZ_ServerCollisionStatusJSON(); // actual main-owner diagnostics
// Observer/wrapper must arm this actual query fence BEFORE the first of ALL14
// native providers captures. It remains held through branch/restore. Release
// only after coordinator Empty/discard, including a refused early capture.
bool KHZ_ServerCollisionFenceEnter(char* reason, unsigned capacity);
void KHZ_ServerCollisionFenceRelease();
// Native longjmp cleanup, after aborting the current main-owner query stack.
// Never clears another owner's active query counter or makes a branch valid.
void KHZ_ServerCollisionErrorCleanup();

// Pair every query/link entry before its first native read/write. Nested native
// calls are counted; caller MUST handle !allowed() before performing work.
// Unknown/foreign query owners while pinned are denied and invalidate replay.
bool KHZ_ServerCollisionQueryBegin(unsigned group, int* slot);
void KHZ_ServerCollisionQueryEnd(int slot);
struct trace_t;
// Conservative caller-owned result on a refused query; touches no CM state.
void KHZ_ServerCollisionBlockedTrace(trace_t* result, int contents);
class KHZ_ServerCollisionQueryScope {
public:
    explicit KHZ_ServerCollisionQueryScope(unsigned group)
        : allowed_(KHZ_ServerCollisionQueryBegin(group, &slot_)) {}
    ~KHZ_ServerCollisionQueryScope() { if (slot_ >= 0) KHZ_ServerCollisionQueryEnd(slot_); }
    bool allowed() const { return allowed_; }
    KHZ_ServerCollisionQueryScope(const KHZ_ServerCollisionQueryScope&) = delete;
    KHZ_ServerCollisionQueryScope& operator=(const KHZ_ServerCollisionQueryScope&) = delete;
private:
    int slot_ = -1;
    bool allowed_;
};

// Before map/CM/DB/hunk lifetime changes, native DynEntity/debug collision work,
// or any otherwise unsupported external collision effect. Pins remain intact
// for a controlled baseline restore after a refused branch operation.
bool KHZ_ServerCollisionAllowUnsupportedEffect(const char* operation);
struct gentity_s;
// Before G_SetOrigin/G_SetAngle/brush trajectory or brush-model assignments.
// The supported branch has original stationary brush geometry/transforms.
// Ordinary non-brush player/item/missile links remain mutable and restored.
bool KHZ_ServerCollisionAllowBrushTransform(const gentity_s*,
    const float* proposedOrigin, const float* proposedAngles, const char* operation);
// At SV_LinkEntity entry, before solid/SnapAngles/abs bounds or CM mutations.
bool KHZ_ServerCollisionAllowEntityLink(const gentity_s*, const char* operation);
// Moving/deleting/reinitializing a stationary brush is unsupported. Non-brush
// native entities remain subject to the other mandatory checkpoint domains.
bool KHZ_ServerCollisionAllowBrushMutation(const gentity_s*, const char* operation);
struct trajectory_t;
bool KHZ_ServerCollisionAllowTrajectoryMutation(const trajectory_t*, const char* operation);

// Source-owned world fragment supplies its single exact dynamic map span to
// the combined binder; it does not independently replace the Collision set.
bool KHZ_ServerCollisionWorldMapSpan(const void** base, std::size_t* bytes,
    char* reason, unsigned capacity);
// Actual unsupported native DynEntity sector globals must remain untouched;
// this is an eligibility check, not a fictitious DynEntity restore provider.
bool KHZ_ServerCollisionDynWorldIsEmpty();

