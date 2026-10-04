# Native Vulkan development status

The `vulkan-testing` branch now contains a native Vulkan 1.3 backend. It is still
experimental. The accepted root and runtime clients retain the Build 86 DX9
binary, SHA-256 `fa9baca2076ec15275ef2474a9587497a32aa3ec5c0310def49ec056459db577`.
Release `v0.1` and its private workspace archives remain intact.

The client and server now have native **x64 / PE32+** builds alongside the
existing Win32 build. Native packaging uses `runtime/x64`. Gameplay, prediction, input
and networking retain their existing independent 1 ms owners. These renderer
changes do not establish that all gameplay or public CoD4x acceptance gates pass.

## Implemented and measured

- Project-local checksum-pinned Vulkan-Headers 1.3.296, VMA 3.2.1,
  SPIRV-Tools 2025.1 and MojoShader `ad5dff84830c2863c841f4b1f4e3df78c705b383`.
  Optional Win32 validation layers are built from pinned source. Dependency
  licenses are in `third-party-vulkan/`.
- A neutral graphics boundary, bounded generation-checked resource tables,
  immutable legacy draw packets, native resources, linked material pipelines,
  caches, descriptors, upload arenas and GPU queries.
- One graphics submission owner; synchronization2, timeline retirement, dynamic
  rendering, one or two frames in flight, and presentation semaphores retired
  independently from frame resources. Normal frames avoid whole-device waits.
- Automatic compatible discrete-GPU selection. The actual Win32 probe selected
  the RX 9070 XT, reported Vulkan 1.4.349 and about 16 GiB of device-local memory.
  VSync-off tests selected Immediate presentation.
- All **1,066** unique shaders discovered across **21 installed stock multiplayer
  zones** translate, and **16,802** vertex/pixel/declaration combinations link and
  validate as Vulkan SPIR-V. This is translation coverage, not visual coverage.
- Actual GPU fixtures cover legacy/SM3 interfaces, alpha tests, blending,
  depth/stencil, separate and partial clears, texture swizzles, MSAA resolve,
  dynamic geometry lifetimes, queries, presentation gamma, copies, scaled
  blits, normalized depth bias and hardware shadow comparisons (including
  projection and filtered edges), R32F channel defaults, packed particle
  inputs, order-sensitive parallel recording and divergent alpha-discard/slope-bias interaction. Latest **108 matched image pairs** have SSIM
  **1.0**, zero differing color channels and zero
  validation/synchronization errors on the RX 9070 XT.
- An actual stock-interface GPU regression exercises native resource locks,
  immutable constants, query progress, lossless readback and dirty 2D/cubemap/
  volume system-memory texture transfers against DX9. Idle
  event queries return ready, matching the loading barrier's DX9 behavior.
- An owned, isolated startup diagnostic has captured the stock frontend through
  native Vulkan. The experimental Win32 client uses `/LARGEADDRESSAWARE`; this
  expands available virtual address space on 64-bit Windows while preserving
  32-bit pointers and stock asset layouts.
- Dynamic geometry uses bounded append-only `NOOVERWRITE` uploads while
  retaining earlier draw bytes. Failed recordings cannot create an unsubmitted
  timeline value that shutdown waits for. Reduced-resolution passes can retain
  a larger depth surface, as stock DX9 does. Separate depth-only sampled views
  and shader comparison instructions preserve stock hardware shadows.
- On this Win32 driver, optional `VK_EXT_depth_bias_control` representations are
  unavailable. The renderer applies stock normalized bias in a validated
  fragment-shader fallback. Positive/negative bias, slope, alpha testing and
  explicit pixel-depth overrides match DX9 fixtures; its performance still
  needs profiling.
- Consecutive draws retain attachment scopes, pin each resource generation once
  per batch, capture only changed register blocks, reuse compatible descriptors
  and omit redundant Vulkan binding commands. Immutable constant reuse is
  opt-in for versioned packet storage; ordinary span callers remain mutable.
- Read-only GPU query polling uses the facade's lifetime/submission mutex and
  does not transfer submission ownership or wait for unrelated work. Resource
  mutations and submissions retain their acknowledged owner barriers.
- Pipeline and descriptor lookups use bounded indexes with full key/generation
  comparisons. Per-flight descriptor pools persist across completed frames;
  stale shader/image generations are pruned after GPU completion. Existing-asset
  frontend uploads join immutable submissions without transferring GPU ownership;
  owner-thread zone uploads remain immediate to avoid accumulating whole maps.
- The existing renderer worker pool now supports up to four workers with separate
  TLS/collision scratch. Large prepared draw batches use independent per-flight
  secondary command pools; small batches and queries remain inline. The submission
  owner prepares resources and preserves draw order. Parallel resource/material
  preparation remains pending; four recording workers alone did not improve the
  checked view's throughput beyond run variability.
