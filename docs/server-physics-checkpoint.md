# Dedicated native physics checkpoint: empty ODE scope

The source-owned provider preserves the real native physics state in the same process. Its eight fragments and native entry guards are included only under `COD4X1KHZ_SERVER_CHECKPOINT && KISAK_DEDICATED`. This opt-in macro is **OFF in the default build**. Current type/visibility staging passes with the macro OFF and ON across 54 translation units, with zero duplicate strong object definitions; a full opt-in executable link and live negative/restore checks remain outstanding. It does not provide dynamic ODE world restoration. The server coordinator still requires all fourteen domains, including Physics; each physics registration strengthens the required part mask to `0xff`.

Stock multiplayer dedicated initialization does not call `Phys_Init`: the engine's native calls are in client cgame initialization and server-command presentation paths. Dedicated player movement, missiles, movers, vehicles and script behavior still require their actual World/Collision/VM providers. Their absence cannot be replaced by an empty ODE provider.

ODE is nevertheless not an all-zero global. Construction of `odeGlob.space[3]` and `odeGlob.worldGeom` runs `dxGeom` constructors before `WinMain`, initializes native collider dispatch, sets virtual tables and installs empty-space metadata. The provider captures those original bytes, together with the native disabled `physInited` flag. An initialized-but-empty `Phys_Init` state is also accepted when every native pool and persistent-joint free chain is complete and valid.

## Exact native parts

Each fragment is included at the **end** of its native source under the dedicated opt-in macro. The corresponding registration runs once on the actual dedicated owner before the coordinator is sealed. Every part must enroll into the real native hook fence, and enrollment refuses when the opt-in hooks are not compiled. Manually including provider fragments in a macro-OFF staging wrapper provides syntax/layout inspection only. Only the original storage actually restored by these parts is advertised as a restored range.

| Part | Native source | Fragment and registration | Actual restored storage |
|---|---|---|---|
| 0 | `physics/phys_ode.cpp` | `khz_server_physics_checkpoint.inl`, `KHZ_ServerPhysicsRegisterCheckpoint` | `physGlob`, `physInited`: native roots, userdata pool, three world deadlines/fractions, jitter/gravity, persistent hinge/ball/motor free arrays, performance metadata |
| 1 | `physics/ode/ode.cpp` | `khz_server_ode_checkpoint.inl`, `KHZ_ServerODERegisterCheckpoint` | Entire original `odeGlob`: three worlds, solver rows/order arrays, per-world LCG seeds, spaces, contact groups, 512 bodies, 2048 geom slots, body/geom pool free lists, world geom |
| 2 | `physics/ode/collision_kernel.cpp` | `khz_server_ode_colliders_checkpoint.inl`, `KHZ_ServerODECollidersRegisterCheckpoint` | Actual dispatch matrix, initialization flag, native custom-class registry/count |
| 3 | `physics/ode/timer.cpp` | `khz_server_ode_timers_checkpoint.inl`, `KHZ_ServerODETimersRegisterCheckpoint` | Actual inactive debug timer registry and count; fixed QPC calibration is prewarmed and hardware identity is checked |
| 4 | `physics/phys_coll_capsulebrush.cpp` | `khz_server_physics_capsule_checkpoint.inl`, `KHZ_ServerPhysicsCapsuleRegisterCheckpoint` | Actual 32-record collision scratch and `numLocalContacts` |
| 5 | `physics/ode/error.cpp` | `khz_server_ode_errors_checkpoint.inl`, `KHZ_ServerODEErrorsRegisterCheckpoint` | Actual error/debug/message callback pointers, requiring the stock default callback set |
| 6 | `physics/ode/quickstep.cpp` | `khz_server_ode_quickstep_checkpoint.inl`, `KHZ_ServerODEQuickstepRegisterCheckpoint` | Actual `g_holdrand` alias, NULL or an exact restored world `stepInfo.holdrand` address |
| 7 | `physics/phys_contacts.cpp` | `khz_server_physics_steps_checkpoint.inl`, `KHZ_ServerPhysicsStepsRegisterCheckpoint` | Actual current/minimum/maximum native step arrays |

Images are fixed BSS byte storage. Commit only copies to the same original object/arena storage and assigns native scalars. It performs no allocation, object construction/destruction, allocator initialization, callback, gameplay, refcount operation or I/O. The coordinator validates **every provider before any commit**. Its actual owner-pause/fence observer must establish that the completed server frame has no active native physics stack or concurrent writer; checking an empty contact count alone is insufficient.

