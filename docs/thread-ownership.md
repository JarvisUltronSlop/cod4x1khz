# Client thread ownership

The current client has an independent 1 ms simulation worker, a separate Raw
Input producer, the main presentation thread and the existing renderer backend.
This supersedes the historical cooperative frontend described in the
[retained earlier audit](history/thread-ownership-before-build73-correction.md).
The boundary is implemented; complete dynamic-world parity and renderer-active
authoritative replay remain release obligations.

## Implemented ownership

| Domain | Owner and boundary |
| --- | --- |
| Clock, transport and prediction | `khz_client_owner.cpp` starts `ClientSimulation1kHz` after map/CG preparation. Its absolute-deadline loop pumps transport, consumes input and predicts with a fixed step, bounded catch-up passes and retained debt. Active main-thread `Com_Frame` does not also run `KHZ_ClientTick`. |
| Mutable gameplay | The worker owns private CG/Cgs/entity/weapon storage, accepted snapshots, animation conditions, aim controls and gameplay RNG. CL/CG accessors select the actual owner domain. Main access to controls requires an acknowledged park. |
| Physical input | `khz_input_owner.cpp` uses a separate message-only window for mouse and keyboard Raw Input, QPC timestamps and a bounded queue. Focus transitions, overflow reset and graphics-window rebind are explicit. `khz_client_input.cpp` consumes events through tick deadlines with frozen binding/key maps. UI text input remains with the native window. |
| World and player publication | Three preallocated `PredictionFrame` records carry player state, active current/next snapshot entities and clients, command/session/generation/tick metadata and animation feedback. The main thread pins a completed record; it may skip superseded frames. Unused snapshot tails are not copied every tick. |
| Visual events and reliable commands | Bounded single-producer/consumer journals transfer entity, damage, animation-sound and menu events, plus ordered reliable presentation commands. Presentation consumes these rather than reading mutable worker state. |
| Gameplay animation and aim | `khz_client_body.cpp` and `khz_client_pose.cpp` prepare native model compositions, private DObj/controller/skeleton state and a private 4,096-node XAnim arena. The worker advances its own pose and aim targets; `KHZ_ClientOwnerPublishAimTargets` does not import render-produced targets. Presentation retains its visual trees and HUD targets. |
| Visibility | `khz_client_visibility.cpp` prepares filtered immutable effect definitions and private gameplay particle state. Native lifetime, motion, alpha and seed logic run on ticks without render vertices or presentation cameras. Cache extension requires a main barrier. |
| Prediction collision | The worker builds bounded records from accepted snapshots and supported trajectories, binding one complete map/session generation for prediction. `THREAD_CONTEXT_CLIENT_SIM` has its own CM temporary brush/model and partition scratch. An incomplete worker world requests a park; it does not concurrently trace mutable presentation entities. |
| Floating point and errors | Worker entry configures the shared gameplay floating-point environment and tick publication checks it. Native worker errors are caught by the worker's own jump boundary, park it and report to main; they do not longjmp across threads. |
| Assets and lifecycle | Main prepares/releases private domains while the worker and renderer jobs are quiescent. Ordered config/asset metadata barriers preserve worker histories and mutable private animation/FX. Map changes, graphics/audio restart and disconnect stop or park the worker before native lifetime work, then resume a valid generation. |

Key source entry points are [the owner](../engine/src/khz/khz_client_owner.cpp),
[runtime scheduling](../engine/src/khz/khz_runtime.cpp),
[Raw Input producer](../engine/src/khz/khz_input_owner.cpp),
[collision adapter](../engine/src/khz/khz_collision.cpp),
[CM preparation](../engine/src/qcommon/cm_load.cpp) and
[thread initialization](../engine/src/qcommon/threads.cpp).

Lifecycle and metadata barriers intentionally pause simulation when native asset
storage must change. This is separate from ordinary rendering work. Progress
through a sampled rendering stall does not establish unlimited journal capacity
or safe operation through arbitrary stalls.

## Measured scope

The [Build 73 four-case matrix](build73-timing-validation.md) checks actual worker
progress, unique human command/snapshot rates, native bot controls and snapshot
collision dispatch on Killhouse and Crash. All four sixty-second cases meet the
rate, sub-millisecond server p99 and sampled debt gates. Collision dispatch
coverage is not a trace count or a proof of every movement behavior.

The [Build 73 lifecycle supplement](../results/native-lifecycle-20261003-122949-049861-14960/result.json)
passes hosted map change, fast/full restart, graphics/audio restart and explicit
Leave with exact owned-server cleanup. The earlier failed bounded query remains
in its original result. [Build 70 functional checks](build70-functional-validation.md)
cover stock Options, FOV and eight-second render-cap stages; these remain tied
to their historical binary.

## Remaining validation and limitations

- Complete authoritative-world checkpoint integration is staged and disabled.
  Its source-owned world/VM/allocator/network/physics records have not yet passed
  an actual loaded-map capture/restore and identical-input replay with the real
  renderer at 125, 240 and 1,000 FPS.
- Supported snapshot collision does not establish fixed-tick DynEnt/ODE,
  ragdoll, vehicle and every moving-world behavior. Unsupported worlds retain
  explicit rejection/fallback diagnostics; they cannot count as complete parity.
- Wider stairs, slopes, ladders, mantle, stance, pickup, weapon, event, historical
  bone/hit and aim-visibility comparisons remain required against stock tuning.
- Exercise queue pressure, physical input/focus transitions, worker errors and
  map/asset lifetime changes under longer stalls. Boundaries and bounded storage
  alone do not prove all recovery cases.
- Rendering-cap stages from moving scenes are throughput tests. They do not
  compare full-world outcomes under identical timestamped input.

The [implementation status](status.md) records these release gates separately
from compiled code, policy tests and historical measurements.
