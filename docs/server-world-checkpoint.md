# Native World checkpoint staging

The eight source-owned fragments are staged but are not included by native
sources or registered at startup yet. No engine build or native restoration
test has been run for these fragments. They never attest full-world replay.

Every registration strengthens the World requirement to `0xff`, so omitting
any part prevents capture and restore. Include each fragment at the end of the
listed source under `COD4X1KHZ && KISAK_MP`, then register all eight on the
dedicated main owner before the first checkpoint capture.

| Part | Native source | New fragment / registration | Actual live storage |
| --- | --- | --- | --- |
| 0 | `game_mp/g_main_mp.cpp` | `khz_server_world_main_checkpoint.inl` / `KHZ_ServerWorldMainRegisterCheckpoint` | `level`, `level_bgs`, `g_entities[1024]`, `g_clients[64]`, native use-hold tuning |
| 1 | `game_mp/g_scr_main_mp.cpp` | `khz_server_world_script_checkpoint.inl` / `KHZ_ServerWorldScriptRegisterCheckpoint` | `g_scr_data`, eight corpse records, native script/bootstrap/scenario handles |
| 2 | `game/g_hudelem.cpp` | `khz_server_world_hud_checkpoint.inl` / `KHZ_ServerWorldHudRegisterCheckpoint` | `g_hudelems[1024]`, native HUD comparison record |
| 3 | `game_mp/g_misc_mp.cpp` | `khz_server_world_turrets_checkpoint.inl` / `KHZ_ServerWorldTurretsRegisterCheckpoint` | `turretInfo[32]` |
| 4 | `game_mp/g_vehicles_mp.cpp` | `khz_server_world_vehicles_checkpoint.inl` / `KHZ_ServerWorldVehiclesRegisterCheckpoint` | `s_vehicles[8]`, `vehEffects[1][8]` counters |
| 5 | `game/g_missile.cpp` | `khz_server_world_missiles_checkpoint.inl` / `KHZ_ServerWorldMissilesRegisterCheckpoint` | `attrGlob.attractors[32]`, both native javelin offsets |
| 6 | `game/g_mover.cpp` | `khz_server_world_movers_checkpoint.inl` / `KHZ_ServerWorldMoversRegisterCheckpoint` | `pushed[1024]`, actual `pushed_p` cursor |
| 7 | `game/g_scr_vehicle.cpp` | `khz_server_world_vehicle_physics_checkpoint.inl` / `KHZ_ServerWorldVehiclePhysicsRegisterCheckpoint` | `s_vehicleInfos[32]`, definition count, actual `s_phys` collision trace and `s_backup`, wheel/flash tag pointer arrays |

The SP-only `g_vehicle_path.cpp` node pool is excluded from this MP scope.
The vehicle records already contain the MP path-position bytes. Network
`svs.clients` belongs to the Network provider; it is the coordinator Frame's
`clientsAddress`. World registers its distinct actual `g_clients` storage.

Capture and every validation require a real paused dedicated-owner callback,
idle native scripts, completed game tick (`currentEntityThink == -1`, no
initialization/spawn, `bgs == nullptr`), matching native level time and original
entity arena. Images use fixed raw BSS bytes without large constructor-generated
temporaries. Commit consists only of bounded copies/scalar assignments: no
entity spawn/free, refcount change, XAnim callback, allocation, logging or I/O.

Entity/free-list/tag-child pointers must address actual native arrays with
element alignment and bounded reciprocal topology. A tag record must start at
an original native MT node and fit entirely inside the concrete restored MT
payload pool. Client/corpse mutable XAnim trees require actual Animation
provider ranges; a broad original hunk address is insufficient. Lerp-frame
animation pointers and script-item pointers must address their exact original
BGS arrays. Loaded BGS definitions, asset pointers and callback bindings retain
their original pinned identities throughout the fixture. The vehicle trace's
material pointer must identify an actual bounded name in the original pinned
BSP material array; model-specific material pointers are currently unsupported.

The first scope rejects an open game log, script file/buffer/parse cursor,
active dedicated-client vehicle FX, unavailable animation restore spans,
invalid native counters/topology and any changed map/arena/asset lifetime.
The coordinator still requires all other domains, including actual collision
links, physics state, RNG, script scheduling, runtime/network histories and
administration. Nonempty ODE, debugger state, plugins, external humans and
uncaptured external effects remain independently ineligible. Before replay
branches, the native integration must guard file I/O, map/asset teardown,
admission and resource loads before they happen and latch branch rejection;
copying World bytes alone cannot reverse external effects.
