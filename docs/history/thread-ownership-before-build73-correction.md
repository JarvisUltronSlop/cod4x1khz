# Client thread ownership and migration obligations

This document records the inspected source and the remaining architecture work.
The current custom client has a **cooperative main-thread frontend** servicing
gameplay and presentation clocks, plus the engine's existing renderer backend.
It does not yet have an independent client simulation worker. A published player
state, a render-time restoration guard, tick-owned local turn calculations,
isolated presentation random streams, and a collision-frame candidate are
useful migration seams; they do not make the complete CG world immutable.
Compilation or short throughput tests cannot establish that missing ownership
boundary. See [implementation status](status.md) for separately recorded results.

## Implemented seams and their limits

| Source and function | Current behavior | Remaining obligation |
| --- | --- | --- |
| [common.cpp](../engine/src/qcommon/common.cpp), `Com_Frame` | Calls `KHZ_ClientTick` and `KHZ_PresentFrame` from the frontend loop. | Move active gameplay, command generation, and protocol progress to an owner that continues while a frontend frame is executing. |
| [khz_runtime.cpp](../engine/src/khz/khz_runtime.cpp), `KHZ_WaitFrame` | Services additional presentations between absolute simulation deadlines, waiting on the existing renderer completion event when necessary. | `SCR_UpdateScreen` still occupies the same thread as the next simulation tick. Long frontend work still delays input and prediction. |
| [khz_runtime.cpp](../engine/src/khz/khz_runtime.cpp), `KHZ_ClientTick` | Advances snapshot processing, command feedback, input, prediction, local turn/condition calculations, recoil, and key catchers with a 1 ms frame duration; publishes player state with session/tick metadata. | These calls still use mutable CG/CL globals and main-thread animation context. They cannot simply be called concurrently. |
| [publication.hpp](../native/publication.hpp), `TriplePublication`, and `KHZ_PresentFrame` | Publishes a buffered player-state view. Presentation stages that view into legacy CG state, counts edits, then restores player state and selected CG/BG clocks. | The guard covers selected fields, not centities, clientinfo, DObj, item state, command feedback, or the complete world. It is a cooperative compatibility mechanism, not a concurrent CG-state guard. |
| [cg_view_mp.cpp](../engine/src/cgame_mp/cg_view_mp.cpp), `CG_InitView` | Custom presentation no longer runs `CG_PredictPlayerState`. | Camera, vehicle-seat, entity, and animation code still accesses mutable presentation/CG objects. |
| [cg_draw_mp.cpp](../engine/src/cgame_mp/cg_draw_mp.cpp), `CG_UpdateCommandFeedback` | FOV sensitivity, aim, weapon selection, and extra buttons are updated from the simulation tick before `CL_Input`; custom rendering no longer invokes this feedback block. | Move feedback state and command construction into the simulation owner, with explicit visual-event inputs where needed. |
| [cg_shellshock.cpp](../engine/src/cgame/cg_shellshock.cpp), `CG_UpdateShellShockInput` | Shellshock input sensitivity and turn caps advance on ticks; custom MP presentation skips those look-control calls. | Publish visual shock state independently and keep mutable control state with the simulation owner. |
| [bg_animation_mp.cpp](../engine/src/bgame/bg_animation_mp.cpp), `BG_ClearPredictionAnimationOwner`, `BG_UpdatePredictionAnimationState`, `BG_PlayerAnimation`; declarations in [bg_local.h](../engine/src/bgame/bg_local.h) | Clears registration at tick entry, then updates predicted local player angles and conditions after prediction. Presentation skips these calculations for that exact BG/clientinfo context. Local `legs.yawAngle`, `torso.yawAngle`, their `yawing` fields and condition inputs advance on ticks; `turnAnimType`/`turnAnimEndTime` remain movement-owned. Visual lerp-frame/XAnim work remains on the frontend. | The clientinfo and condition storage is still shared and main-thread owned. Private replay state, server/client parity, and lifecycle barriers remain necessary. |
| [khz_collision.h](../engine/src/khz/khz_collision.h), [khz_collision.cpp](../engine/src/khz/khz_collision.cpp), `buildCollisionFrame`, `traceCollisionFrame`, `KHZ_PredictionTrace` | Builds bounded collision proxies from accepted snapshot trajectories at a fixed sample time and traces immutable records plus BSP. Build 43 short matches validated the bound handler-3 path on both target maps, with counted legacy fallback available for unsupported worlds. | No simulation worker, fixed DynEnt owner, dedicated CM scratch, or concurrent lifecycle publication has been installed. |
| [com_math.cpp](../engine/src/universal/com_math.cpp), `KHZ_Rand`, `random`, `flrand`, `irand` | `KHZ_IsPresentationContext` selects private presentation seeds. The context surrounds the full `SCR_UpdateScreen` call. Active inspected FX, tracer, DynEnt-piece, and UI direct-rand consumers use the helper. | This isolates those rendering consumers from gameplay RNG. It does not establish full-world determinism, isolate all tick-time cosmetic event consumption, or make other shared globals safe. |

