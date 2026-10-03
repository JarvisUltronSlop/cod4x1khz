# All fourteen native server checkpoint domains: integration checklist

This is a staged integration plan, not an enabled runtime feature. Default
Build68/69 server checkpoint opt-in is off. OFF/ON staging has compiled 54 native
translation units and checked COFF definitions, but has not linked a full game,
registered live providers, restored an authoritative world, or completed a
renderer-cap replay comparison. The already-tested private-client roundtrip and
actual consumed-input tapes remain separate evidence.

## Link an isolated opt-in dedicated build first

Keep the normal deployed target unchanged until the whole integration is
reviewable. The dedicated-only target must define
`COD4X1KHZ_SERVER_CHECKPOINT=1` consistently for every native TU and guard header,
alongside `COD4X1KHZ`, `KISAK_MP`, and `KISAK_DEDICATED`. A definition on only a
helper file is not hook enrollment. Use the existing Win32 Release dependencies,
`khz_core` include paths/linking, and FP policy.

Root `CMakeLists.txt` currently lists neither
`engine/src/khz/khz_server_checkpoint.cpp` nor
`engine/src/khz/khz_server_network_checkpoint.cpp` for the game target. Link both
in the opt-in dedicated build. Do not compile an `.inl` independently: it needs
its actual native owner TU's private state. The already-compiled native
`universal/com_math.cpp` supplies the reviewed gameplay RNG get/set functions.
Schedule and RNG registrations both live in the runtime schedule fragment.

Perform a real full link, including the native imports/exports. The staging
object library's no-duplicate-symbol check cannot establish that all externs
resolve or that the resulting process starts. Archive sources and binaries only
after the actual opt-in link, startup, and disabled-feature checks pass.

## Exact native EOF fragments and ownership

Each include belongs at the end of the matching original TU, under the common
dedicated opt-in guard. Existing guarded EOF includes must be retained once,
not appended twice. `scripts/compile_checkpoint_staging.py` has the same owner
inventory, including the separate native files that contain pre-effect guards.