- Benchmark-only timing storage records individual presentations and every
  ordered upload/draw/readback submission. GPU timestamps join to their original
  serials. Export drains the last one/two timestamp pairs at an explicit diagnostic
  barrier; ordinary frames do not wait for this export. The real-GPU regression
  retains all 24 displayed frames and 97 submissions with complete GPU coverage.
- The loaded-world particle defect was traced to slope derivatives evaluated
  after divergent `texkill`. A GPU checker-mask regression failed before the
  fix (178 maximum channel error) and passes afterward. Rasterizer bias is now
  calculated in the entry block before discard. This follows the derivative
  requirements in the [SPIR-V specification](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html).
- Exact stock resource/draw mirroring into an offscreen DX9 reference produced
  SSIM **0.99998268** on Killhouse at 480p and **0.99998420** on Crash at 720p.
  Captures are under `results/vulkan-world-20261004-044755` and
  `results/vulkan-world-20261004-045038`. Remaining differences were small edge
  and rounding differences; this does not qualify other maps/effects/options.
  With four recording workers, a later checked Killhouse capture
  (`results/vulkan-world-20261004-053243`) reached SSIM **0.99998354** and zero
  validation errors. Comparison mirroring has substantial overhead and cannot
  serve as a throughput measurement.

Archived settings are `r_graphicsApi auto|vulkan|d3d9`, `r_vkDevice -1` or an
explicit device index, and `r_vkFramesInFlight 1|2` (default 2). `auto` selects DX9
until correctness and matched performance gates pass. `com_maxfps 0` remains
uncapped, `com_maxfps 1000` caps rendering, and `r_vsync` retains its stock role.
`r_vkValidation` is a development diagnostic. Unsupported operations report
explicit errors; they are not accepted as implemented features.
`r_vkCompare` is an isolated benchmark-only image comparison, with substantial
overhead. `r_vkStateCache`, `r_vkDescriptorCache` and `r_vkRecordingWorkers 1..4`
are development A/B controls, not archived stock settings. `r_vkProfile` enables
bounded timing capture only in isolated benchmark/test profiles; `r_vkWriteProfile`
exports it outside measurement. Disable validation and comparison for throughput
measurements. The world probe always checks camera clearance and position; its
`--capture` option adds a lossless image after measurement.
It also retains pre-launch/post-start/post-quit system and Steam memory snapshots,
plus client/server CPU, private memory and WDDM GPU engine samples. GPU busy time
is reported as a diagnostic and is not equated to shader throughput.

## Development builds and diagnostics

Run from the project root after preparing Steam assets and the DirectX dependency:

```powershell
./scripts/build-vulkan-tools.ps1 -Validation -GpuTest
python ./scripts/compare_graphics_fixtures.py ./results/vulkan-gpu-tools
cmake -S . -B build/Release -G 'Visual Studio 17 2022' -A Win32 `
  -DCOD4X1KHZ_BUILD_ENGINE=ON -DCOD4X1KHZ_BUILD_VULKAN=ON
