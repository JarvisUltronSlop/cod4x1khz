# cod4x1khz 0.2 — x64, Vulkan, installer and Server Manager

Release 0.2 upgrades the 0.1 / Build 86 DX9 checkpoint to a native **64-bit Windows
client and dedicated server**, adds a **native Vulkan renderer alongside DX9**,
and introduces a standalone installer with an included Server Manager.

Authoritative simulation and client prediction retain their fixed **1,000 Hz**
clock. Rendering runs independently and defaults to uncapped. This release
includes the subsequent gameplay, rendering, audio and interface fixes through
game implementation commit `94d1932238d500013f7607978982ddd993f64ff4`.
The game identifies this release as **v0.2.0**.

## Download and install

Download **cod4x1khz_setup.exe** from this release. You need 64-bit Windows and
your own installed Steam copy of **Call of Duty 4: Modern Warfare (2007)**.
Modern Warfare Remastered is not a compatible asset source.

1. Open the installer and select your Steam CoD4 folder.
2. Select a separate output folder, click **Install**, and confirm.
3. Launch **cod4x1khz.exe** to play or **cod4x1khz_server_manager.exe** to host.

The installer copies retail assets from your Steam installation without modifying
it. The download contains project binaries, runtime dependencies, bots and
license notices; retail game assets are not embedded. No compiler, Python,
.NET, Qt or Electron installation is needed to play.

To update an existing standalone installation, close its applications and rerun
the installer using the same output folder. Repair preserves player/server
profiles, bans, administrators and custom configuration. For a 0.1 manual-layout
installation, choose a new standalone output folder.