`KHZ_PresentFrame` is session-aware and can skip superseded presentations. Its
scoped presentation flag restores nested context on normal return;
`KHZ_WaitFrame` clears it after a possible `Com_Error` longjmp. Worker error
handling must have its own boundary rather than longjmp into another thread.

## Implemented collision candidate and measured scope

Build 41 compiled both engine targets and passed `khz_core` and
`khz_publication` (2/2), as recorded in the
[build log](../results/custom-build-41.log). Active rendered matches exercised
the animation seam and the collision shadow diagnostic. The diagnostic results
were:

| Map / active clients | Entity proxies / DynEnt brushes | Capsule sweeps | Trace / pose / coverage mismatches | Ordering ambiguities |
| --- | --- | --- | --- | --- |
| [Killhouse / 1](../results/collision-41-console.log) | 3 / 0 | 46 | 0 / 0 / 0 | 0 |
| [Crash / 1](../results/collision-41-crash-console.log) | 0 / 0 | 46 | 0 / 0 / 0 | 0 |
| [Crash / 4](../results/collision-41-fourplayer-crash-console.log) | 3 / 0 | 46 | 0 / 0 / 0 | 0 |

Build 43 also passed the [native suite](../results/custom-build-43.log). Its
[four-player probes on both maps](../results/collision-43-fourplayer-console.log)
had 46 sweeps and zero mismatches each, with 6 entity proxies on Killhouse and
3 on Crash, again with no DynEnt brushes. All nine rejection/binding guards
passed: incomplete/unbound frames, stale generation/session/CM identity,
diagnostic poses and invalid inputs, plus valid bind/trace/unbind and restoration
of the prior binding. No guard was skipped (required/passed mask 511).

Each sampled frame was complete with zero build issues. These are bounded sweeps
at one fixed gameplay sample per probe, not full-world or complete Pmove parity
tests. The one-client Crash probe covered BSP without entity proxies. None
exercised a DynEnt brush. The diagnostic's elapsed microseconds are probe wall
times, not steady simulation p99 or proof of sustained 1 kHz. Native publication
tests do not establish concurrent ownership of CG state.

`CollisionFrame` preallocates 1,024 entity proxies, 1,024 dynamic-brush proxies,
and 32 issue details; the issue counter can exceed that detail limit. The builder
performs no allocation. Normal entity transforms come from accepted current/next
snapshots and supported trajectories at the supplied gameplay time, with player
interpolation and mover translation. It uses real CM brush models and contents,
and the stock packed-solid bounds convention. Locally observed entity generations
track disappearance/reappearance, type changes and teleport flags; they are not
an authoritative protocol use-count.

