# Native rendering profile

The native x64 client remains below 1,000 FPS in the checked broad 1080p
Backlot view. Direct DX9 renders the stock scene correctly. Vulkan still has
black/distorted models; its scene timings are diagnostic until visual parity
and simulation timing acceptance pass.

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

The `r_altModelLightingUpdate 0` experiment did not resolve Vulkan's rendering
problem. The harness explicitly resets that option to 1. The exact packet
mirror's DX9 reference also renders the models incorrectly, so the shared
adapter/data path remains a target for investigation.

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
