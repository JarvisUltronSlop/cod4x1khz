# Native rendering profile

The final native x64 Vulkan package reached **1,001.77 FPS** over **78.23 seconds**
in the checked broad 1080p Backlot view with two active bots. Prediction ran at
999.88 Hz and the dedicated server at 1,000.00 Hz, with ending tick debt 1/0 and
zero input admission rejections. The earlier descriptor-version run reached
1,039.17 FPS over 79.26 seconds. These are averages for the retained camera;
other scenes and minimum frame rates have not been qualified.

The final run measured **55.82% owned-process WDDM GPU busy time**. Full GPU
utilization has not been achieved. CPU submission remains the measured limit:
owner execution averaged 0.965 ms per completed presentation, versus 0.529 ms of
GPU command work. Increasing GPU activity alone is not the throughput goal.
The complete validation-enabled asynchronous restart lifecycle passed after
repairing a native script compiler pointer fault. Historical results below
retain their original limitations; black-scene measurements are not current
visual acceptance. See the final throughput section for evidence and launch.

The earlier recorded WDDM GPU engine utilization averaged **43.11%** (34.96--49.11%)
in the final DX9 run and **18.25%** (10.38--25.59%) in the Vulkan prefix trial.
These are samples of the game process's busiest GPU engine, not percentages of
peak shader throughput. The Vulkan control averaged 17.60%. Exact source hashes,
interval selection and sample counts are retained in
`results/x64-render-gpu-utilization.json`.

## Measured bottleneck

The eight-second native DX9 capture in
`results/x64-dx9-cpu-detail.profile-analysis.json` contains 6,406 complete GPU
samples without query errors or overflow. All 24 sampled camera states after
the capture request retained the intended view and two active bots.

| Work | Mean time |
| --- | ---: |
| GPU frame commands | 0.527 ms |
| Gap between GPU command intervals | 0.722 ms |
| CPU backend submission | 0.810 ms |
| CPU frontend | 0.415 ms |
| CPU Present call | 0.192 ms |
| Backend adaptive GPU wait | 0.00065 ms |
| Lit pass | 0.277 ms |
| Decal pass | 0.240 ms |

CPU stages are inclusive and overlap across threads. These rows cannot be
summed into a frame time. GPU command intervals exclude Present and may contain
internal bubbles; their fraction of the capture is not whole-device utilization.
The small adaptive GPU wait does not support disabling synchronization. The
submission path is the main measured target, particularly lit/decal draws and
frequent constant/state changes. Low aggregate CPU utilization can coexist with
a busy submission thread.

The final packaged build measured **766.02 FPS** over 56.22 seconds without
capture enabled, **999.88 Hz** prediction and **1,000.00 Hz** server ticks, with
zero ending client/server debt. The subsequent eight-second capture passed 17
sampled camera/bot checks and contained 5,520 complete GPU frames. It measured
about **1,760 draw calls**, **989 stream changes** and **1,357 vertex constant
updates per rendered frame. Its GPU-command work averaged 0.503 ms; CPU
submission averaged 1.010 ms with frequent-call instrumentation active, which
adds overhead relative to the earlier coarse capture.
Evidence: `results/x64-dx9-render-final.json`, its retained lossless screenshot,
`results/x64-dx9-render-final.profile-analysis.json` and timeline. Both created
processes exited with code zero. This remains below the requested 1,000 FPS.

## Changes and experiments

- Added bounded, per-thread totals for driver calls and tessellation, plus
  detailed backend pass spans. The inline idle guard avoids out-of-line timing
  calls when capture is off. Frequent-call capture adds timing overhead; use the
  unprofiled interval to measure throughput.
- Added an x64 Release compiler IPO option, `COD4X1KHZ_CLIENT_IPO`, default OFF.
  The matched valid runs measured 769.53 FPS without it and 755.49 FPS with it.
  Both held approximately 1,000 Hz prediction/server ticks with zero ending debt.
  Evidence: `results/x64-dx9-control-valid.json`,
  `results/x64-dx9-ipo-valid.json`. The renamed control executable was rejected
  by image-path validation and produced no usable benchmark.