Unsupported physics/ragdoll trajectories, invalid trajectory parameters,
nonfinite transforms, missing client/mover data and attached-vehicle seat
transforms make a frame incomplete. Dynamic maps require samples for every map
brush id, including invisible or unlinked brushes, with a matching sample time
and declared source. Only a defined physics owner may supply
`FixedGameplayOwner`. The diagnostic can copy main-thread DynEnt poses as
`DiagnosticMainThreadPose`, but `KHZ_BindPredictionCollisionFrame` rejects that
source. Missing samples must never become an apparently empty dynamic world.

`traceCollisionFrame` rejects incomplete frames, invalid trace input and stale
map/session identities without modifying the result. It has no BSP-only fallback.
The token checks identity, not asset lifetime: readers still need to quiesce
before map storage is freed. `KHZ_PredictionTrace` reports an unbound/incomplete/
stale frame through `Com_Error`; a future worker needs its own error boundary.

## Collision migration obligations

The Build 42/43 integration uses snapshot collision in production. With
`khz_snapshot_collision=true` by default, `KHZ_ClientTick` calls
`KHZ_BeginClientCollisionFrame` after `CL_Input`, counts a bound frame as
`snapshot_collision_ticks` or rejection/disabled use as `legacy_collision_ticks`,
runs prediction, then unbinds immediately. `CG_PredictPlayerState_Internal` selects
handler 3 only while a complete matching frame is bound. Unsupported worlds
explicitly retain the legacy CG path; this is counted fallback, not incomplete
proxy tracing. The owner deliberately supplies no DynEnt samples yet, so maps
requiring them reject this path. Main-thread assertions remain in force.
These counters cover dispatch whenever both snapshots exist, including dead or
followed-player interpolation. A snapshot fraction of 1 means a complete frame
was bound for every dispatch, not 1,000 collision traces per second; alive-player
gameplay and `prediction_steps` checks remain independent.
The shadow/guard results above do not establish complete gameplay correctness or
steady throughput of this production integration.

Separate [Killhouse](../results/mp_killhouse-60s-uncapped-720p-20261003-000813.json)
and [Crash](../results/mp_crash-60s-uncapped-720p-20261003-000958.json) Build 43
60-second matches passed rate, server work p99, backlog, gameplay, collision,
script and GPU present checks with four clients and uncapped 720p rendering. Every client
bound complete snapshot frames at approximately 1,000 dispatches/s, with fraction
1 and zero legacy dispatches. Gameplay activity and prediction-step checks also
passed independently. This validates the production path in those sampled
supported worlds; full movement parity, DynEnt ownership and progress through
arbitrarily long frontend frames remain open.

The inspected movement path is
`Pmove -> pmoveHandlers[0].trace -> CG_TraceCapsule -> CG_Trace` in
[bg_public.h](../engine/src/bgame/bg_public.h),
[bg_pmove.cpp](../engine/src/bgame/bg_pmove.cpp), and
[cg_world.cpp](../engine/src/cgame/cg_world.cpp).
`CG_TraceCapsule` requests **nonlocational** tracing with static-model tracing
disabled. It therefore does not need rendered bones or DObj poses. Calling the
existing CG trace from a worker is unsafe: it asserts main-thread ownership,
uses mutable CG collision trees, and reads `cent->pose` and `cent->nextState`.

The implemented candidate uses the same trace signature:

```cpp
void KHZ_PredictionTrace(trace_t*, const float* start, const float* mins,
                         const float* maxs, const float* end,
                         int passEntityNum, int contentMask);
```

The integration adds handler index 3 `{ KHZ_PredictionTrace, nullptr }`, retaining
index 2 `KHZ_ReplayTrace` for static replay. `KHZ_BindPredictionCollisionFrame`
supplies a TLS-pinned frame for a whole prediction job. The candidate does not
call `CG_GetEntity`, CG collision-world setters or DObj callbacks; that property
alone does not authorize worker use or prove all replay-time geometry semantics.

The proxy types in `khz_collision.h` hold identities, fixed-time transforms,
contents, model/local bounds and broad bounds without copying the visual world.
Snapshot stance and entity reuse must update those records. DObj-only vehicle
geometry is outside this nonlocational handler's current semantics; linked-seat
state requires separate authoritative handling. Item pickup journals are private
prediction state, not collision geometry.

