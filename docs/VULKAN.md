# Native Vulkan development status

The `vulkan-testing` branch now contains a native Vulkan 1.3 backend. It is still
experimental. The accepted root and runtime clients retain the Build 86 DX9
binary, SHA-256 `fa9baca2076ec15275ef2474a9587497a32aa3ec5c0310def49ec056459db577`.
Release `v0.1` and its private workspace archives remain intact.

The client and server are **32-bit Win32** builds. Gameplay, prediction, input
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
  projection and filtered edges). Latest **78 matched image pairs** have SSIM
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

Archived settings are `r_graphicsApi auto|vulkan|d3d9`, `r_vkDevice -1` or an
explicit device index, and `r_vkFramesInFlight 1|2` (default 2). `auto` selects DX9
until correctness and matched performance gates pass. `com_maxfps 0` remains
uncapped, `com_maxfps 1000` caps rendering, and `r_vsync` retains its stock role.
`r_vkValidation` is a development diagnostic. Unsupported operations report
explicit errors; they are not accepted as implemented features.

## Development builds and diagnostics

Run from the project root after preparing Steam assets and the DirectX dependency:

```powershell
./scripts/build-vulkan-tools.ps1 -Validation -GpuTest
python ./scripts/compare_graphics_fixtures.py ./results/vulkan-gpu-tools
cmake -S . -B build/Release -G 'Visual Studio 17 2022' -A Win32 `
  -DCOD4X1KHZ_BUILD_ENGINE=ON -DCOD4X1KHZ_BUILD_VULKAN=ON
cmake --build build/Release --config Release --target KisakCOD-mp khz_launch_probe --parallel 5
python ./scripts/native_vulkan_startup_probe.py
```

Explicit engine targets build `bin/Release/cod4x1khz.exe` without deploying it over
the accepted executable. The startup probe temporarily stages that experimental
binary at its recognized runtime path, owns and closes its process, uses a
separate profile/home and restores the original runtime binary. Ordinary
`build.ps1` includes deployment; use the explicit targets during development.

## Remaining delivery gates

Loaded-world material/effect coverage, stock options, cinematics, restarts and
map lifetimes still need native validation. Worker-pool extension and parallel
command recording remain open. Stock image comparison must reach SSIM 0.995
without missing geometry or effects; fixture parity cannot substitute for it.
Earlier match-start failures exposed idle event-query behavior, dynamic upload
capacity, dirty volume transfers, depth bias and shadow image-view/comparison
requirements. Their diagnostic results remain private under `results/`.
The latest loaded-world diagnostic, `results/vulkan-world-20261004-030221`,
reached an active Killhouse client and an authenticated owned server with one
human and 15 bots. Three subsequent samples show a running independent
prediction owner with no queue failures or growing debt. A later telemetry
request timed out, so the complete diagnostic remains **failed**. Leave and
Quit completed normally and restored the accepted client and preferences.
Validation-enabled rendering was very slow in this short diagnostic; this is
neither performance acceptance nor evidence of stock visual parity.

No Vulkan Killhouse/Crash FPS or 60-second gameplay results have been accepted.
Fresh camera-checked DX9 comparisons are under `results/native-render-scene86-*`:
about 569 FPS uncapped on Killhouse at 480p, and 501 FPS on Crash at 720p. These
are short baselines, not full timing acceptance. Alternating warmed runs, 1 kHz
rate/p99/debt gates, fault tests and loaded-world capture/restore replay remain.

GPU utilization is diagnostic. Acceptance depends on useful frame throughput,
visual correctness and independent gameplay progress. Vulkan becomes the
automatic default only after correctness passes and both checked routes improve
beyond measured run variability.

## Recommended subsequent 64-bit phase

An x64 migration is recommended after Vulkan correctness and its Win32 baseline
are established. It has not been started or included in the accepted renderer
scope. It requires serialized/in-memory asset separation, audits of pointer
casts and packed structures, replacement of x86 inline assembly, 64-bit versions
or replacements for runtime libraries, and an adapted plugin ABI. Retain the
custom protocol and repeat gameplay/replay/performance gates. More address space
is a clear benefit; an FPS improvement must be measured.

Use `backup.ps1` for source/private workspace backups; it includes otherwise
ignored engine sources and CMake files. Never move `v0.1` or force-push. Keep
retail assets, captures and personal profiles private. The readiness marker
`docs/vulkan-ready.json` must remain absent until the complete renderer is
accepted. The backup automation does not authorize renderer changes itself.
