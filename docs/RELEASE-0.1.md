# Release 0.1 — Direct3D 9 development checkpoint

This release preserves **Build 86** as of October 3, 2026. It is an experimental
Windows client/server and a recovery checkpoint, not completion of the original
full CoD4x integration plan. The game binaries are the existing tested build;
repository setup changes documentation and preparation/backup tooling only.

## Project and current behavior

cod4x1khz extends KisakCOD with fixed **1 ms authoritative simulation, client
prediction and human command/snapshot updates**. Rendering has a separate clock
and defaults to uncapped. The stock-style multiplayer frontend, disabled
Singleplayer, project-local profiles, managed dedicated hosting, blue COD4-derived
icon, Bot Warfare integration, FOV scaling and unlock tooling are in the current
source. Local matches support sixteen total slots, including native bots.

Bot Warfare v2.2.0 retains its decision/reaction cadence while held native controls
are applied each authoritative millisecond. The custom protocol is **10002**.
Stock speed, gravity, weapon timing and appearance are the intended behavior;
complete gameplay parity still needs the acceptance work described below.

Pins: KisakCOD `8aadf94cc82795d6829ca06052996a5ec79001c3`, CoD4x Server
`ddd65fce1a1093017bad2dbb6840d75d11c0d56f`, public client
`17182c7bf04b859dea5c0db4acdb0b83c28d6dd2`, Bot Warfare
`7b52b462e40fa68287b9e54602e52da83b17766c`. See `upstream-lock.json`.

## Measured evidence

All **40 native suites pass** for Build 86. Camera-checked sixty-second walking
routes ran one diagnostic human and fifteen Regular combat bots, windowed DX9,
VSync off and low benchmark graphics on Ryzen 7 9700X / RX 9070 XT hardware.
The human is protected from damage and external pushes to keep the route observable.

| Scene | Average rendered FPS | Server rate | Server simulation + snapshot work p99 |
|---|---:|---:|---:|
| Killhouse, 640 × 480 | 615.88 | 1000.00245 Hz | 0.897 ms |
| Crash, 1280 × 720 | 546.76 | 1000.00070 Hz | 0.899 ms |

Simulation, prediction, human command/snapshot and each bot's control rates pass
the **0.1%** tolerance. No growing backlog was observed. Current native lifecycle
checks cover hosting, map change, fast/full restart, graphics/audio restart and
owned Leave/Quit. Separate eight-second render-cap tests pass.

Compact current evidence is also tracked in Git under
[docs/evidence/release-0.1](evidence/release-0.1/): the seven-case native validation
summary and both camera-checked CPU/GPU analyses. Full original records remain in
the workspace backup archives.

These routes are not whole-map combat averages or deterministic replays. Historical
wall-facing averages above 1,000 FPS are not representative gameplay evidence.
Server p99 is a CPU gameplay statistic, separate from render frame time. See
[the scoped evidence and remaining rendering costs](render-performance-20261003.md).

## Open work and boundaries

- Sustained representative **1,000 FPS** is not achieved. The measured GPU busy
  time is about 22–24%; frontend/submission/presentation costs remain under study.
- The current public capability manifest contains **2,117 entries** with **zero
  current-build proven entries** and twenty opaque entries. Historical scoped
  proofs remain archived, but full public CoD4x parity must not be claimed.
- Current-build four-human cases, full-world identical-input replay with the
  renderer active, broader movement/weapons/objectives and recovery qualification
  remain pending. Fullscreen recovery and live spectator/killcam behavior need work.
- The benchmark client suppresses rendering after connection but still initializes
  Direct3D and client assets. A fully GPU-free protocol client remains outstanding.
- Official/proprietary authentication, external-service acceptance and legacy-server
  compatibility are excluded. External update/publication endpoints stay unconfigured.
- **Vulkan has not been started.** `vulkan-testing` is a branch reserved for later
  renderer work, with fixed 1 kHz gameplay and user-selectable FPS caps preserved.

## Requirements and backup contents

An installed Steam copy of COD4 (2007) is required. See [build instructions](BUILD.md).
Git contains editable source, tooling, configuration, upstream snapshots and license
notices. Private release ZIP volumes preserve the current runtime/game data, profiles,
dependencies, binaries, logs, historical results and local nested Git metadata.
Every captured file and uploaded archive has a SHA-256 record. Credentials and the
backup mechanism's own staging/verification files are excluded; no files from the
installed game on D: are edited. See [restoration](BACKUP.md).

Existing deployed root client SHA-256:
`fa9baca2076ec15275ef2474a9587497a32aa3ec5c0310def49ec056459db577`.
Existing server SHA-256:
`08090f4625e91ae4490fd93d3cf1cdb04ab29d4dfcad193b21a2cf8bdf7b15cd`.

Upstream license notices govern their respective components. Personal backup
archives containing owned retail content and proprietary dependencies must remain
private and must not be redistributed as a free copy of the game.