The source of these frames must be accepted network state and a fixed tick time.
Copying `cent->pose` after rendering would preserve presentation-dependent
collision and is insufficient for the final architecture. The candidate uses
such samples only for explicitly diagnostic DynEnt input. `CG_AddPacketEntity` in
[cg_ents_mp.cpp](../engine/src/cgame_mp/cg_ents_mp.cpp) computes poses and links or
unlinks CG collision while drawing packet entities. `CG_CalcEntityLerpPositions`
also interpolates snapshot trajectories and invokes cosmetic physics/ragdoll
paths. The candidate extracts supported trajectory interpolation into a fixed-time
builder and rejects unsupported physics paths. Any collidable TR_PHYSICS/DynEnt
pose must be produced by a defined physics owner; the simulation cannot sample
concurrently changing ODE or ragdoll objects. `R_RenderScene` in
[r_scene.cpp](../engine/src/gfx_d3d/r_scene.cpp) still calls `FX_RunPhysics` and
`DynEntCl_ProcessEntities` during presentation. Move gameplay-relevant dynamics
and collision-link updates to a fixed owner before publishing those proxies.

Trace implementation must preserve the inspected behavior:

- `CG_Trace` first calls `CM_BoxTrace` against BSP model 0, sets world hit metadata,
  and exits immediately at fraction 0. Capsule traces then visit entity solids
  and `DynEntCl_ClipMoveTrace`; the current point branch visits entity solids
  without the DynEnt capsule branch.
- `CG_GetEntityBModelBounds` decodes a packed solid as XY bounds
  `1 - BYTE0` through `BYTE0 - 1`, and Z bounds `2 - BYTE1` through
  `BYTE2 - 33`. Do not substitute renderer bounds or a different box convention.
- `CG_GetEntityBModelContents` uses `CM_ContentsOfModel` for brush solids,
  CONTENTS_PLAYER for packed ET_PLAYER, and CONTENTS_SOLID for other packed
  solids. Brush solids with `eFlags & 1` are excluded.
- `CG_ClipMoveToEntity` uses `CM_TempBoxModel` for packed boxes and
  `CM_TransformedBoxTrace` for the narrow phase. Its model/mask argument packs
  both values into a 64-bit integer; preserve that API convention.
- `DynEnt_ClipMoveTraceToBrush` in
  [DynEntity_coll.cpp](../engine/src/DynEntity/DynEntity_coll.cpp) filters contents
  and clipMove, tests a broad sphere, and calls
  `CM_TransformedBoxTraceRotated`. The traversal in
  [DynEntity_client.cpp](../engine/src/DynEntity/DynEntity_client.cpp) currently
  reads global mutable brush poses; replace that access with frame data.
- Preserve hit type/id, fraction, normal, walkable, contents, surfaceFlags,
  startsolid/allsolid, and metadata clearing. Strict fraction comparisons and
  traversal order matter for equal-distance contacts. Establish parity before
  changing broad-phase ordering or acceleration structures.

`PM_playerTrace` retries character start-solid contacts with a changed mask;
prone checks, lean, ladders, ground, stairs, foliage and mantle all use the handler
callback. There is no `BG_GetPlayerCollisionBounds` symbol in this source tree;
`PM_CheckDuck` supplies player movement bounds. `Mantle_FindMantleSurface` in
[bg_mantle.cpp](../engine/src/bgame/bg_mantle.cpp) requires CONTENTS_MANTLE and
the original mantle surface flags. Generic AABB approximations cannot replace
the BSP/brush narrow phase.

`CG_AdjustPositionForMover` in `cg_ents_mp.cpp` is a clean extraction seam: it
looks up a script mover or plane, evaluates its position/angular trajectories at
from/to times, and applies deltas. Use the immutable mover lookup in both
prediction and correction. `Pmove` has no separate mover callback in this tree;
the existing prediction wrapper performs these adjustments.

