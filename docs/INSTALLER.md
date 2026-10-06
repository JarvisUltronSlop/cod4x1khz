# Windows installer and Server Manager

The `vulkan-testing` branch builds a standalone native x64 installer. It requires
an installed Steam copy of **Call of Duty 4: Modern Warfare (2007)**. Retail game
assets are copied locally from that installation and are excluded from the
installer, Git, and redistributable packages.

## Install and play

1. Open `cod4x1khz_setup.exe`.
2. Select your Steam CoD4 folder. The wizard detects Steam libraries when possible;
   **Browse** also accepts another library. Select the folder containing
   `iw3mp.exe`, `main`, `zone`, and `localization.txt`.
3. Select a separate output folder. Its parent must exist and be writable. Use a
   short Windows path; paths with spaces work. Linked/reparse folders and paths
   the legacy asset loader cannot represent are rejected.
   Both folder fields can be edited in any order.
4. Click **Install** and confirm the selected output folder, then open
   **cod4x1khz.exe** for ordinary multiplayer or
   **cod4x1khz_server_manager.exe** to host a dedicated server.

The output has two main applications at its root, with client DLLs beside the
game. Assets are under `runtime/main` and `runtime/zone`; the dedicated engine is
under `runtime/x64`. Helpers live under `runtime/tools/x64`. No .NET, Qt, Electron,
Python, compiler, or development environment is required to run either application.
A graphics driver providing Vulkan is required for Vulkan rendering.
The installer and Server Manager use a fixed dark theme with blue accents.

Fresh installed player profiles select Vulkan with the Performance preset
and uncapped rendering. A Vulkan initialization failure falls back to DX9.
**Options → Graphics** includes renderer and preset controls; changing either
restarts graphics. Subsequent choices are saved. Uncapped rendering does not
guarantee a particular FPS or GPU utilization. Simulation and prediction retain
the fixed 1 ms step.

**Performance** overlaps frontend preparation, geometry uploads and submission,
uses three Vulkan frames in flight, and enables asynchronous sun queries and
audio streaming. **Reduced Overlap** adds synchronization, uses two Vulkan frames
in flight, and disables those overlap paths. Both disable VSync and leave FPS
uncapped. These are scheduling presets; texture quality and resolution remain
separate settings. More queued frames can improve throughput while adding latency.
The gameplay build label shows the version, architecture, active renderer,
rendering preset and fixed simulation rate. While aiming in a focused game, the
mouse stays inside the game area in windowed and fullscreen modes. Menus, the
console, switching applications, and minimizing release it.

In-game **Start Server** uses localhost regardless of an older profile's
dedicated/network selection. Use the normal map, game mode, rule, bot-count, and
four difficulty controls. **Leave** and **Quit** stop that game-owned server.
The game chooses an available owned-host port independently of manager hosting.

## Repair and update

Close the installed game, Server Manager, and dedicated engine, then run the
installer again and select the same output folder. Project binaries are repaired
from the embedded payload. Existing retail assets, player profiles, server
profiles, administration policy, bans, and custom configuration are preserved.
Unknown files at project-binary destinations are protected from replacement.
Standalone updates use this installer repair flow; the older development updater
is disabled for this layout.

Copying occurs in a unique sibling stage. Cancellation removes only that stage.
Installation commits only after validation, checksums, and a second running-process
check. Repair keeps backups until commit succeeds and rolls back failed replacement.
If rollback itself fails, its recovery path is reported and its backups are retained.

## Host a server

Choose or create a named profile, then configure the tabs and click **Start Server**.
The status becomes **Running** only after the owned engine acknowledges the loaded
map/mode and actual bound UDP port. Defaults are localhost, UDP **28960**, and
**16** maximum players. Bots yield slots to joining players.

| Hosting mode | Bind address | Connection address |
| --- | --- | --- |
| Localhost | `127.0.0.1` | `127.0.0.1:port` |
| Local network | Selected adapter's IPv4 | Selected adapter's IPv4 and port |
| Internet | Selected adapter's IPv4 | Discovered public IPv4 or manual public override |

Use **Copy address** to share the displayed address. **Join server** starts the
installed game using the local bind address and configured join password.
Internet clients connect directly using cod4x1khz, protocol **10002**. Standard
CoD4/CoD4x clients and public master-list compatibility are outside this package.