| Domain / part | Native owner relative to `engine/src/` | Fragment in `khz/` |
|---|---|---|
| ScriptVM 0 | `script/scr_vm.cpp` | `khz_server_vm_checkpoint.inl` |
| ScriptVM 1 | `script/scr_variable.cpp` | `khz_server_variables_checkpoint.inl` |
| ScriptAllocators 0 | `script/scr_memorytree.cpp` | `khz_server_mt_checkpoint.inl` |
| ScriptAllocators 1 | `script/scr_stringlist.cpp` | `khz_server_sl_checkpoint.inl` |
| Graphs 0 | `game/enthandle.cpp` | `khz_server_enthandles_checkpoint.inl` |
| Graphs 1 | `qcommon/dobj_management.cpp` | `khz_server_dobj_checkpoint.inl` |
| World 0 | `game_mp/g_main_mp.cpp` | `khz_server_world_main_checkpoint.inl` |
| World 1 | `game_mp/g_scr_main_mp.cpp` | `khz_server_world_script_checkpoint.inl` |
| World 2 | `game/g_hudelem.cpp` | `khz_server_world_hud_checkpoint.inl` |
| World 3 | `game_mp/g_misc_mp.cpp` | `khz_server_world_turrets_checkpoint.inl` |
| World 4 | `game_mp/g_vehicles_mp.cpp` | `khz_server_world_vehicles_checkpoint.inl` |
| World 5 | `game/g_missile.cpp` | `khz_server_world_missiles_checkpoint.inl` |
| World 6 | `game/g_mover.cpp` | `khz_server_world_movers_checkpoint.inl` |
| World 7 | `game/g_scr_vehicle.cpp` | `khz_server_world_vehicle_physics_checkpoint.inl` |
| Schedule 0 + RNG 0 | `khz/khz_runtime.cpp` | `khz_server_schedule_checkpoint.inl` |
| Network 0 | standalone native adapter | `khz_server_network_checkpoint.cpp` |
| Network 1 | `server_mp/sv_snapshot_mp.cpp` | `khz_server_snapshot_checkpoint.inl` |
| Network 2 | `qcommon/net_chan_mp.cpp` | `khz_server_transport_checkpoint.inl` |
| Runtime 0 | `khz/khz_runtime.cpp` | `khz_server_runtime_checkpoint.inl` |
| Bots 0 | `khz/khz_bots.cpp` | `khz_bots_checkpoint.inl` |
| Admin 0 | `khz/khz_admin_server.cpp` | `khz_server_admin_checkpoint.inl` |
| Animation 0 | `xanim/xanim.cpp` | `khz_server_animation_checkpoint.inl` |
| Collision 0 | `qcommon/cm_load.cpp` | `khz_server_collision_load_checkpoint.inl` |
| Collision 1 | `qcommon/cm_world.cpp` | `khz_server_collision_world_checkpoint.inl` |
| Collision 2 | `server/sv_world.cpp` | `khz_server_collision_query_checkpoint.inl` |
| Physics 0 | `physics/phys_ode.cpp` | `khz_server_physics_checkpoint.inl` |
| Physics 1 | `physics/ode/ode.cpp` | `khz_server_ode_checkpoint.inl` |
| Physics 2 | `physics/ode/collision_kernel.cpp` | `khz_server_ode_colliders_checkpoint.inl` |
| Physics 3 | `physics/ode/timer.cpp` | `khz_server_ode_timers_checkpoint.inl` |
| Physics 4 | `physics/phys_coll_capsulebrush.cpp` | `khz_server_physics_capsule_checkpoint.inl` |
| Physics 5 | `physics/ode/error.cpp` | `khz_server_ode_errors_checkpoint.inl` |
| Physics 6 | `physics/ode/quickstep.cpp` | `khz_server_ode_quickstep_checkpoint.inl` |
| Physics 7 | `physics/phys_contacts.cpp` | `khz_server_physics_steps_checkpoint.inl` |
| Dvars 0 | `universal/dvar.cpp` | `khz_server_dvars_checkpoint.inl` |
| Authenticated owner / orchestration | `khz/khz_host.cpp` | `khz_server_checkpoint_owner.inl` |
| Original hunk lifetime observer / guard | `universal/com_memory.cpp` | `khz_server_checkpoint_hunk.inl` |

There are fourteen domains, thirty-five concrete provider parts, and two
observer-only fragments. The initial coordinator minimum is strengthened by the
native registrations: World `0xff`, Network `0x7`, Collision `0x7`, Physics
`0xff`, Graphs/ScriptVM/ScriptAllocators each `0x3`. Other domains require their
part0. Omitted registrations must reject capture. Do not weaken those masks,
mark a provider complete from a hash, or register an arena whose mutable bytes
will not actually be restored.

## Initialize, bind a completed map, and preserve its lifetime

Call `KHZ_ServerCheckpointRegisterNativeOwner()` once from dedicated owner
initialization after the native subsystems needed for static range enrollment
exist and before capture. It registers the exact native provider callbacks and
all static ranges, plus Collision hook enrollment. Physical source enrollment
must report all eight Physics parts (`0xff`) and the actual Collision required
hooks; a macro-off hook cannot count as an enrolled one.

After actual map load, `G_LoadAnimTreeInstances`, and authenticated host Ready,
call `KHZ_ServerCheckpointBindNativeMap(reason,capacity)` on the completed owner.
It requires the unpinned coordinator and releases only already-empty fences,
resets old map ranges, then binds exact native Animation trees and Collision
ranges. Animation is the actual seventy-two tree allocations, not a broad hunk
span. The hunk observer records original base/low/high use. Root's `Ranges`
capacity256 is metadata capacity, not permission to capture unknown memory.

Map generation, server id, BSP checksum, original server/client/entity storage,
script program, MT buffer, and hunk use remain fixed while pinned. Failed binds
retire the partial map ranges and refuse capture. Discard every provider before
map teardown/rebind, asset reload, or shutdown. A same-map restart also changes
native session/lifetime metadata; it cannot silently keep an old saved image.

## Actual pause and private controls