## Private animation and movement state

[bg_animation_mp.cpp](../engine/src/bgame/bg_animation_mp.cpp) defines plain
process-global `bgs` and `globalScriptData`. `BG_CheckThread` in
[bg_misc.cpp](../engine/src/bgame/bg_misc.cpp) asserts main-thread ownership.
`PmoveSingle` calls `BG_AnimUpdatePlayerStateConditions`, while movement and
weapons use `BG_AnimScriptAnimation`, `BG_AnimScriptEvent`, and condition helpers.
Those calls select and change player-state animation data; skipping them would
change stock behavior.

Use an explicit private prediction context or custom TLS pointers for these
calls. Keep parsed animation scripts and weapon definitions immutable and
lifetime-pinned. Own mutable condition bits and local client turn state on the
simulation worker. Worker assertions must check this actual context rather than
globally relaxing `Sys_IsMainThread` checks. Loading, parsing, DObj creation and
the visual XAnim tree remain outside worker Pmove.

`PM_Footsteps_NotMoving` and `PM_Footsteps_TurnAnim` in `bg_pmove.cpp` read/change
`clientinfo.legs.yawing`, legs/torso yaw angles, turnAnimType and turnAnimEndTime.
The custom MP seam now invokes `BG_PlayerAngles` then `BG_AnimPlayerConditions`
once per prediction tick using predicted view angles, movement direction and
lean. `BG_PlayerAnimation` suppresses those calls only for the registered local
BG/clientinfo pair; remote players, corpses, server calls and stock builds retain
their previous behavior. `BG_RunLerpFrameRate`, animation verification and visual
XAnim work remain in presentation and do not write the inspected yaw/yawing or
turn fields. `CG_ResetPlayerEntity` in
[cg_players_mp.cpp](../engine/src/cgame_mp/cg_players_mp.cpp) still initializes
that state during snapshot/entity reset. Clearing registration before every tick
prevents stale suppression when prediction is skipped or the map changes.

This removes the inspected local render-time turn-state feedback; it is not a
private animation context. Replay still shares clientinfo and conditions, and
the arithmetic runs after prediction for subsequent movement ticks. Separate
those fields into the private job before moving Pmove, with server/client turn
parity and full replay checks. Do not call full `BG_PlayerAnimation` on a worker
or move DObj/XAnim tree mutation with the arithmetic seam.

`BG_ExecuteCommand` directly invokes `globalScriptData->playSoundAlias`; replace
the worker callback with a bounded event enqueue. `Mantle_GetAnimDelta` uses
`XAnimGetAbsDelta`: the inspected chain in
[xanim.cpp](../engine/src/xanim/xanim.cpp) and
[xanim_calc.cpp](../engine/src/xanim/xanim_calc.cpp) reads immutable animation
parts and uses stack-local delta sampling, rather than mutable DObj/XAnim info.
Asset lifetime must still be pinned. `KHZ_BobCycle` is already private-state
arithmetic because its fractional remainder lives in the supplied player state.

Pmove's remaining inspected CL/CG/G calls include
`CL_GetLocalClientActiveCount`, `CG_ShouldPlaySoundOnLocalClient`, and
`G_ExitAfterConnectPaths`, which currently return constants, plus
`G_IsServerGameSystem` for animation debug logs. Replace implicit cross-system
dependencies with explicit worker settings where practical. Capture gameplay
dvar changes at tick boundaries; concurrent reads of mutable dvar storage are
not an ownership solution. Steady worker simulation must enqueue diagnostics
instead of performing synchronous output.

## CM scratch and map lifecycle

CM tracing has mutable per-thread scratch even when BSP geometry is immutable:
`CM_GetTraceThreadInfo` in
[cm_trace.cpp](../engine/src/qcommon/cm_trace.cpp) reads TLS slot 3 and increments
partition check counters. `CM_TempBoxModel` modifies that thread's temporary box
model/brush. `CM_InitThreadData` in
[cm_load.cpp](../engine/src/qcommon/cm_load.cpp) allocates partition scratch and
initializes separate model/brush storage during map load.