- Added conservative adjacent BSP pre-tess batching, `r_bspPreTessBatch`, default
  OFF. It preserves triangle order and merges only compatible vertex streams,
  contiguous indices and identical lighting bindings. Per-draw diagnostic
  controls preserve separate draws. The trial measured 769.01 FPS, showing no
  useful gain; its sampled capture camera passed.
  Evidence: `results/x64-dx9-bsp-batch.json`.
- Vulkan immutable packets now copy only each constant file's monotonically
  growing written prefix. Untouched trailing registers remain zero, including
  sparse float/integer/boolean registers and gaps. Explicit zero writes extend
  the prefix; retirement creates a fresh immutable snapshot. Ordinary backend
  callers still reject undersized constant spans. Shader literals retain their
  overrides. The uniform cache includes the zero-tail interpretation.
  The warmed Vulkan control measured 236.37 FPS; the prefix trial measured
  255.73 FPS. One comparison is insufficient to establish a reliable gain.
  The trial had one ending client tick of debt and still rendered black models;
  it is not performance acceptance. Complete per-frame GPU timing exports are
  retained alongside both reports under `results/x64-vulkan-registers-*`.

The archived `r_altModelLightingUpdate 0` experiment did not resolve Vulkan's
rendering problem. The harness resets that option to 1. Its packet mirror also
rendered models incorrectly; the viewport/reference-clear repairs documented
below supersede that visual diagnosis.

## Reproduce

Run one owned fixture at a time. It places and protects the camera before bots
join, checks actual rendered angles (including angle wrapping), exports a frozen
capture and quits its exact created client/server. It uses isolated benchmark
homes and ports 28995/28997.

```powershell
python scripts/native-client-map-probe.py --client runtime/x64/cod4x1khz.exe --server runtime/x64/cod4x1khz_server.exe --renderer d3d9 --resolution 1920x1080 --broad-view --seconds 90 --cpu-profile --output results/x64-dx9-render-final
python scripts/analyze_render_profile.py results/x64-dx9-render-final.json
```

The analysis emits a summary and a Chrome/Perfetto timeline. Capture completeness,
raw SHA-256 and sampled camera/bot checks must pass. Do not use instrumented FPS
as an unprofiled performance gate. A Windows CPU sampling trace was attempted,
but WPR could not enable its profiling policy; no ETW call-stack capture exists
for this pass.

## Verification

The current packaged DX9 lifecycle passed in 72.66 seconds: hosting Backlot,
changing to Crash, fast/video/sound restart, leaving and quitting. Active stages
checked rendering, prediction, snapshots and the simulation owner; the exact
hosted server stopped and the client exited with code zero. Evidence:
`results/x64-native-packaged-lifecycle.json`.

Both architectures passed 58 core suites and compiled the production client and
server. Native shader packing and the real legacy GPU boundary fixtures passed
on x64 and Win32. Each architecture also passed one/two frames in flight,
minimize/restore, resize, retirement and query checks with zero Vulkan validation
errors. All 108 primitive/material DX9/Vulkan image pairs per architecture had
SSIM 1.0. These fixtures do not establish stock-world visual parity.

Native package PE/hash verification covers 11 AMD64/PE32+ files. The accepted
Win32 runtime hashes remain unchanged. See [the x64 migration status](X64.md)
and [Vulkan status](VULKAN.md) for outstanding acceptance work.

## Vulkan preparation and geometry reuse

The next native pass adds whole-submission preparation of immutable packets.
The existing renderer worker pool now resolves warmed material pipelines,
computes reflected texture keys and viewport/scissor state, and packs distinct
shader uniform snapshots into disjoint, owner-reserved upload slices. Mutable
ordinary callers still pack at draw time. Resource updates, texture barriers,
descriptor allocation, geometry versions, queries and draw order remain under
the submission owner. Worker failures join the batch before reaching that owner.

