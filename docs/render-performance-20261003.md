# Rendering and camera measurements: October 3, 2026

Build 86 corrects the FPS measurement bias from automatic camera rotation into
nearby walls. The deployed executable is `C:\Users\jack\Desktop\cod4x1khz\cod4x1khz.exe`.
Presentation defaults to uncapped; `/com_maxfps 1000` sets a presentation ceiling
and `/com_maxfps 0` removes it. The cap does not change the 1 ms gameplay step.

The previous rotating-input timing workloads include occluded views, deaths and
killcams. Their FPS averages, including Build 83's 1,056 FPS average, must not be
described as normal gameplay performance. Historical records remain intact.
New legacy benchmark results explicitly carry that limitation.

## Current measurements

Actual rendered client, owned dedicated server, one diagnostic human and fifteen
Regular Bot Warfare bots. Direct3D 9, windowed, VSync off, low benchmark settings;
normal interactive settings and profiles are preserved. Each row is one complete
sixty-second **physical walking** measurement through a checked open-view segment.
No low-FPS intervals are removed.

| Scene | Mean FPS | Lowest sampled second | Median sampled second | Server simulation | Server work p99 | Client GPU busy mean |
|---|---:|---:|---:|---:|---:|---:|
| Killhouse, 640 × 480 | 615.88 | 491.24 | 617.35 | 1000.00245 Hz | 0.897 ms | 23.65% |
| Crash, 1280 × 720 | 546.76 | 511.54 | 549.54 | 1000.00070 Hz | 0.899 ms | 21.72% |

The FPS distribution uses approximately one-second presentation-counter
intervals. It is **not individual-frame 1% lows**. These are controlled route
measurements, not whole-map normal-combat averages or restored deterministic
replays. The human is immune to damage and external pushes to keep the camera
observable; its movement still runs through normal input and Pmove. All bots
retain ordinary combat, decisions, weapons, damage and respawns.

Client prediction/command rates were 999.99932 Hz on Killhouse and 999.90890 Hz
on Crash. Client snapshot rates were 1000.01598/1000.00890 Hz. All fifteen bots
retained their slot generation and applied controls at the measured server rate.
Every required rate passes the strict ±1 Hz tolerance. Server ending debt is
zero on both maps; client sampled debt never exceeds one tick. Server p99 includes
simulation and human snapshot work, not rendering or GPU time.

Mean process CPU use was 2.047 client plus 0.434 server core equivalents on
Killhouse and 2.173 plus 0.403 on Crash. The observer records OS process CPU time
and WDDM GPU engine busy time for the exact owned PIDs. GPU engines are not summed
or equated to shader throughput. Both Copy engines report zero activity in these
samples; that alone does not establish a transfer or scheduling problem.

Native timing results and offline summaries:

- [Killhouse 60-second native result](../results/native-render-scene86-mp_killhouse-480p-20261003-202853-889999/result.json),
  [CPU/GPU/FPS analysis](../results/native-render-scene86-mp_killhouse-480p-20261003-202853-889999/analysis.json).
- [Crash 60-second native result](../results/native-render-scene86-mp_crash-720p-20261003-203216-573753/result.json),
  [CPU/GPU/FPS analysis](../results/native-render-scene86-mp_crash-720p-20261003-203216-573753/analysis.json).

The separate eight-second 125, 240, 1000 and uncapped cap stages all pass their
camera, cap, accepted-input, per-bot, private prediction, collision, GPU health,
server p99 and backlog checks. A cap is an upper limit; setting 1000 does not
force hardware throughput to 1000. Fixed-view uncapped averages were 603.17 FPS
on Killhouse and 512.92 FPS on Crash; the accompanying eight-second walking
stages averaged 616.68 and 517.20 FPS respectively.

- [Killhouse short cap stages](../results/native-render-scene86-mp_killhouse-480p-20261003-201834-358883/result.json),
  [actual rendered view](../results/native-render-scene86-mp_killhouse-480p-20261003-201834-358883/broad-view.png).
- [Crash short cap stages](../results/native-render-scene86-mp_crash-720p-20261003-202329-903577/result.json),
  [actual rendered view](../results/native-render-scene86-mp_crash-720p-20261003-202329-903577/broad-view.png).

## Renderer changes and remaining costs

The existing two backend frame slots now each have preallocated CPU storage for
skin, FX mesh, mark and pre-tessellation index data. Frontend generation writes
there; the backend owner uploads only used prefixes before drawing, with native
worker/event ownership preserved. Unknown/transient buffers retain their native
path. There is no extra world copy or gameplay owner. The staging mode is enabled
by default and checked per frame so an in-flight diagnostic toggle is safe.

Actual eight-second fixed-view profiles on Build 86 show the buffer-lock issue:

| Measured CPU span | Legacy upload diagnostic | Staging enabled |
|---|---:|---:|
| Main vertex lock, mean per call | 86.24 µs | 0.123 µs |
| Main index lock, mean per call | 372.62 µs | 0.162 µs |
| Frontend, mean per frame | 1.506 ms | 0.842 ms |
| Actual Present call, mean | 0.795 ms | 0.768 ms |

These runs share the checked camera but have independently evolving bot worlds.
They prove the lock path and its measured cost; they do not prove identical-input
FPS causality or a precise percentage speedup. The production staging source is
still byte-identical to the source exercised by the
[19,487-check contract fixture](../results/build81-frontend-upload-contract-console.json).
Its simulated D3D dependencies are not GPU timing evidence.