Add a dedicated simulation trace context and initialize it on worker entry with
the correct TLS and profile context. Do not borrow renderer worker 0/1 scratch.
The MP `ThreadContext_t` in
[rb_backend.h](../engine/src/gfx_d3d/rb_backend.h) currently reserves four trace
contexts; update trace counts/storage, thread names, initialization and any
index-based affinity code deliberately. `Sys_InitThread` in
[threads.cpp](../engine/src/qcommon/threads.cpp) calls `Com_InitThreadData` and
`Profile_InitContext`; `Com_InitThreadData` in
[q_shared.cpp](../engine/src/universal/q_shared.cpp) installs trace TLS. A
separate explicit scratch installation API is also possible, but must preserve
independent profiling, error, and trace storage.

At disconnect, map restart/change, asset reload, or fatal worker error: stop new
jobs, wait for worker quiescence, invalidate old session/map publications, then
free/reload assets and create scratch for the new clipmap. Resume only after a
complete new generation is published. No buffer may retain a dangling clipmap,
animation, weapon, item, or entity reference. Allocate queues, proxy storage,
scratch and history before active gameplay.

## Prediction journals and full owner migration

Command feedback still has an indirect presentation dependency. `CL_MouseMove`
in [cl_input.cpp](../engine/src/client_mp/cl_input.cpp) calls
`AimAssist_UpdateMouseInput`, which applies auto-melee and melee charge from the
target arrays, including when mouse movement is zero. `CG_DrawActiveFrame` builds
those arrays through `AimTarget_ClearTargetList`, entity processing,
`AimTarget_UpdateClientTargets` and `AimAssist_UpdateScreenTargets`. In
[aim_target.cpp](../engine/src/aim_assist/aim_target.cpp) and
[aim_assist.cpp](../engine/src/aim_assist/aim_assist.cpp), visibility and aim
position can read DObj aim bones, CG traces, smoke visibility and presentation
camera/FOV transforms. A worker needs a fixed-time target/visibility contract or
explicit immutable bone/visibility samples. Collision proxies alone do not solve
this; leaving these render-fed arrays shared would keep commands dependent on
presentation cadence.

`CG_PredictPlayerState_Internal` in
[cg_predict_mp.cpp](../engine/src/cgame_mp/cg_predict_mp.cpp) combines replay with
snapshot access, CG smoothing, mover correction, item prediction and event
dispatch. Moving that entire wrapper onto a thread would race the visual world.
Extract a prediction job with authoritative player state and acknowledgement,
immutable world generation/sample, captured settings, and bounded pending input
history. Return player state, touched entities, view-step changes, correction
information, and events; publish only completed ticks.

`CG_TouchItemPrediction` / `CG_TouchItem` currently alter
`cent->nextState.lerp.eFlags` and `cent->miscTime`. Use immutable item states and a
worker-local journal keyed by entity generation and input sequence instead.
`CG_TransitionPlayerState` / `CG_CheckPlayerstateEvents` in
[cg_playerstate.cpp](../engine/src/cgame/cg_playerstate.cpp) dispatch damage
feedback and `CG_EntityEvent`. Deduplicate prediction events during replay and
deliver bounded journals to the presentation owner; publishing only the latest
player state must not silently drop intervening firing/audio events.

A physics-only worker is an intermediate stage. For 1 kHz client logic and
command/snapshot progress during a long frontend frame, the simulation owner
must also own connection state, accepted snapshots, prediction, input command
construction and protocol send/receive. Network packet processing should
validate/enqueue data without touching the presentation world. Console/config
changes and connection/map transitions need ordered mailboxes or lifecycle
barriers. Snapshot decode baselines and the acknowledged input history require
one clear owner.