Dynamic geometry now has a version per frame slot. A completed slot can reuse
unchanged geometry, or replace its version before recording. A second version
within the same submission uses a separate arena slice. NOOVERWRITE appends
preserve earlier draw ranges. Cache allocation is lazy and bounded to 32 MiB per
frame slot; retirement frees it after GPU completion.

Adjacent indexed triangle lists merge only when pipeline, descriptors, uniforms,
geometry bindings, raster state and base vertex match, and the index ranges are
contiguous. Queries and any input changes prevent a merge. Blended primitive
order is preserved.

The controls `r_vkParallelPrepare`, `r_vkGeometryCache` and `r_vkBatchDraws` are
enabled by default in Vulkan. Set them before renderer creation, or use
`vid_restart` after changing them. `r_vkRecordingWorkers` controls the existing
pool's preparation and recording task count. Simulation scheduling is unchanged.
The trace now retains preparation task counts, geometry bytes copied/reused and
merged draws. These counters distinguish useful work removed from increased CPU
or GPU activity alone.

Real GPU regression coverage includes immutable batches crossing clears,
queries and DISCARD updates; cached geometry across one/two frames in flight;
NOOVERWRITE overlap rejection and appends; adjacent-triangle image equality;
and joined worker packing errors with cancellation of unsubmitted work.

Two valid comparisons used the same packaged executable, uncapped 1080p
Backlot, the checked fixed broad view and two active bots. All three controls
were switched together, so these runs measure their combined effect.

| Run | FPS | GPU busy time | CPU preparation per frame |
| --- | ---: | ---: | ---: |
| Disabled | 251.45 | 18.65% | 1.268 ms |
| Enabled | 309.18 | 24.00% | 0.470 ms |
| Disabled repeat | 253.54 | 18.38% | 1.300 ms |
| Enabled clean repeat | 308.18 | 22.71% | 0.506 ms |

The paired increases are 23.0% and 21.6%. The first enabled run copied
1.87 MB of geometry per frame instead of 10.36 MB, reused 8.39 MB, prepared
about 1,774 logical draws in the worker pool and merged about 73 draws. Server
simulation remained approximately 1,000 Hz with zero ending server debt;
client prediction averaged approximately 998-1,002 Hz, with 1-4 ticks of ending
client debt across these runs. CPU preparation improved, but other CPU work
still limits throughput. Stock Vulkan models remain black/distorted, and these
measurements do not establish visual or 1,000 FPS acceptance.

Evidence: `results/x64-vulkan-prepare-comparison.json` and its four source
reports/complete timing exports. The `enabled-repeat` attempt had no sustained
broad view because a class menu remained open; the `enabled-retry` attempt
overlapped package verification. Both are excluded. The fixture now records
recovery through the actual stock assault-class control before camera placement.

Both architectures passed 58 core suites, compiled production client/server
targets, and passed shader and real legacy GPU fixtures. The direct GPU tests
covered one/two frames in flight, resize/minimize, generation retirement and the
new preparation/cache/batching cases with zero synchronization-validation errors.
Initial Win32 legacy timing-export attempts failed; the final retry passed
without renderer changes, and no cause has been established. Its ordinary
readback/parity fixture passed. See `results/*-vulkan-prepare-*-test.log`.

The same current package completed the uncapped DX9 broad-view fixture at
733.61 FPS, 999.83 Hz prediction and 1,000.00 Hz server simulation, with zero
ending debt and clean exits (`results/x64-dx9-prepare-regression.json`). This
single run against earlier reports does not establish a matched DX9 performance
regression or improvement.