With staging enabled, native D3D9 timestamps measured mean GPU frame commands of
**0.346 ms**, with a mean **1.205 ms** gap before the next frame's commands. Mean
frontend preparation was 0.842 ms and the actual Present call averaged 0.768 ms.
Those CPU spans overlap and must not be added together. These observations point
to the remaining CPU preparation/submission/presentation path, rather than enough
GPU work to saturate this card at the tested resolutions. More GPU usage or
1,000 FPS is not achieved by the current change.

The profiled CPU intervals between actual Present returns have p99 **1.936 ms**
(reciprocal 516.64 FPS). This is a separate render statistic from server work
p99. It includes diagnostic query overhead and is not physical display timing.
GPU timestamps use asynchronous zero-flush query polling; both captures completed
without skipped records, overflow, disjoint clocks, query errors or upload failures.

- [Staging profile and native lifecycle](../results/native-render-profile86-20261003-202119-627747/result.json),
  [timing analysis](../results/native-render-profile86-20261003-202119-627747/analysis.json),
  [CPU/GPU timeline](../results/native-render-profile86-20261003-202119-627747/timeline.json).
- [Legacy upload diagnostic](../results/native-render-profile86-20261003-203600-810260/result.json),
  [timing analysis](../results/native-render-profile86-20261003-203600-810260/analysis.json).

Build 83 also fixed a reproduced owner-barrier deadlock: main held the dvar lock
while waiting for the renderer, and the renderer needed that lock to patch model
lighting. The barrier now finishes renderer/worker waits before acquiring the
dvar scope, while the acknowledged simulation owner remains parked. The original
failure and two no-progress native stack captures are retained in
[the Build 82 diagnosis](../results/build82-render-stall-diagnosis/result.json).

## Camera controls and retained failures

`khz_scene` requires a loaded isolated benchmark and test controls. Before timing,
the helper probes the actual collision map and pinned waypoint data. It chooses
an open view facing the playable area by geometry, before observing FPS: center
ray at least 400 units, median of five rays at least 300. Walking requires a
clear capsule segment and open midpoint/end views. Selection considers only
geometric suitability, including an available level walking edge on Crash.

The actual human is placed once before timing. Input holds the checked view or
physically walks the segment while aiming toward its open target. Sampled actual
render angles, player positions, map/connection identity and post-stage ray
clearance are checked. Any failed check fails the stage; samples are retained.
External-push protection binds the exact human slot, admission generation,
challenge, connection time, port, server session, map id and checksum. It expires
on identity/map changes and explicitly restores the previous god flag before
normal Leave/Quit. It is inactive in ordinary gameplay.

Earlier failures are retained: the native screenshot's TGA format was initially
missed by a JPEG-only helper; a selected Crash viewpoint had no level walking
edge; and Build 85's protected-from-damage human was still pushed into cover by
bullet knockback/player ejection. None supplied accepted FPS results. Build 86
suppresses those external pushes for the diagnostic human only. Existing map
collision and movement are retained. Helper revisions after the immutable build
archive have explicit provenance addenda; original results are not rewritten.
The retained Killhouse 60-second samples also pass the subsequently strengthened
[walking-angle oracle](../results/native-render-scene86-mp_killhouse-480p-20261003-202853-889999/stronger-walking-camera-check.json).

## Repeat and provenance

From the project directory:

```powershell
./cod4x1khz.exe
./test-render-scenes.ps1 -Map mp_killhouse -Resolution 480p
./test-render-scenes.ps1 -Map mp_crash -Resolution 720p
./test-render-scenes.ps1 -Timing -Map mp_killhouse -Resolution 480p
./test-render-scenes.ps1 -Timing -Map mp_crash -Resolution 720p
```

Short cap stages are eight seconds each; `-Timing` is one sixty-second walking
stage. Startup, geometry selection, screenshots and warmup precede measurement.
Source/binary work and bulk hashes stay outside running game fixtures. The
read-only CPU/GPU observer runs separately. Tests restore isolated homes, leave
the interactive profile unchanged and stop only their owned processes.

[Build 86's archive](../results/build86-source-and-binary-hashes.json) records
all 40 passing native suites (11.94 seconds). All six current native lifecycle
stages—hosting, map change, fast/full restart, graphics and audio restart—pass
with visible menus, independent owner recovery and normal Leave/Quit in
[48.67 seconds](../results/build86-render-lifecycle/result.json).
The deployed binary/package bytes are also retained in the immutable
[Build 86 preservation](../results/build86-release-binaries/preservation.json).
The [validation summary](../results/build86-render-validation-summary.json)
binds all seven native case results and confirms all fourteen deployed binaries
and the bot package still match the build archive after testing.

Client SHA256: `fa9baca2076ec15275ef2474a9587497a32aa3ec5c0310def49ec056459db577`.

Dedicated server SHA256: `08090f4625e91ae4490fd93d3cf1cdb04ab29d4dfcad193b21a2cf8bdf7b15cd`.

The public-capability manifest/release gate, current four-human timing cases,
restored full-world input/render replay, live fullscreen device-loss recovery
and sustained representative 1,000 FPS remain pending. These measurements make
no official authentication or external-service compatibility claim.