The map list comes from installed multiplayer fastfiles. Add map/mode rows, remove
or move them, and choose ordered or shuffled cycles. Each assigned row retains its
mode. Mixed mode chooses independently from the selected supported modes for each
map visit when the server constructs a cycle. Rotation uses separate cryptographic
random draws and never advances gameplay randomness. The supported stock modes are
FFA, TDM, Search and Destroy, Sabotage, Domination, and Headquarters.

Rules include general and per-mode limits, hardcore, health, friendly fire,
respawn, voting, killcam, private slots, RCON, and existing private web administration.
Select a rule, edit its value, and apply it to the form. **Save & Restart** applies
configuration edits. Bot adjustments, player administration, server chat,
**Next map**, and **Restart map** operate live. Player actions carry both the
server session and admission generation; a stale selection cannot target a
replacement player. Bots can be kicked but cannot receive identity bans/admin power.

**Advanced & Logs** provides generated config, editable extra config, import/export,
plugin commands, and a server console. Import preserves settings the form does not
represent. Extra config permits one `set`/`seta`/`sets` or `loadPlugin` command per
line. Nested `exec`, command chaining, path/startup overrides, and conflicting
simulation/protocol settings are rejected. Only installed supported native plugins
can be loaded; the production package excludes regression plugins and fixture scripts.

Profiles have stable IDs. Changing a port retains the same
`runtime/servers/<id>` home, including `administration/policy.bin`, audit data,
`profile.ini`, `extra.cfg`, and generated `main/manager.cfg`. RCON is disabled by
default. The private web-admin defaults to loopback and remains opt-in.

Closing the manager hides it to the notification tray and keeps its server alive.
Tray actions reopen, stop, or exit. The window's **Exit** button also exits and
gracefully stops its owned engine. A crash reports failure and keeps the saved
profile for a manual restart. Job ownership and authenticated local pipes prevent
Stop from affecting unrelated servers.

## Internet setup and verification

**Refresh public IP** uses [ipify over HTTPS](https://www.ipify.org/). Failure shows
Unavailable; a private adapter address is never substituted for the public address.
The manual override accepts a public IPv4 address.

Automatic setup is optional. The network helper uses
[Windows UPnP port mapping](https://learn.microsoft.com/en-us/windows/win32/api/natupnp/nf-natupnp-istaticportmappingcollection-add)
and [Windows firewall rules](https://learn.microsoft.com/en-us/windows/win32/api/netfw/nn-netfw-inetfwrule).
Only the firewall helper requests elevation. Its rule is scoped to the dedicated
EXE and chosen UDP port; LAN rules limit remote sources to LocalSubnet. The manager
records ownership before adding a router mapping and removes its own mapping on
Stop/Exit. It never replaces unrelated mappings. Firewall rules are retained for
future hosting with the same profile/port.

If automatic setup fails, forward the selected UDP port in your router to the
chosen local adapter and permit that dedicated EXE/UDP port in Windows firewall.
CGNAT or double NAT may require upstream-router changes or ISP assistance. Public
address discovery and successful mapping are separate from verified reachability.
A successful connection from an external client is required to verify Internet
hosting; a second machine is required to verify LAN hosting.

## Build the local installer

Use the tools and dependency preparation in [BUILD.md](BUILD.md):

```powershell
./prepare.ps1 -GameDirectory 'D:\SteamLibrary\steamapps\common\Call of Duty 4'
./scripts/package-installer.ps1
```

This builds the x64 engine with Release IPO, the native desktop applications, a
production bot package, and `results/distribution/cod4x1khz_setup.exe`. Its embedded
CAB manifest contains per-file SHA-256 hashes. `package-report.json` records the
installer hash and payload inventory. `-SkipBuild` repackages already built inputs;
use it only after a successful current native build. No GitHub release is created.

Native unit/regression checks run through `build.ps1`. After packaging, build and
run `khz_install_package_tests` for the actual CAB extraction/installation flow.
`khz_host_controller_integration` is an explicit integration target requiring game
assets; it exercises real owned servers, a 60-second clock measurement, live
controls, port conflicts, natural assigned/random rotation, and crashes. The final
local validation report records interface checks, gameplay measurements, and the
verification status of LAN/Internet connections.

See [INSTALLER_VALIDATION.md](INSTALLER_VALIDATION.md) for the local artifact hash,
measured results, and deferred checks. Production packaging rejects compiled
benchmark credentials; developer owned-host probes provide their RCON credential
explicitly through `KHZ_BENCHMARK_RCON_PASSWORD` in the child environment.