Packaged lifecycle checks passed all five active stages, leave and quit for
DX9 in 68.32 seconds and ordinary Vulkan in 137.62 seconds; both exact owned
servers stopped and clients exited zero. Evidence:
`results/x64-dx9-prepare-lifecycle.json` and
`results/x64-vulkan-prepare-lifecycle-normal.json`. The lifecycle helper now
accepts renderer/output/validation options, retains its last readiness sample,
and allows 90 seconds for initial asset loading. Two earlier validation-enabled
attempts timed out with a shorter loading budget. A subsequent validation-enabled
run passed hosting, map change, fast restart and video restart with zero reported
graphics errors, but its server exited before sound-restart recovery completed;
that complete validation lifecycle remains unverified. Evidence:
`results/x64-vulkan-prepare-lifecycle-validation.json`.

All 11 packaged native binaries were verified as AMD64/PE32+; the accepted
Win32 runtime executable hashes remain unchanged. The updated manifest is
`results/x64-packaged-binaries.json`.

## Viewport, prediction clock and frame overlap

`SetRenderTarget(0)` must reset the viewport and depth range even when rebinding
the same surface. The Vulkan facade retained the previous viewport. Correcting
that state restores the stock models. The private DX9 comparison also needed
the actual color target bound during depth-only clears. Both changes have real
GPU regressions. The checked 640x480 mirrored capture reached **0.999455 RGB
SSIM**, above 0.995, with identical immutable draw/resource packets:
`results/x64-vulkan-clear-reference-compare-fixed-join.json-diagnostics/world-parity.json`.
This one Backlot capture does not qualify every map/material/effect.

Native stack sampling found the prediction thread using **7,796.875 ms of CPU
over 8.028107 seconds**, predominantly collision traces. Aggregate CPU usage
hid this nearly full core. Inputs had drifted ahead of the server's +200 ms
admission bound: about 260 one-millisecond movement steps were replayed per
prediction tick. The newest accepted snapshot now also bounds forward clock
drift, preserving sequences, saved inputs, button edges and one-ms simulation.
The native regression covers stale and future admission, delayed snapshots and
preserved history/sequences. The new watermark participates in checkpoint
capture/restore and session reset. In the measured overlap interval, replay
fell to **1.789 steps/tick**, with **zero server admission rejections**.

The HUD also wrote an already-false UI setting every frame, acquiring the
prediction owner's settings lock. Conditional writes reduced its measured CPU
time from about **0.76 ms to 0.12 ms**. This UI setting remains owned by the
presentation thread; the simulation lock and its synchronization stay intact.

| Checked 1080p Backlot run | FPS | Ending client/server debt |
| --- | ---: | ---: |
| HUD redundant write removed | 307.00 | 1 / 0 |
| Admission clock drift repaired | 306.98 | 0 / 0 |
| Extra DX9 swap fence disabled on Vulkan | 419.28 | 0 / 0 |

The last two runs use the same binaries and camera/bots. The Vulkan backend's
own frame-slot/timeline resource protection remains active; only the additional
DX9 swap fence is disabled by `r_vkLegacySwapFence 0`. The default stays enabled
while broader validation is pending. The last interval averaged **33.94%**
owned-process WDDM busiest-engine busy time and **0.792 ms** useful GPU work per
frame. Mean CPU preparation was 0.517 ms, command recording 0.162 ms and total
recording 0.829 ms. GPU activity and FPS have improved, but 1,000 FPS and full
GPU utilization are not established. Evidence: `results/x64-vulkan-hud-lock-control.json`,
`results/x64-vulkan-input-lead-control.json`, `results/x64-vulkan-swap-overlap.json`
and their CPU/native analysis exports.

The 64-bit animated-model decal callback still read fields at 32-bit pointer
offsets. Bot bullet impacts exposed a native crash. Typed context fields and
initialized optional fields repair this path; its native regression passes
both architectures. The subsequent 90-second bot session exited cleanly.

`--sample-cpu` adds an eight-second exact-process stack diagnostic, excluded
from FPS intervals. Samples contain blocked/descheduled time; separate thread
CPU deltas identify actual execution. `--cpu-profile` can retain Vulkan CPU
spans even though the DX9 timestamp-frequency/disjoint query pair is unsupported;
GPU timing still comes from the serial-matched Vulkan trace. Analyze retained
fixtures with `analyze_cpu_render_profile.py`, `analyze_cpu_samples.py` and
`analyze_native_map_profile.py`. Incomplete captures cannot qualify acceptance.

