# Windows source build

For ordinary installation, download **cod4x1khz_setup.exe** from
[release 0.2](https://github.com/JarvisUltronSlop/cod4x1khz/releases/tag/v0.2).
The installer includes the x64 game and Server Manager and asks for your owned
Steam copy of **Call of Duty 4: Modern Warfare (2007)**. No development tools
are needed to install or play. See [installation and hosting](INSTALLER.md).

## Required game and tools

Install Git, Python 3, CMake **3.25+**, and Visual Studio 2022 or its Build Tools
with **Desktop development with C++**, MSVC v143 x86/x64 tools and a Windows SDK.
Run from PowerShell with `git`, `cmake` and `python` available on PATH.

Install your owned Steam game (app 7940). Steam's **Manage > Browse local files**
locates its directory, which must contain `iw3mp.exe`, `main/`, `zone/`, and
`localization.txt`, including the multiplayer fastfiles. Modern Warfare Remastered
and later Call of Duty games are incompatible asset sources. The repository
does not provide retail assets, game executables or proprietary middleware
binaries, including in historical commits.

## Clone, prepare, build

```powershell
git clone https://github.com/JarvisUltronSlop/cod4x1khz.git
Set-Location cod4x1khz
./prepare.ps1 -GameDirectory 'D:\SteamLibrary\steamapps\common\Call of Duty 4'
./build.ps1 -Architecture x64 -Configuration Release
./launch-client.ps1 -Throughput -Resolution 1080p
```

The public repository includes the edited engine and pinned reference source
snapshots as ordinary tracked files. No submodule initialization is needed.
Upstream license notices and source provenance remain in the tree; upstream
binary bundles are deliberately omitted.

`prepare.ps1` defaults to x64. It reads the selected installation and copies
retail assets into the ignored `runtime/` directory without modifying Steam's
files. It obtains the pinned `Microsoft.DXSDK.D3DX` **9.29.952.8** package,
verifies its SHA-256 and prepares the pinned Vulkan dependencies beneath `.deps/`.
The x64 build prepares its verified native FFmpeg dependency automatically.
It builds the game, dedicated server, updater, Server Manager and network helper,
packages the Bot Warfare scripts, and runs the native regression suites.

Native development outputs use `bin/x64/Release` and `runtime/x64`; desktop
applications build beneath `build/x64/Release/desktop/Release`. The packaged
installation places the game and Server Manager at its root. Development
profiles use `runtime/interactive/players`; benchmark profiles are separate.

To build the standalone installer with its project-only embedded payload:

```powershell
./scripts/package-installer.ps1
```

The installer is written beneath `results/distribution`. Its package guard
rejects retail fastfiles/IWDs (except the project's bot-script package), benchmark
tools and the benchmark RCON credential. Retail data is copied only on the end
user's computer after they select their Steam installation.

## Rendering and tests

Release 0.2 supports Vulkan and Direct3D 9. Fresh installed profiles select
Vulkan / Performance, with DX9 fallback. Options > Graphics saves the renderer
and Performance / Reduced Overlap preset. Rendering is uncapped by default;
`/com_maxfps 240` sets a cap and `/com_maxfps 0` removes it. Simulation and
prediction remain at 1,000 Hz. The custom gameplay protocol is **10002**.

To launch DX9 explicitly from a development build:

```powershell
./launch-client.ps1 -Architecture x64 -Renderer d3d9 -RenderFps 0
```

`launch-server.ps1` and `launch-bench.ps1` also accept `-Architecture x64`.
Use the installed Server Manager for ordinary hosting.

```powershell
./build.ps1 -Architecture x64 -CoreOnly
```

Core-only builds do not load retail game assets. Engine-dependent native tests
need the pinned DirectX headers when enabled. See [validation](INSTALLER_VALIDATION.md)
for the checks performed on the shipped package and remaining limitations.
Read the [rendering measurements](render-performance-20261003.md) before comparing
FPS; earlier camera fixtures could face walls and inflate averages.

## Historical Win32 builds

The build and launch scripts retain their historical Win32 defaults; explicitly
select **x64** for the supported 0.2 source build. The `v0.1` tag is a historical
Win32/DX9 source checkpoint, with retail/proprietary binaries removed from its
history. It is not a self-contained playable download. Optional Win32 runtime
preparation (`prepare.ps1 -Architecture Win32`) copies middleware DLLs only from
your own installed game. Building the old Win32 engine also requires separately
licensed import libraries that are not distributed here. Use the 0.2 x64 build
or installer for the current game.