cmake --build build/Release --config Release --target KisakCOD-mp khz_launch_probe --parallel 5
python ./scripts/native_vulkan_startup_probe.py
python ./scripts/native_vulkan_world_probe.py --no-validation --compare --capture --samples 2 --map mp_crash --resolution 720p
python ./scripts/compare_native_world.py ./results/vulkan-world-<capture-id>
python ./scripts/native_vulkan_world_probe.py --no-validation --samples 8 --cap 0 --recording-workers 4
python ./scripts/memory_snapshot.py --output ./results/memory-before-game.json
python ./scripts/native_render_scene_probe.py --experimental --api vulkan --map mp_killhouse --resolution 480p --timing-seconds 60
```

Explicit engine targets build `bin/Release/cod4x1khz.exe` without deploying it over
the accepted executable. The startup probe temporarily stages that experimental
binary at its recognized runtime path, owns and closes its process, uses a
separate profile/home and restores the original runtime binary. Ordinary
`build.ps1` includes deployment; use the explicit targets during development.

## Remaining delivery gates

Loaded-world material/effect coverage, stock options, cinematics, restarts and
map lifetimes still need native validation. Parallel preparation and matched
performance improvement remain open. Stock image comparison must reach SSIM 0.995
without missing geometry or effects; fixture parity cannot substitute for it.
Earlier match-start failures exposed idle event-query behavior, dynamic upload
capacity, dirty volume transfers, depth bias and shadow image-view/comparison
requirements. Their diagnostic results remain private under `results/`.
The earlier loaded-world diagnostic, `results/vulkan-world-20261004-030221`,
reached an active Killhouse client and an authenticated owned server with one
human and 15 bots. Three subsequent samples show a running independent
prediction owner with no queue failures or growing debt. A later telemetry
request timed out, so the complete diagnostic remains **failed**. Leave and
Quit completed normally and restored the accepted client and preferences.
Validation-enabled rendering was very slow in this short diagnostic; this is
neither performance acceptance nor evidence of stock visual parity. Subsequent
owned diagnostics completed normally, with unchanged interactive preferences
and restored runtime binaries. `results/vulkan-world-20261004-043327` reported
zero Vulkan validation errors in an active stock match; the later slope-bias
fix and caches also pass the GPU validation suite.

No Vulkan Killhouse/Crash FPS or 60-second gameplay results have been accepted.
Short, camera-checked uncapped Killhouse measurements rose from about 59 FPS
before scope batching to 127 FPS after batching, about 199 FPS after packet
capture changes, and 263--272 FPS after binding/constant reuse and query polling.
These are diagnostic progress measurements, not alternating warmed acceptance.
The earlier short run (`results/vulkan-world-20261004-050223`) recorded about
1.68 ms of Vulkan recording, 0.35 ms of GPU work, 0.42 ms of resource recycling
and 0.09 ms of owner-handoff wait per displayed frame. CPU recording and frontend
work remain bottlenecks; GPU occupancy cannot be improved by removing FPS caps
alone.

Camera-checked fixed-view diagnostics with one and four recording workers reached
about **282.5/281.9 FPS**, respectively (`053552`/`054020`). Four workers reduced
command recording, but serial preparation remained about **1.39 ms** per display.
The four-worker frame-time p50/p95/p99 was **3.49/3.95/4.37 ms**. Its old GPU
coverage was incomplete because immediate texture uploads were omitted; that
result is retained with its limitation. Complete upload/readback/export capture
now passes the native GPU regression, but needs another live game run. Persistent
descriptors and queued frontend uploads have passed native validation and exact
DX9 parity; their in-game throughput benefit has **not yet been measured**.
Earlier `052217`/`053357` uncapped probes did not place a checked camera and must
not be compared with checked-view FPS. The probe now requires camera checks even
without screenshots.

All **41** current native suites pass (`results/vulkan-continuation-native-tests.log`).
The GPU presentation regression passes one/two frames in flight, minimize/restore
and resize with zero validation errors and 108 exact image pairs. These fixtures
do not substitute for actual stock option/restart/map tests.

Steam held the excess resident memory observed during testing. Closing it restored
about 22 GiB of available system RAM; no game/test process remained. Native GPU
tests afterward left memory stable. The experimental client now calls the previously
unused Steam API shutdown path after stopping its owners and unregisters callbacks
before shutdown. This inherited cleanup bug is fixed in source and compiles, but
is not a proven cause of Steam's shared-memory accumulation and still needs live
launch/quit validation. See [the memory observation](memory-observation-20261004.md).

The 60-second walking run
`results/native-render-scene86-mp_killhouse-480p-20261004-050742-966300`
held server ticks at 1000.005 Hz, prediction/commands at 999.972 Hz, snapshots
at 1000.022 Hz and all 15 bot control rates at 1000.005 Hz. Server work p99 was
985 us, with zero ending server/client debt. Its camera left the checked corridor
around 30 seconds; the complete benchmark therefore **failed**. All intervals
and CPU/GPU/memory observations are retained. Its 262 FPS average is not an
accepted scene result. Do not loosen the camera gate or discard those intervals.

Fresh camera-checked DX9 comparisons are under `results/native-render-scene86-*`:
about 569 FPS uncapped on Killhouse at 480p, and 501 FPS on Crash at 720p. These
are short baselines, not full timing acceptance. Alternating warmed runs, 1 kHz
rate/p99/debt gates, fault tests and loaded-world capture/restore replay remain.

GPU utilization is diagnostic. Acceptance depends on useful frame throughput,
visual correctness and independent gameplay progress. Vulkan becomes the
automatic default only after correctness passes and both checked routes improve
beyond measured run variability.

## Native x64 integration

The [x64 migration](X64.md) now builds the production client, server, updater and
SDK3 plugin with native asset layouts, OpenAL, FFmpeg and the Vulkan adapter.
Both architectures pass 58 core suites. The packaged native direct DX9 path
completed an uncapped Backlot session, measuring 779.96 FPS over 30.88 seconds in
the checked broad 1080p view with two bots and 1,000 Hz simulation. The packaged
client also passed hosting, map change, fast/video/sound restart, leave and quit.
The Vulkan path completed a
90-second match but measured 243.56 FPS and displays black models. A mirrored
DX9 reference also shows black models for the same immutable packets; the
adapter/data path still needs investigation. Native Vulkan performance and
visual acceptance remain open. Moving to x64 has not established 1,000 FPS or
full GPU utilization.

The subsequent [native rendering profile](RENDER_PROFILE.md) measures CPU
submission and GPU command gaps, records unsuccessful IPO/BSP batching trials,
and documents the Vulkan constant-prefix change with its validation and limits.
More than 1,000 FPS and Vulkan stock-world visual parity remain unverified.

The current packaged DX9 build measured **766.02 FPS** over 56.22 seconds
in the same broad 1080p view, **999.88 Hz** prediction and **1,000.00 Hz** server
simulation, with zero ending debt and clean client/server exits. The separate
eight-second profiling interval passed its sampled camera/bot and GPU capture
checks: `results/x64-dx9-render-final.json`.