Streamed MP3/WAV decoding now has an optional bounded PCM producer,
`snd_asyncStreams 1`. One worker owns each active stream decoder/file; it makes
no OpenAL calls. Presentation consumes completed PCM without waiting for
decode. Initial prefill remains synchronous. Channel release, map changes and
sound shutdown join the worker before closing its file or destroying audio
resources. EOF, loop boundaries, delayed/paused playback and underrun recovery
remain explicit. Native tests cover chunk order, bounded look-ahead, stalled
decodes, joined teardown and propagated errors.

The checked `x64-vulkan-audio-overlap-joined` interval measured **598.31 FPS**,
**50.95%** owned-process GPU busy time, **999.78 Hz** prediction and
**999.97 Hz** server simulation. Main-thread sound work fell from approximately
0.988 ms to **0.0088 ms**. Ending client/server debt was 0/1, admission
rejections were zero in the measured interval, and replay averaged 1.948
steps/tick. The first audio attempt left the class menu open and is excluded.
A later stack-control repeat measured 568.22 FPS and 48.21% GPU busy time over
32.51 seconds. These runs remain below 1,000 FPS.

DX9 with asynchronous streams measured **587.71 FPS** and **34.65%** GPU busy
time over 77.04 checked seconds, with 999.85/999.99 Hz prediction/server rates
and ending debt 1/1 (`results/x64-dx9-audio-overlap.json`). It does not establish
a matched regression against earlier DX9 packages. Its main-thread samples
identified repeated microphone-device cursor calls; an independent configurable
poll interval retains all captured PCM while reducing empty device queries.

The packaged Vulkan lifecycle with asynchronous streams and the extra swap
fence disabled passed hosting, Backlot-to-Crash map change, fast/video/sound
restarts, leave and quit, with zero reported validation errors and the exact
owned server stopped. Evidence:
`results/x64-vulkan-render-overlap-lifecycle.json`. The x64 native suite passed
60/60 and the Win32 suite 61/61 including shader packing. Win32 GPU fixtures
now use `/LARGEADDRESSAWARE`, matching the Win32 game: the earlier 2 GB test
process exhausted address space; the subsequent legacy timing-export and direct
GPU presentation/validation fixtures passed.

`r_vkOverlapUploads 1` adds a separate bounded CPU geometry-transfer lane. Its
resource leases and immutable bytes drain at the next scene boundary, preserving
the previous frame. GPU calls and resource creation/retirement retain their
submission owner. A real regression uploads during command preparation and
checks unchanged current-frame pixels plus changed next-frame pixels. Another
checks new dynamic buffers uploaded within a scene after ownership changes.
The first integrated attempt exposed that loading ownership case and is
excluded; scene ownership is now established at BeginScene. These controls and
`r_vkLegacySceneFence` remain experimental until integrated validation passes.

## Asynchronous submission and frontend construction

Generation-checked adjacent pipeline-key reuse reduced repeated complete-key
hashing in both owner warmup and each preparation worker. The checked
`x64-vulkan-adjacent-pipeline-control` interval reached **677.32 FPS** over
51.74 seconds, with **55.18%** owned-process GPU busy time, approximately 1,000 Hz
prediction/server progress and zero ending debt. Both keys and shader resource
generations must still match; no draw or material is discarded by this cache.

`r_vkAsyncSubmit 1` separates immutable packet construction from Vulkan owner
execution. Two CPU packet banks permit the existing engine backend to construct
the next packet while a dedicated Vulkan owner prepares and submits the previous
one. Bytes and resource leases move by bank swaps rather than borrowed legacy
pointers. The queue is bounded to one active submission plus one producer bank;
it never accumulates an unbounded render backlog. Restart barriers finish CPU
submission before the shared renderer workers can be paused, and shutdown joins
the new owner before retiring resources. GPU frame timelines still protect every
frame slot and geometry version.