For opt-in integration, initialize the server coordinator and actual main thread context after CRT native construction, then call these registrations once before sealing provider/range registration:

```cpp
KHZ_ServerPhysicsRegisterCheckpoint();       // part 0; real PhysGlob
KHZ_ServerODERegisterCheckpoint();           // part 1; real odeGlob
KHZ_ServerODECollidersRegisterCheckpoint();  // part 2; native dispatch/classes
KHZ_ServerODETimersRegisterCheckpoint();     // part 3; prewarms calibration
KHZ_ServerPhysicsCapsuleRegisterCheckpoint();// part 4; real capsule scratch
KHZ_ServerODEErrorsRegisterCheckpoint();     // part 5; native handler set
KHZ_ServerODEQuickstepRegisterCheckpoint();  // part 6; real solver RNG alias
KHZ_ServerPhysicsStepsRegisterCheckpoint();  // part 7; native step arrays
```

Each registration calls `KHZ_ServerPhysicsFenceEnroll(part)` only after its actual provider and exact restored spans register successfully. Duplicate registration or enrollment is fatal; do not call these again after each map. Physics contributes original static storage and has no per-map range replacement API. A fixture is not eligible until the root observer verifies the fully loaded map, completed owner frame, authenticated host pause/transport drain and pinned map/hunk lifetime. The snapshot's `Frame` supplies that actual map generation, server/session/fence identity and hunk extents. `sameLifetime` permits restoring gameplay tick/time while retaining those exact asset/arena identities. Map reload/release must wait for coordinator Empty and explicit fence release; Animation and other actual map allocation providers bind their own exact spans separately.

## Ownership fence before all domains

The real paused observer must call `KHZ_ServerPhysicsFenceEnter(frame, reason, capacity)` **before the first capture or validation in any of the fourteen domains**. Physics callbacks can change World or script state, so closing at Physics part 0 would be too late. Entry requires the actual dedicated owner, a valid pinned-lifetime frame and all eight native parts (`255`). A single `seq_cst` atomic packs the closed bit and active lease count; its exclusive `0 -> Closed` compare/exchange cannot succeed while a native function owns a lease. Native nested calls are bounded at 2,048 leases.

`KHZ_ServerPhysicsFenceIdle(frame, reason, capacity)` checks closed/exclusive enrollment and the exact saved fixture/session/map/hunk/client identity before each of the eight capture and validate callbacks. Each commit calls `KHZ_ServerPhysicsRequireCommitFence(frame)` before writing any native bytes. This contract check remains active in Release and is fatal if the coordinator invokes commit without its verified fence. It deliberately does not introduce a new fallible step midway through otherwise validated commit.

The observer holds this fence through capture, pinned branch work, every validation/restore and all reverse cleanup. A provider's discard never releases it. Only after the coordinator is Empty and every provider has discarded may the observer call `KHZ_ServerPhysicsFenceRelease(reason, capacity)`. A failed pre-capture attempt that closed the gate also needs this explicit cleanup/release. Cleanup metadata clears while admission is still closed; a foreign refusal racing after that clear remains recorded when admission reopens.

The orchestration order is: verify actual host/map/pause identity; close Physics (and other native exclusive fences); verify every fence is idle; invoke the coordinator's all-provider capture/validate/restore operation; preserve fences while pinned; invoke all-provider discard; verify coordinator Empty; release Physics and the other fences; then permit asset release or ordinary owner resume. If the observer or any provider rejects, perform controlled discard/Empty cleanup before releasing the closed gates. Never release Physics from its part-0 discard while another provider still holds an image.

All enrolled native functions hold `KHZ_ServerPhysicsEntry` from their first statement through function return/unwind. The lease macro is intentionally declared in function scope; a temporary `do/while` block would release it before the native effects. Foreign entry checks use only atomic owner/gate/counter data, refuse before effects and publish a bounded first-reason code. Foreign code never reads `KHZ_ServerCheckpointPinned()` or calls the main-owned rejection API. The real owner consumes the atomic reason with `KHZ_ServerPhysicsDrainGuardRefusals`; a pre-capture reason remains pending until the observer rejects/cleans up, while a pinned reason latches branch ineligibility without discarding the saved baseline.

## Eligibility and topology checks