Use the existing verified current-user pipe/token/job relationship. The client
first parks its real gameplay owner and Raw Input producer, receives the actual
producer acknowledgement, and drains UDP/loopback queues through the existing
transport fence. Server Pause must validate the real matching fixture/session,
one active owned local human, no active native fragments/downloads/voice,
no fake-lag queues, no loaded plugin/webadmin, and original loaded map.

Add explicit private owner-dispatched operations for capture, validate,
restore, discard and status. Route them through actual HostPoll in the paused
owner loop; a pipe worker must never call provider code or mutate gameplay.
Only serve a success acknowledgement after the operation has actually returned
and its masks/reason have been copied. Do not unpause while a restore or failed
cleanup is pending. These are plumbing requirements, not currently available
HostMessage commands.

Use the exact wrappers:

1. `KHZ_ServerNativeCheckpointCapture(fixture)` observes the authenticated
   paused owner and closes the **Physics gate before any provider**. Collision
   closes next. Both must show zero active foreign/native entries for the same
   actual fence. All capture callbacks then run inside the coordinator context.
2. `KHZ_ServerNativeCheckpointValidateRestore()` observes the same lifetime and
   validates **every** required provider before any restore callback writes.
   A missing provider or validation rejection must leave native gameplay bytes
   unchanged. Unsupported branch effects latch ineligibility without discarding
   the only saved baseline.
3. `KHZ_ServerNativeCheckpointRestore()` revalidates and restores actual arenas,
   graphs, VM/allocators, schedules, RNG, network/runtime, bot/admin state,
   animation, collision, empty Physics metadata, and numeric Dvars in the
   coordinator's native ordering. No fresh C++ ownership construction, script
   execution, or pointer-file deserialization belongs in commit.
4. `KHZ_ServerNativeCheckpointDiscard()` discards every provider, reaches real
   coordinator Empty, then releases Collision and Physics in the wrapper's
   documented order. A refused capture uses explicit Empty cleanup too. No
   individual provider discard may reopen a fence early.
5. While still authenticated and paused, call
   `KHZ_ServerCheckpointPrepareNativeResume(reason,capacity)` to rebase the
   runtime pacing epoch without changing restored logical time. Preserve fences
   across a pinned branch. Ordinary game stalls continue to retain scheduler
   debt; they do not use this explicit diagnostic halt bridge.

During pause run only private control/discard/drain service. No gameplay ticks,
sleeping scripts, admin/plugin services, Cbuf commands or ordinary packet
admission may run behind the observer's idle check. During a replay branch,
socket sending/admission must be coordinated with the client and rejected
external packets discarded before native mutation.

## Pre-effect guards required before enabling capture

Provider images cannot undo disk, OS handles, unowned heap allocations or asset
destruction. Guard these paths before their first mutation, not merely when
validation notices that an effect already happened.