The checked `x64-vulkan-async-submit-control` interval reached **741.93 FPS** over
49.71 seconds, **55.29%** GPU busy time, 999.91/1,000.00 Hz prediction/server rates
and zero ending debt. The current frame can be built while a test deliberately
holds the previous GPU owner in preparation. Other regressions cover ordered
frontend uploads, query polling, invalid replacement issues, failed background
recording and joined shutdown under both architectures.

`r_overlapFrontend 1` allows the stock two-bank SMP frontend to start while the
CPU backend is busy, waiting at its normal end-of-construction handoff instead.
This requires the independent gameplay owner. Actual frontend/backend span
intersection rose from **0.013% to 91.24%** of inclusive frontend time (including
its handoff waits), but the checked run
remained **733.69 FPS**. An exact-process stack diagnostic identified blocking
sun-query polling in the backend; overlap alone does not establish an improvement.
Evidence: `x64-vulkan-frontend-handoff-control` and
`x64-vulkan-frontend-handoff-stacks`, including retained span/stack exports.

Completed event, timestamp and occlusion results now have sequence-checked atomic
publication by the Vulkan owner. An older result can be read while another frame
is in preparation without taking the GPU mutex; a newly issued replacement
immediately invalidates that result. `r_asyncSunQueries 1` keeps the last measured
sun-flare visibility when a query is pending and waits to reissue that query until
its actual result is available. It preserves sun effects without a CPU spin loop.
The checked `x64-vulkan-query-overlap-control` run remained **734.58 FPS** over
77.46 seconds, with 999.88/1,000.00 Hz prediction/server progress and ending debt
3/1. GPU frame-slot recycling then averaged **0.264 ms**, so the requested FPS
and latency/debt acceptance are still open.

The experimental GPU frame-ring limit now supports one to three protected slots.
Each slot owns its command pools, descriptors, uploads and geometry versions;
timestamp export drains every configured slot. Direct GPU tests cover all three
ring sizes, exact-size copies and scaled presentation, resize and minimize/restore.
Adjacent descriptor preparation reuses a hash only when shader generations and
every used texture/sampler match. Redundant legacy bindings retain their existing
reference instead of repeating RTTI and reference-count operations, and material
capture rebuilds only when a render state changes. These tests establish component
correctness. The checked `x64-vulkan-three-flight-control` interval reached
**946.60 FPS** over 75.68 seconds, 999.90/1,000.00 Hz prediction/server rates and
ending debt 1/0. GPU recycling fell to **0.0082 ms** and useful GPU work averaged
0.556 ms, with **54.96%** owned-process busy time. The submission owner averaged
1.021 ms of CPU execution per completed presentation, including 0.770 ms of
backend recording; query polling and packet lease retirement contributed only
0.0007/0.0065 ms. Broad performance and restart acceptance remain pending.

The `x64-vulkan-pipeline-version-control` interval reached 918.14 FPS over
74.71 seconds. Producer-owned versions of the complete immutable pipeline key
reduced owner warmup from 0.077 to 0.046 ms; the overall FPS result did not
establish an improvement. Both architectures passed the stock primitive parity,
state-change, asynchronous query and protected-slot regressions.

Following this run, ordinary per-draw clock sampling was removed. `prepare_ms`
now means the frame's `record_ms` minus its measured command-pass time. This
includes replay, uploads, queries, barriers and frame finish, whereas earlier
captures measured draw preparation and the immutable preparation prepass only.
Compare total `record_ms` and owner execution across these versions; the old
and new `prepare_ms` values have different scopes. Frame and GPU timestamps,
submission serials and command-pass measurements are retained.

## Final throughput preset and validation

Launch the native package with uncapped Vulkan presentation:

```powershell
./launch-client.ps1 -Throughput -Resolution 1080p
```