`KHZ_RawInput`, `KHZ_RawMouse`, `KHZ_QueueKey`, and `KHZ_PumpKeys` currently use
bounded timestamped queues and consume events through tick deadlines. These are
producer/consumer seams, not proof of input delivery through long rendering:
Windows messages still depend on the thread that pumps the game window. Keep
that pump responsive or provide a dedicated valid input-window/pump owner;
respect focus transitions, key-up recovery and overflow handling. The worker
consumes raw events and owns mutable key/button, shellshock, aim and command
state. The presentation thread should never feed unsynchronized visual-frame
state back into next-tick command selection.

The final presentation owner receives immutable player/world views and events,
owns camera smoothing, FX/audio and visual animation, and prepares frames for
the existing backend. It may skip superseded views without skipping gameplay
or protocol ticks. Legacy main-thread assertions must be adapted by subsystem
ownership, not removed wholesale. Preserve one producer/consumer contract per
bounded queue and publication; use deltas/active proxies rather than per-client
copies of complete world state.

## Frontend work to profile before caching

The completed [Build 43 Killhouse match](../results/mp_killhouse-60s-uncapped-720p-20261003-000813.json)
records approximately 678 us average frontend work versus 50 us prediction work.
These coarse wall timers include waits and overlap backend work; do not sum
them as CPU cost. They justify measuring frontend preparation phases before
choosing a cache or claiming that a particular function is the bottleneck.

Uncapped frames can repeat an integer CG time, but that does not make all work
redundant. `CG_DrawActiveFrame` clears the scene and `CG_AddPacketEntities` submits
it again. Split trajectory/collision/animation preparation from per-frame scene
submission rather than skipping the whole call. `CG_UpdateViewOffset` currently
calls `CL_ResetSkeletonCache` each presentation; bone reuse needs keys for pose,
animation, attachment/controller and view-offset changes plus backend lifetime
safety. `CG_UpdateEntInfo` and DObj updates also touch animation notifications;
gate notification production/consumption together rather than reusing stale
global notifications.

Measure these phases and FX update/sort, DynEnt scans and audio drain separately.
Any cache needs time and spawn/despawn/pose/visibility revisions, not just a
matching millisecond. Aim targets and gameplay DynEnt ownership above must be
resolved before eliminating repeated preparation. Camera-dependent draw work
still runs for each submitted frame. These are source-identified candidates,
not measured wins or implemented caches.

## Next validation stages

1. Expand the implemented shadow diagnostic on the current owner beyond the
   bounded Build 41/43 probes. Compare full trace results with CG collision
   for player bodies, packed solids, moving/rotating brushes, vehicles,
   stairs/slopes, ladders, prone and mantle. Exercise positive fixed-owner DynEnt
   samples, unsupported-world fallback and stale/restarted map rejection.
2. Run private-context Pmove on the current thread. Compare complete player state,
   movement outputs and event journals using identical input/authoritative
   baselines. Cover turn animations, weapon timing, pickups and mover correction.
3. Move the verified prediction context onto its dedicated trace worker. Exercise
   map lifecycle and asset lifetime barriers, stalled presentation, queue pressure
   and error recovery before changing network/input ownership.
4. Complete network, input and snapshot ownership. Inject controlled long frontend
   frames and verify 1,000 unique commands/snapshots and simulation steps while
   rendering is blocked, with retained bounded debt and no duplicated presses.
5. Replay the actual authoritative world with identical timestamped inputs at
   125, 240, 1,000 FPS and uncapped presentation. Compare authoritative hashes and
   client correction/event behavior with dynamic collision enabled. Static BSP
   replay and immutable read-cadence tests alone do not establish this result.
6. Run the shortened match benchmarks on both maps; measure worker tick lateness,
   work p99, protocol rates, queue/history use, corrections, CPU and GPU presents.
   Longer stability/overflow coverage remains a separate acceptance obligation.

The evidence above supports the narrow implemented seams and sampled collision
agreement. Complete gameplay replay, dynamic-world parity, concurrency and
sustained 1 kHz through long frontend frames remain validation obligations.