- `physInited` and the 38 exact native dvar bindings must retain their original identities. Dvars restores their native values; the physics provider never writes through dvar nodes.
- Initialized world/space/contact roots must equal their exact `odeGlob` addresses; disabled roots must be NULL. A retained shutdown `worldGeom` pointer may only address the original static world geom.
- World body/joint counts and roots, space counts/roots/current geom/lock count and contact-group active count must all be zero. Spaces must retain their native simple-space type and original executable virtual-table identity. Geometry body/position/rotation/list/parent/data pointers must be NULL, with no placeable world/space object.
- Userdata, body and geom free chains are bounded walks over exact stride-aligned native slot addresses. Every slot is visited exactly once and active counts are zero. Repeated, foreign, misaligned, shortened or cyclic chains reject. Dormant uninitialized pools are accepted only when their original storage is entirely BSS-zero and their head/active count are zero.
- Persistent hinge/ball/motor arrays must have a complete unique index free chain ending at `-1`. Their pre-init BSS-zero dormant form is distinct from initialized free chains and is accepted only while disabled. Inactive payload bytes are copied; they are never treated as live graph pointers.
- Trimesh vertex/index buffers, `triMeshDataID`, debug `visTrisGeom` and active debug object counts must be absent. No heap/vector/OS ownership is silently copied.
- Collision callback contexts are rejected. Registry callbacks must address this executable's image, and the full original registry, class set and dispatch identity must remain unchanged before restore. Active native custom classes are either none or the stock five; custom plugin classes are rejected.
- Native solver/gravity/jitter metadata must be finite; native `dInfinity` is `FLT_MAX` and is valid. Capsule scratch count is bounded by 32 and its retained active records are validated. The actual native step arrays must remain positive and ordered.
- Error handlers must be the real stock default NULL set. A custom handler's external mutable context has no provider in this scope.
- ODE debug timer use is rejected. With `TIMING` disabled in this build, its registry is unused and BSS-zero. A timer start leaves `num` nonzero, also exposing its function-static slot initializer. Its fixed lazy QPC frequency calibration is prewarmed outside capture; it is not a gameplay clock, deadline or replayed wall time.

## Required guards before branch effects

Native guards are enrolled at **246** concrete compiled entry sites across fifteen `.cpp` files and the original `collision_kernel.h` inline helpers. Main owner calls can enter while the fence is open. Closed-fence or foreign calls return a typed failure before allocation, registration, callbacks, placement new, destruction or I/O and record fixture ineligibility. A return after `Pool_Alloc` or `dCreateGeomClass` is too late. Primary native factories and `ReInit` methods guard before constructor/placement-new effects; arbitrary external raw pointer writes or direct placement construction remain outside this API contract and are excluded with plugins.

`KHZ_ServerPhysicsAllowUnsupportedMutation(operation)` remains a compatibility helper for direct owner control guards. It refuses unknown foreign access and closed-fence effects, but provides no full-function lease by itself. Production native entry hooks use the scoped macros instead.

The catalog covers these actual compiled entries and indirect paths:

- `Phys_Init` / `Phys_Shutdown`, `ODE_Init`, `dWorldCreate` / `dWorldDestroy`, `dGetSimpleSpace` / `dGetContactJointGroup`, `dCloseODE` and custom geometry-class initializers;
- native Phys object create/add-geom/destroy, userdata allocation/free, all persistent joint creation/destruction; `dBodyCreate` / `dBodyDestroy`, `createJointInPlace` / `ODE_InitJoint`, `dJointCreateContact`, `dJointGroupCreate` / destroy, and direct joint-group mutation;
- `ODE_AllocateGeom` / create / free / destruct, `dInitUserGeom`, geometry constructors/reinitialization, standalone/hash/quadtree space creation, membership/lifetime changes and heap-backed debug visible-triangle creation;
- `Phys_RunToTime` / `Phys_RunFrame`, `dxProcessIslands` / `dxQuickStepper`, `dWorldQuickStep`, `dSpaceCollide` / `dSpaceCollide2`, `dCollide` and `ODE_CollideSimpleSpaceWithGeomNoAABBTest`, before either callback or lazy dispatch changes;
- `Phys_SetCollisionCallback`, `dCreateGeomClass`, native error-handler setters, and `dError` / `dDebug` / `dMessage` before dialog/abort/report effects;
- `dTimerStart` / `dTimerNow` / `dTimerEnd` / `dTimerReport` before their local initializer, pointer assignment or external report.