| Actual entry family | Required gate / refusal policy |
|---|---|
| Seven actual bot script methods `fs_fopen`, `fs_fclose`, `fs_fcloseall`, `fs_readline`, `fs_writeline`, `fs_testfile`, `fs_remove`, plus path creation/open-handle helpers in `khz_bots.cpp` | `KHZ_BotCheckpointScriptFileAccessAllowed(operation)` before fopen/engine open/close/read/write/remove/create path; return the actual typed failure and latch branch rejection. Include every exposed method, not just the writing ones. |
| Native `GScr_OpenFile`, `GScr_CloseFile`, `GScr_FPrintln`, `GScr_FPrintFields`, `GScr_FReadLn`, `GScr_FGetArg` in `game_mp/g_scr_main_mp.cpp`, and `G_LogPrintf` in `g_main_mp.cpp` | First capture requires `level.logFile==0` and no script FILE/parse cursors. Refuse later open/write/close/parse side effects before mutation; returning an arbitrary success value is not a valid guard. |
| Bot/native client admission, bot add/remove, connect/drop and map rotation/restart | First scope fixes admission topology/generation. Refuse before changing client state, VM pers arrays, freeing entities or native control generations. An ordinary respawn is distinct from an admission change. |
| Admin persistence, `flushAudit`, ban-store load/save, plugin-ban/proof/admission callbacks and admin command services | `KHZ_ServerAdminAllowExternalEffect(reason)` before disk/policy/admission effects; drain queues before capture, disable background/external service work while fenced. Numeric counters in the image do not restore external files or remote messages. |
| `Dvar_SetVariant`, string reset/latched mutation, register/reregister/shutdown in `universal/dvar.cpp` | `KHZ_ServerDvarsAllowValueMutation(node,value)` / `KHZ_ServerDvarsAllowTopologyMutation()` before Copy/Free/Register changes. Only an already-equal three-way current/latched/reset string alias is a supported no-op. Numeric state is restored by the actual Dvar owner. |
| Hunk clear/free/init/shutdown and server DB/map/XAnim/DObj asset lifetimes | `KHZ_ServerCheckpointAllowHunkLifetimeChange(operation)` plus exact map/tree lifetime refusal before freeing/replacing storage. No whole-hunk range or new heap vector ownership may be declared restored. |
| Actual ODE class/factory/body/geom/joint/allocation/step entries | The opt-in native full-function Physics guard leases and 246 active catalog sites must all be linked. Unknown/foreign entries refuse atomically before effects; main consumes the latched refusal. Preserve native fatal behavior for unreachable fatal/assertion paths. |
| BSP/brush/mover transforms, entity deletion, collision debug builders and DynEntity operations | Actual opt-in Collision guards in their native owners, including brush trajectory setters and G_FreeEntity/Delay. Active unsupported topology/transforms must refuse before mutation; arbitrary static-BSP approximation is not restoration. |
| Plugins, web/download/screenshot services, filesystem/update/network callbacks | Exclude them in the first capture and reject starting them while pinned. Their socket/job/file state is not a provider in this first scope. |

The Physics image is the actual initialized disabled/empty ODE graph only,
including constructed globals/collider callbacks. It does not support active
bodies, joints, custom classes, heap vectors or dynamic world restoration. The
World provider separately covers the original native vehicle pools and rejects
unrestored file/pointer state. Unsupported scope must remain an explicit capture
or branch rejection.

The staged `KHZ_ServerAnimationAllowAssetMutation` and
`KHZ_ServerAnimationAllowTreeMutation` currently return true for foreign-thread
calls. Before activation, prove those dedicated native lifetime mutations are
main-only, or introduce source-owned atomic lifetime admission covering every
possible foreign caller. A later fingerprint mismatch detects damage; it does
not undo freed assets or make a concurrent copy safe. Apply the same review to
hunk/DB lifetime entrypoints. Physics/Collision admission fences are not a
blanket fence for unrelated asset allocators.

## Verification order after the timing batch

1. Finish EOF/CMake/guard plumbing as one isolated opt-in source checkpoint.
   Compile and **link** it, verify startup and actual provider/part/hook status.
   Keep ordinary build default off; do not register an incomplete shortcut.
2. Run one actual private Killhouse immediate server capture/validate/restore
   roundtrip while both peers are fenced. Require all fourteen domain masks and
   all native part masks, exact identities, zero unsupported effects, and
   advancing admitted input/snapshots/prediction/GPU after orderly resume.
3. Exercise negative lifetime/provider/fragment/open-file/dynamic-ODE cases
   using actual guards. A rejected validation writes no gameplay data; cleanup
   must discard all images and release both real fences. Preserve the original
   saved image when a branch is rejected.
4. Advance a short controlled branch, pause/drain again and restore **both**
   authoritative and private-client images in the same pinned map/session. This
   is a separate live proof from immediate restore and consumed-input capture.
5. Feed the actual recorded authoritative consumption timeline after plugin
   selection together with deterministic normalized client input receipts. Keep
   native sequences/history, scripts, RNG and clocks aligned. Hardware producer
   remains parked during replay; rebase only the recorded receipt QPC epoch.
6. Compare actual named canonical world/VM/RNG/client domains at fixed logical
   checkpoints under real 125/240/1000 render caps, while rendering actually
   advances. A command tape, snapshot hash or successful client-only roundtrip
   does not establish restored script/world state. Mark full-world replay passed
   only after actual branch restore and identical authoritative outcomes.

No test, hash, compile, game launch, asset deployment or production-source edit
was performed while preparing this checklist during the Build69 timing batch.