This selects x64/Vulkan, three protected GPU frame slots, bounded asynchronous
submission, frontend construction/upload overlap, nonblocking sun query reads,
asynchronous PCM decoding and the 5 ms voice capture poll. The additional legacy
swap/scene fences are disabled; required resource timelines and frame handoff
barriers remain. Explicit `-RenderFps` still sets a presentation cap; the preset
defaults to zero. It opens the multiplayer menu unless `-Server` or `-Benchmark`
requests a connection. It does not enable benchmark/test controls in ordinary
use. Existing launch defaults are preserved when the preset is absent.

Immutable producer versions now bypass adjacent pipeline/descriptor comparisons
only when complete captured state is equal. Used descriptor writes retain full
initialization; cached draws avoid clearing unused scratch arrays. Geometry
binding uses the resource already validated for that draw. The `-6` legacy GPU
regression changes shaders, color write masks, textures and wrap/clamp sampling
inside a captured frame and compares exact DX9 pixels on both ABIs.

The final checked interval contains **78,367 complete displayed frames** with
GPU timestamps joined to their original submission serials and no trace gaps.
Mean recording/command time is 0.739/0.152 ms; timeline recycling is 0.011 ms.
Retirement/query polling average 0.0070/0.00065 ms. CPU stage totals overlap and
must not be added together. Stack sampling follows the FPS interval, and the
subsequent eight-second detailed capture is excluded from the reported FPS.
Evidence: `results/x64-vulkan-throughput-final-control.json`, its lossless PNG,
`.native-analysis.json`, `.cpu-analysis.json`, and `-cpu-stacks.analysis.json`.

Two cached-RAM/device-copy experiments passed synchronization and pixel tests
but reduced throughput to **846.29** and **850.95 FPS**, while GPU busy time rose
to **77.30%** and **74.64%**. The second kept uniforms in GPU-local memory. Both
paths were removed. Their retained evidence is
`results/x64-vulkan-staged-geometry-control.native-analysis.json` and
`results/x64-vulkan-separate-staging-control.native-analysis.json`.

Combat exposed a native compiler fault: an unresolved code position copied
from an undefined variable could retain upper pointer bytes when initialized
through `intValue`. `SpecifyThreadPosition` now initializes/checks `codePosValue`.
Its actual production function is regression-tested with a stale address above
4 GiB. Debug thread termination also uses the typed frame position rather than
a Win32 stack-layout alias. The two-minute 15-bot stress passed three sessions
and **97 observed spawn changes**, with a clean server exit:
`results/x64-script-forward-pointer-combat-stress.json`.

The complete selected Vulkan path passed Backlot/Crash, fast/video/sound
restart, leave and quit with zero validation errors and clean native client,
server and attached-debugger exits:
`results/x64-vulkan-throughput-forward-pointer-lifecycle.json`.
The final packaged executables separately pass the same complete sequence in
149.79 seconds with clean exits:
`results/x64-vulkan-throughput-final-lifecycle.json`.
Final component checks passed **60 x64 / 61 Win32** tests and both production
architectures built. Both GPU suites passed protected slots 1/2/3, queries,
uploads, descriptor/state changes, resize, failure and joined shutdown. Each
architecture passed **162 exact primitive/material image pairs** at SSIM 1.0:
`results/x64-vulkan-throughput-final-direct-tests/image-parity.json` and
`results/x86-vulkan-throughput-final-direct-tests/image-parity.json`.
Package verification checks eleven AMD64/PE32+ files and local import ABIs,
and preserves both accepted Win32 executable hashes:
`python scripts/verify-native-package.py`.

The final selected asynchronous path passes one mirrored **640x480 stock-world
Backlot frame at SSIM 0.999461**, using identical immutable draw/resource packets:
`results/x64-vulkan-throughput-final-world-parity-diagnostics/world-parity.json`.
Private DX9 comparison substantially slows rendering and is not a performance
configuration; its frame rate and ending debt are excluded from FPS acceptance.
This comparison does not qualify every map, material or effect.