It also covers native body/geom/world/joint setters, solver helpers, capsule scratch mutators, AABB recomputation and inline space/body linked-list helpers. `dxSpace::getGeom` and `dSpaceGetGeom` are included because enumeration writes the native `current_geom/current_index` cache. Guarding only obvious setters would leave those writes unprotected.

Native `dError` and `dDebug` use a fatal guard rather than returning to an assertion caller. The real owner reports `ERR_FATAL`; a foreign fatal call aborts without touching main-owned engine/coordinator state. This is explicit fatal fixture failure, never permissive assertion continuation or replay parity. `dMessage` can return before its reporting effect after recording refusal. Factory/step guards should make these fatal paths unreachable in the supported empty branch.

`KHZ_ServerPhysicsGuardStatsJSON()` is actual-owner-only and reports `compiled_native_hooks`, required/enrolled parts, `guard_entries`, total checks/refusals, foreign/unknown refusals, pending reason, active lease count, exclusive fence state, closure/release counts and the complete per-site counts. All counters are atomic and deliberately **not restored**, so repeated restore cannot erase refusal evidence. Known site IDs and names are fixed by `khz_server_physics_guards.h`; unknown/control misuse has its own refusal count.

The read-only `scripts/physics_guard_inventory.py` checks the reviewed 246-entry source catalog against Windows dedicated opt-in branches, verifies each guard is the first function statement, every source is in the native build inventory, each part's EOF opt-in include/enrollment/callback fence checks and default-disabled macros. It emits exact source SHA-256 values and explicit zero native/world restore attestations. Dormant `#if0` `dJointSetFeedback` is excluded from the active catalog. It does not invoke a compiler or game.

The final 246-entry source audit passes in `results/physics-guard-inventory68.json`. `scripts/compile_checkpoint_staging.py` adds all three Collision parts, the source-owned host/hunk observer and every touched native opt-in TU; it avoids including an inl twice when the actual native source already includes it. Its macro-OFF/ON results are in `results/checkpoint-staging-off68.log`, `results/checkpoint-staging-on68.log` and `build/checkpoint-staging[-hooks]/staging-result.json`. It checks actual COFF strong external definitions, including bigobj/COMDAT selection rules, but performs no full engine executable link. These artifacts have explicit zero native restoration/full-world replay attestations.

The initial branch may restore bounded scalar/scratch changes whose original storage is captured. It does not permit creation and destruction of dynamic ODE objects during a branch merely because the graph is empty again afterward. Original C++ object lifetime and virtual-table identity are mandatory. Preset/model/BSP assets remain pinned by the real map/hunk observer and other mandatory asset-domain providers; this empty graph retains no body/geom preset or model pointer to a missing asset range.

## Compiled source inventory and remaining limitations

`engine/scripts/common_files.cmake` excludes legacy `ode/odemisc.cpp`, `ode/memory.cpp`, `ode/stepfast.cpp`, `ode/testing.cpp`, the old variable-array/obstack implementation and old matrix allocators. In addition, `deps/ode/memory.h` places the allocator-handler declarations under `#if0`, and `ode.cpp` places its legacy `dRand` test calls under `#if0`. No dormant fake global `dRand` seed or memory-handler state is introduced. The actual compiled solver RNG consists of each `world.stepInfo.holdrand` plus `g_holdrand`, restored by parts 1/6.

`USE_POOL_ALLOCATOR` is actually defined in `deps/ode/common.h`; the native body/geom pools are captured. Direct `new` still exists for unsupported external groups/spaces, and those paths must refuse before effects. `dTRIMESH_ENABLED` is disabled in `deps/ode/config.h`. The retained old global scratch at the end of `collision_trimesh_box.cpp` is referenced only by `#if0` routines; the compiled native triangle-box routine uses a local `collData_t`. Capsule scratch is real global mutable storage and is captured by part 4.

Supporting nonempty ODE physics would require active object-type-aware lifetime tracking, exact body/geom/joint graph validation, immutable preset/model/brush pins, supported callback contexts and any extra heap allocator providers. It cannot be enabled by relaxing empty checks or increasing a range to the whole hunk. Native standalone compilation is only a syntax/layout check. Live paused capture, repeated restore, guarded unsupported-effect rejection and replayed actual world/script/input tests remain required before any full-world replay gate can pass.
