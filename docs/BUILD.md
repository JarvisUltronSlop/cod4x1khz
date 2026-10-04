# Windows build and launch

## Required game and tools

Own and install **Call of Duty 4: Modern Warfare (2007) on Steam**, app 7940.
[Steam store page](https://store.steampowered.com/app/7940/).
The installation must contain `main/`, `zone/`, and `localization.txt`.
The remaster and newer Call of Duty games are incompatible asset sources.
Steam's Manage > Browse local files locates the installed directory.

Install Git, Python 3, CMake **3.25+**, and Visual Studio 2022 or its Build Tools
with **Desktop development with C++**, MSVC v143 x86/x64 tools and a Windows SDK.
Run from PowerShell with `git`, `cmake` and `python` available on PATH.
The client, dedicated server, updater and regression plugin now build as native
x64 applications. Native outputs use `bin/x64/Release`; packaging uses
`runtime/x64`. The existing Win32 deployment remains available and `build.ps1`
still defaults to Win32 while native acceptance continues. For explicit x64
commands and current runtime limitations, see [x64 migration status](X64.md).

For the native game on this branch, use:

```powershell
./build.ps1 -Architecture x64 -Configuration Release
./launch-client.ps1 -Architecture x64 -Renderer d3d9 -RenderFps 0
```

The x64 build prepares its verified native FFmpeg dependency automatically.
`launch-server.ps1` and `launch-bench.ps1` also accept `-Architecture x64`.
The regular client renders the game; the headless benchmark executable is for
simulation/network probes. Use `/com_maxfps 0` for uncapped rendering and
`/r_vsync 0` to disable synchronization. Simulation continues at 1,000 Hz.

## Clone, prepare, build

The repository is currently a private personal backup. Authenticate as an account
with access before cloning. Its Git tree contains complete edited engine and
pinned reference snapshots; no submodule initialization is needed.

```powershell
git clone https://github.com/JarvisUltronSlop/cod4x1khz.git
Set-Location cod4x1khz
./prepare.ps1 -GameDirectory 'D:\SteamLibrary\steamapps\common\Call of Duty 4'
./build.ps1 -Configuration Release
./cod4x1khz.exe
```

The current branches create the ignored `results/` evidence directory automatically.
If building the frozen **v0.1 tag**, run `New-Item -ItemType Directory -Force results`
before `build.ps1`; the fresh-clone check found this assumption after the original
snapshot was captured. The corrected clean-clone native suite passes all forty tests.

Change `-GameDirectory` to your Steam installation. `prepare.ps1` reads that
installation and copies its data into `runtime/`; it does not edit the original.
It obtains the pinned `Microsoft.DXSDK.D3DX` **9.29.952.8** package, verifies its
SHA-256, and places dependencies beneath `.deps/`. Upstream license notices remain
in the source tree. The build packages the pinned Bot Warfare source plus the
project's native integration scripts, compiles the game and helper targets, and
runs the native tests. A failed command stops the script.

Outputs include root `cod4x1khz.exe`, its four startup DLLs, and
`runtime/cod4x1khz_server.exe`, `runtime/cod4x1khz_bench.exe`,
`runtime/cod4x1khz_update.exe` and the adapted SDK fixture DLL.
The root executable opens the stock-style multiplayer menu. Use Start Server
for the managed local match. Singleplayer is disabled. Benchmark profiles are
isolated from `runtime/interactive/players`.

## Rendering, hosting and short tests

Direct3D 9 is the only implemented renderer in release 0.1. Rendering defaults
to uncapped with VSync off. Console `/com_maxfps 1000` sets a **1,000 FPS ceiling**;
`/com_maxfps 0` removes it. Any rendering cap leaves the 1 ms gameplay clock intact.
The custom client/server protocol is **10002**; legacy or authenticated public
servers are outside the verified scope.

The `vulkan-testing` branch adds a native experimental Vulkan backend and pinned
project-local sources during preparation. DX9 remains the automatic default.
See [Vulkan development status and build commands](VULKAN.md) to test the
experimental client without replacing the accepted root executable. Stock
world-rendering and performance gates remain open.

```powershell
./build.ps1 -CoreOnly
./test-quick.ps1
./test-render-scenes.ps1 -Map mp_killhouse -Resolution 480p
./test-render-scenes.ps1 -Timing -Map mp_crash -Resolution 720p
```

`-CoreOnly` runs the standalone native suites without loading retail game assets.
Rendered cap stages are eight seconds; strict timing stages are sixty seconds.
Read [the current measurements](render-performance-20261003.md) before comparing
FPS. Older camera fixtures can face walls and inflate averages.

Existing source-checkpoint tooling originally assumed nested upstream Git
checkouts. Use `backup.ps1` and the root repository for complete current snapshots.
The original nested Git histories are retained in the private v0.1 workspace
archives and need not be restored to build a fresh clone.
