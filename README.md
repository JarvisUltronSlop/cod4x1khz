# cod4x1khz

**[Release 0.2](https://github.com/JarvisUltronSlop/cod4x1khz/releases/tag/v0.2)
adds native x64, Vulkan alongside DX9, a standalone Windows installer and an
included Server Manager.** Simulation and prediction run at 1,000 Hz while
rendering runs independently and defaults to uncapped. The release includes the
gameplay, rendering, audio and interface fixes from user testing.

Experimental Windows client/server built from KisakCOD at
`8aadf94cc82795d6829ca06052996a5ec79001c3`. CoD4x Server at
`ddd65fce1a1093017bad2dbb6840d75d11c0d56f` is retained under `upstream/` as
the feature reference. This is an early implementation, not a CoD4x feature-parity release.

All dependencies, copied game data, binaries, logs, and measurements stay inside
this project directory. **You need your own installed Steam copy of
[Call of Duty 4: Modern Warfare (2007)](https://store.steampowered.com/app/7940/).**
Modern Warfare Remastered and later games do not supply the required data.
Original IWD and fastfile assets remain outside source control. The 0.2 installer
embeds no retail assets and copies them from the selected Steam installation.
Retail assets, original game executables and proprietary middleware binaries
are excluded from the public repository and its history. Release 0.1 is retained
as a source checkpoint; use the 0.2 installer to install and play.

Download **cod4x1khz_setup.exe** from release 0.2, select your Steam CoD4 folder
and a separate output folder, then launch **cod4x1khz.exe** or
**cod4x1khz_server_manager.exe** from the output. The installer and manager use a
dark theme. Fresh profiles select Vulkan / Performance, with DX9 fallback.
Rerun the installer on the same standalone output to repair/update while
preserving profiles and server configuration.

See [all changes since 0.1](docs/RELEASE-0.2.md), [installation and hosting](docs/INSTALLER.md),
[validation and remaining limits](docs/INSTALLER_VALIDATION.md), and
[Windows source builds](docs/BUILD.md). Release 0.1 remains the original Win32
DX9 checkpoint. The `v0.2` tag and `vulkan-testing` branch contain the x64 release.
Release-based workspace backups are disabled; see [source and local backups](docs/BACKUP.md).

## Build and run

Use PowerShell from the project directory with Git, Python 3, CMake 3.25 or newer,
and Visual Studio 2022's Desktop development with C++ workload installed:

```powershell
./prepare.ps1 -GameDirectory 'D:\SteamLibrary\steamapps\common\Call of Duty 4'
./build.ps1 -Architecture x64 -Configuration Release
./launch-client.ps1 -Throughput -Resolution 1080p
```

`prepare.ps1` defaults to `D:\SteamLibrary\steamapps\common\Call of Duty 4`.
Pass `-GameDirectory` to select another installation. Preparation defaults to
the native x64 runtime and downloads/checks the pinned DirectX NuGet package.
Use `-Architecture x64` when building; the build script retains its historical
Win32 default. `build.ps1 -CoreOnly -Architecture x64` builds the standalone
clock/queue/protocol tests without the engine or game assets.

The native launcher opens the multiplayer menu. Native development builds live
in `runtime/x64`; the standalone install puts the x64 game at its root.
Interactive development profiles use
`runtime/interactive/players`; benchmark homes are separate. Local hosting is
managed through Start Server. Source changes for bots and public extensions are
still undergoing integration and acceptance checks; see `docs/public-capabilities.md`.

The benchmark server binds to `127.0.0.1:28960`; its rendered client uses port 28962.
Select a team and class normally. Resolution choices are `480p` (640×480)
and `720p` (1280×720). Rendering defaults to uncapped (`com_maxfps 0`). In the
game console, enter `/com_maxfps 240` to set a cap or `/com_maxfps 0` to remove
it. `-RenderFps` sets the initial cap. The cap limits presentation only;
gameplay and protocol updates use a fixed 1 ms step. The client and server use
protocol 10002 and reject incompatible clients. Local key proofs do not claim
official authentication or external-service acceptance.

The current benchmark executable suppresses presentation after connecting and
does not open an audio device. It still initializes Direct3D and loads client
assets during startup. A GPU-free protocol client remains outstanding.

## Measurement

Benchmarks now default to **60 seconds**. Short diagnostics run independently:

```powershell
./test-quick.ps1
./test-render-caps.ps1
./test-render-scenes.ps1 -Map mp_killhouse -Resolution 480p
./test-render-scenes.ps1 -Timing -Map mp_crash -Resolution 720p
./test-collision.ps1 -HeadlessClients 3
./benchmark.ps1 -Map mp_killhouse
./benchmark.ps1 -Map mp_crash
./benchmark.ps1 -Map mp_crash -Seconds 20 -HeadlessClients 0 -Resolution 480p
./benchmark.ps1 -Map mp_crash -Seconds 20 -HeadlessClients 0 -Resolution 720p
```

The default match has one uncapped rendered client and three quiet benchmark clients.
Startup and a ten-second warmup are outside the measured interval. Test scripts
close the processes they launch, and rendered benchmark runs mute audio. Normal
interactive launches retain audio. Use `-Seconds` to select another duration.

Use `test-render-scenes.ps1` for camera-verified FPS measurements with one human
and fifteen Regular bots. It checks native collision geometry before choosing
an open view, runs four eight-second cap stages, then physically walks a checked
segment. `-Timing` instead runs one sixty-second walking stage with the strict
0.1% gameplay rate tolerance. Only the diagnostic human is protected from damage
and external pushes; bots retain normal combat. Tests use isolated profiles and
close their owned game processes normally. The legacy rotating-camera benchmark
can face nearby walls or enter killcams, so its average FPS does not represent
normal gameplay. The camera fixture's one-second FPS ranges are not frame 1% lows.
Current results and remaining rendering limits are in
[the rendering report](docs/render-performance-20261003.md).

Engine console commands: `khz_stats`, `khz_json`, and `khz_resetstats`.
When entering them through the in-game console, prefix with `/`.
The localhost-only client telemetry endpoint accepts `khz_stats` and `khz_state`.
Development fixtures supply their RCON credential explicitly through
`KHZ_BENCHMARK_RCON_PASSWORD`. Production RCON is disabled by default; configure
it through the Server Manager if needed.

`scripts/monitor.py` reads live engine counters and Win32 CPU/memory counters.
It also samples WDDM GPU engine activity for the benchmark PIDs, CPU time for
the engine's main/backend threads, and cumulative wall times for prediction,
frontend preparation, backend execution, Present, and renderer waits. Timing
stages overlap and must not be added together. GPU counters are optional: an
unavailable counter is reported explicitly and does not fail gameplay checks.
The GPU percentage represents the process's busiest GPU engine, not shader
throughput or a sum across parallel engines. Dedicated servers may have no GPU
counter instance. GPU activity depends on the workload and chosen cap; three
quiet clients do not render after joining. A CPU or presentation limit can leave
the GPU partly idle even with uncapped rendering.
It records measured rates, server work p99, tick lateness, scheduler debt,
prediction corrections, compressed snapshot bytes, successful Direct3D presents,
and errors in JSON. `render_present_fps` is the swap-chain Present rate; the
frontend's `presentations_hz` counter is recorded separately.
It does not replace engine simulation with synthetic arithmetic. Generated
reports are saved under `results/`. Rate checks alone do not establish complete
gameplay correctness or independence from rendering.

Match checks also require each client to perform prediction and spend at least
20% of the measured interval alive in authoritative gameplay. This catches a
connected client stuck spectating, while allowing ordinary deaths and killcams.
Script runtime errors fail the match check. These are diagnostic safeguards;
the complete movement, collision, and weapon acceptance suite remains in progress.
Match checks also require snapshot collision on every measured prediction dispatch
with zero legacy fallbacks. Unsupported dynamic worlds retain a counted legacy
path and therefore fail that coverage check. Dispatch coverage is not a trace count
or proof of complete world parity.

`test-quick.ps1` checks core queues/clocks, actual GSC waits and notifications,
shared Pmove replay, immutable publication, body rewind/restore, a 50 ms scheduler stall, map changes,
restarts, disconnect/reconnect, and a 30-second impaired-network session.
Individual `test-*.ps1` scripts can run those checks separately.
`test-render-caps.ps1` changes caps to 125, 240, 1,000, and uncapped in one
session and checks that gameplay rates and both client/server backlog remain
bounded. Each measured stage lasts eight seconds. FPS depends on the scene;
a cap is a maximum, not a promised frame rate.
`test-collision.ps1` probes both maps with bounded capsule sweeps and nine
rejection/binding guards, saving the summary and hashed full report. It is a
separate diagnostic; active prediction uses the client simulation worker's
snapshot collision records and dedicated CM scratch.

See [implementation status](docs/status.md) for the remaining work and the
conditions required before calling the 1 kHz milestone complete.
Active input, networking and prediction run on an independent client simulation
worker. Presentation consumes buffered player/world records and event journals,
with the existing renderer backend. Map and asset changes use acknowledged
barriers. See [thread ownership](docs/thread-ownership.md) for this boundary and
the remaining dynamic-world and full-world replay validation.

## Source and notices

`./checkpoint.ps1` saves a small source archive under `results/`, including root
tools, new engine adapters, a patch against the pinned engine, and binary hashes.
It excludes game data, dependencies and builds; restoration instructions are in
its manifest.

The editable fork is `engine/`, with upstream provenance and license notices.
The shared scheduling/protocol primitives are in `native/`, and engine adapters
are in `engine/src/khz/`. Original KisakCOD and CoD4x notices remain in their
respective repositories. Dependencies retain their own notices and license files.
No authenticated CoD4x client behavior has been implemented or claimed.