[Installation, repair and hosting guide](https://github.com/JarvisUltronSlop/cod4x1khz/blob/v0.2/docs/INSTALLER.md)

## Graphics and performance

- Added a native Vulkan 1.3 backend, stock shader translation to SPIR-V, native
  resources, material pipelines, texture uploads, GPU queries and pipeline caches.
  DX9 remains selectable and serves as the fallback if Vulkan initialization fails.
- Added parallel rendering preparation and command recording with up to four
  workers, cached material/descriptor state, immutable draw packets, geometry
  reuse and compatible draw batching. Bounded asynchronous uploads/submission,
  timeline-based resource retirement, overlapping frontend work and asynchronous
  sun queries reduce unnecessary CPU/GPU waits.
- Fresh installed profiles select **Vulkan / Performance**. **Options → Graphics**
  exposes the renderer and the **Performance / Reduced Overlap** presets and
  saves subsequent choices. Performance uses three Vulkan frames in flight;
  Reduced Overlap uses two and adds synchronization. These scheduling presets
  are separate from resolution and texture quality.
- Rendering remains uncapped by default. `/com_maxfps 1000` sets a 1,000 FPS cap;
  `/com_maxfps 0` removes it. Changing the render cap does not change the 1 kHz
  simulation clock. Extra frame overlap can improve throughput while adding latency.
- Corrected Vulkan texture-channel defaults, volume-texture transfers, shadow
  sampling, depth bias, particle discard/derivative handling, viewport/depth
  behavior and DX9 pixel-coordinate translation. Shader cache versioning
  invalidates affected old translations.

## Native 64-bit engine

- Converted the production client, dedicated server, updater and native plugin
  interface to AMD64 / PE32+, with architecture-specific build and runtime paths.
- Adapted the retail 32-bit asset format to native structures: 150 asset schemas
  with 1,641 fields cover typed records, pointers, aliases and delayed fixups.
  Original game files retain their existing format.
- Reworked native pointer widths and structure sizes across memory pools,
  scripting/compiler/VM state, UI/HUD, model/effect storage, collision, renderer
  commands, filesystem lists and restart archives. Added checks using allocations
  above 4 GiB and replaced x86 MMX skinning with SSE2.
- Integrated native OpenAL audio and FFmpeg cinematic decoding, asynchronous
  stream prefetch, native dependency packaging and Steam shutdown cleanup.
  Native plugins use SDK3; the existing Win32 interface remains SDK2.
- Added bounded input-timestamp handling and native lifecycle/profiling tools.
  Source build tooling also fixes the fresh-clone results-directory assumption
  discovered after 0.1.

## Installer and desktop applications

- Added **cod4x1khz_setup.exe**, which detects Steam libraries, accepts Browse
  selections, and validates assets, destinations, path limits, permissions and
  space before installation. Both folder fields can be edited immediately.
- Added staged installation, cancellation cleanup, per-file checksums, rollback,
  repair, dependency recovery and refusal to replace running applications.
- The game and Server Manager appear at the output root; assets, the dedicated
  engine, helpers and plugins live beneath `runtime`. The game retains its blue icon.
- Installer and Server Manager use a fixed dark theme with blue accents, themed
  controls, keyboard navigation and DPI scaling. Revised installer confirmation,
  wording and caption spacing address the first user-test feedback.
- The gameplay build label now identifies the version, architecture, renderer,
  rendering preset and simulation rate.

## Included Server Manager

- Localhost, LAN and Internet hosting modes; adapter selection, editable UDP port,
  server name, password and player limit. Defaults are localhost, port **28960**,
  and **16 total player/bot slots**, with Copy Address and Join controls.
- Installed-map discovery, editable ordered or shuffled rotations, assigned
  per-map modes, or random choices from a selected mode pool. Supported stock
  modes are FFA, TDM, Search and Destroy, Sabotage, Domination and Headquarters.
- General/per-mode rules, score/time limits, hardcore, health, friendly fire,
  respawn, voting and killcam; live bot count and Recruit, Regular, Hardened and
  Veteran difficulty controls. Bots yield slots to joining players.
- Player/bot lists; supported kick, temporary/permanent ban, unban, admin-power,
  chat, console, Next Map and Restart Map controls. Admission identifiers protect
  against a stale player selection affecting a replacement player.
- Named profiles with stable IDs, generated configuration, editable extra config,
  import/export that preserves unrepresented settings, supported plugin controls
  and private web-admin settings. Simulation/protocol overrides are rejected.
- **Save & Restart** for configuration changes, persistent Start/Stop/status,
  tray operation, graceful owned-server shutdown, crash reporting and readiness
  based on the actual loaded map/mode and bound port. Authenticated local pipes,
  peer-PID checks and process jobs keep ownership scoped to the correct server.
- Public-address discovery/manual override, optional UDP UPnP mapping and scoped
  Windows firewall assistance. Only the firewall helper requests elevation.
  Address discovery and port mapping are distinct from verified reachability.
- Ordinary in-game **Start Server** always creates a localhost match using the
  menu's maps, rules and bot settings, including with old dedicated/network
  profile settings. Leave/Quit stops its owned server. Game-owned and
  manager-owned servers can operate independently.

## Gameplay, rendering and audio fixes from testing

- Restored the **Esc game menu**, including Options, Resume, team/class selection,
  Leave and Quit, and corrected off-screen menu positioning. Restored the **Tab
  scoreboard**. Fixed native slider/list capture, including the volume-change crash.
- Confined the mouse to the focused gameplay window while aiming. Menus, the
  console, focus loss and minimizing release capture; moving/resizing updates bounds.
- Corrected the 1 ms friction cutoff that nearly prevented ADS strafing. Bot
  obstacle recovery now uses elapsed time and bounded mantle retries for stairs,
  lips and ledges. Bot controls run every millisecond; navigation decisions retain
  their timed 50 ms script cadence.
- Changed dropped-item pickup notifications to authoritative server events to
  prevent repeated predicted pickup sounds when standing on empty ammo drops.
- Fixed x64 model-physics access used by airstrike debris, stale effect references
  implicated in the Overgrown crash, undersized entity-cell storage, and adjacent
  HUD-element corruption. Exercised both teams' helicopter call-in, damage and
  destruction paths and repeated stock airstrikes.
- Corrected ragdoll pool clearing and pose-time velocity calculations, and limited
  cosmetic ragdoll updates to new presentation times to address stretched bodies.
- Fixed native mark-pool strides, concurrent impact clipping scratch, bounded
  mesh reservations and decal context handling. Most recently, world-mark workers
  and main-thread model marks now share the same CPU staging buffer, preventing
  valid blood/bullet-impact vertices from being overwritten before GPU upload.
- Disabled missing-light-grid rainbow coloring and developer warning overlays in
  normal startup, including the GLOW label and on-screen dynamic-light warnings.
- Stopped map audio before unloading aliases on disconnect, fixing lingering
  Wetwork rain/announcements in the frontend. Stopped/detached OpenAL sources
  correctly when reusing channels, fixing previous-weapon sounds on a new weapon.

## Validation and current limits

The packaged build was validated on October 6, 2026, on a Ryzen 7 9700X and
Radeon RX 9070 XT. The latest native suites passed **74/74 x64** and **71/71
Win32**, and the final embedded-installer suite passed extraction, repair,
rollback and profile-preservation checks. The installer contains the exact
client/server binaries used in the final gameplay runs.

The final Showdown check used 15 bots, wall impacts and stair traversal. Its
**60.998-second** measured interval averaged **1,932.527 FPS at 720p**, with
**1000.010 Hz prediction / 999.991 Hz server simulation**, zero ending tick debt
and zero sampled snapshot/event backlog. A separate Bog wall-impact check had
zero Vulkan validation errors; nine matched Vulkan/DX9 captures differed by at
most 8/255 per channel.

These are scoped scene measurements, not a universal FPS guarantee. Full GPU
utilization, every map/effect, all bot obstacle cases and every possible stutter
or visual artifact are not established as resolved. Internet reachability was
**not tested**, as requested; a connection from an external client remains
necessary. LAN joining from a second machine is also unverified.

This remains an experimental custom client/server release, not complete public
CoD4x feature parity. Protocol **10002** requires compatible cod4x1khz clients;
stock-client compatibility and proprietary authentication remain outside scope.

[Detailed validation](https://github.com/JarvisUltronSlop/cod4x1khz/blob/v0.2/docs/INSTALLER_VALIDATION.md)
· [Source build instructions](https://github.com/JarvisUltronSlop/cod4x1khz/blob/v0.2/docs/BUILD.md)
· [Complete source changes since 0.1](https://github.com/JarvisUltronSlop/cod4x1khz/compare/v0.1...v0.2)

## Installer checksum

`cod4x1khz_setup.exe` — **67,907,072 bytes**, SHA-256:

```text
72a9f1ad8ced9e5461ae74034c626d34065bd65daf2fa3b320a406b530b239c4
```

`SHA256SUMS.txt` and `package-manifest.json` are attached for verification.
