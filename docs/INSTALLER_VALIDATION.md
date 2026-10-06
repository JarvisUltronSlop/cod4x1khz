# Windows installer validation

Latest local validation on October 6, 2026, for `vulkan-testing`, using Windows,
Ryzen 7 9700X, Radeon RX 9070 XT, and the MSVC/CMake Release x64 toolchain.

The local artifact is `results/distribution/cod4x1khz_setup.exe` (67,906,560 bytes).
SHA-256:

```text
512773a48bb55b90d77b21bd2c69f5646c07778a67b65e378b768fdfec9cf71b
```

The package inventory and individual payload hashes are recorded in
`results/distribution/package-report.json`. Installation instructions are in
[INSTALLER.md](INSTALLER.md). No GitHub release was created.

## October 6 corpse timing, Vulkan pixel positions, scoreboard and sound reuse

- Ragdoll initialization cleared the retail 32-bit byte count, leaving part of
  the larger native x64 pool uncleared. Initialization now clears the complete
  typed array. A production-code regression fails with the old implementation
  and passes across three map lifecycles, including the last of all 32 bodies.
- Ragdoll velocity used an earlier `Com_Frame` duration instead of the elapsed
  time between the two presented animation poses. The native regression
  reproduces a 60 ms pose change being treated as 1 ms. Actual close-up Bog
  matches also reproduced excessive corpse bounds before the correction
  (188–260 units). Velocity capture now uses those pose timestamps, and cosmetic
  ragdoll physics advances once per new presentation time. Server simulation
  remains at 1 kHz. Repeated timestamps and map-time resets are covered.
- Vulkan's fragment coordinates include a half-pixel offset that the stock
  DX9 `VPOS` semantic does not. Stock shader loads now receive the original
  integer pixel position, while later depth-bias lowering keeps raw depth.
  The real-GPU DX9/Vulkan comparison fails before this change and passes after,
  including a nonzero viewport origin. Shader cache versioning invalidates the
  previous translation. The full GPU material, frame-flight and retirement
  checks passed with synchronization validation and **zero validation errors**.
- `r_showMissingLightGrid` previously defaulted to true: its documented engine
  behavior is rainbow colors for entities outside the light grid. It now
  defaults to false. Fresh native gameplay confirmed that both light-grid debug
  displays are disabled, with the gold AK and hands captured in normal lighting.
- `+scores` and `-scores` now update presentation-owned client state. Real native
  Tab down/up events showed all **16 players/bots** and hid the scoreboard on
  release. The binding remains `TAB +scores`; captured gameplay verifies that
  the scoreboard is actually drawn.
- A playing or paused OpenAL source rejected detaching its old buffer. Reusing
  that channel could therefore replay the previous weapon's sound. Cleanup now
  stops the source before detaching/deleting the buffer. The production helper
  passes real-device checks for loaded 2D/3D voices, paused/playing replacement,
  new-buffer attachment, repeated stops and two device lifecycles, without
  OpenAL errors.
- Native suites passed **73/73 x64** and **70/70 Win32**. The final presentation
  clock regression also passed on Win32. The installer regression was rebuilt
  against this shipping CAB and passed extraction, repair and profile
  preservation. Its twenty payload files exclude retail assets, benchmark
  executables, regression plugins and the benchmark RCON credential.
- The final Performance Showdown test ran for **60.999 seconds** with 15 bots,
  a gold AK firing at world geometry, a real Tab scoreboard, and traversal of
  a stair/ledge with a **16-unit** height change. It measured **999.939 Hz**
  prediction and **1000.002 Hz** server simulation, within 0.1%. At the endpoints
  client tick debt was **0/3**, server debt **0/0**, snapshot backlog **0/0** and
  event backlog **0/1**. Sampled backlogs never exceeded **0/1**; queue drops and
  GPU presentation failures were zero. It averaged **1,638.608 FPS at 720p**
  in this isolated scene; this is not a general 1080p gameplay guarantee.
- A final **30-second** Bog close-up test fired the gold AK, captured settled
  impacts, and mirrored the same immutable draws into DX9. The impact capture's
  maximum channel difference was **1/255**; all five captured comparisons were
  within **3/255**, with no channels differing by more than 8/255. Ragdoll bounds
  stayed below **40.722 units**; the Showdown run stayed below **41.611 units**.
  No invalid bone coordinates were sampled. Both final clients exited normally
  and Leave stopped their exact owned servers. Comparison/validation runs carry
  substantial diagnostic overhead and are not throughput measurements.
- Tested and embedded SHA-256 hashes:

  ```text
  client 12b7f0a23f73bc06b377e1ffb5f5e71e7af796f957a76fff0763f4ddcdf7c6e4
  server b588a253a9866387556a2bc39ad33ff50a950b4782be5a5362fbec7fe241f904
  ```

Evidence is in `results/bog-corpse-feedback/validation-report.json`, the native
suite/package logs, `vpos-before`/`vpos-fixed`, the failed corpse-bound reports,
`final-performance-showdown`, and `final-bog-impact-compare-layer`. The first
final Bog diagnostic launch exited during startup without the prepared
validation-layer path; that failed report is retained. The fixture now sets
the prepared layer path itself when requesting comparison.

The user's intermittent impact stretching and wall-blood flicker were not
reproduced in these final captures. These checks establish the specific fixes
above, not that every temporal decal artifact is resolved. No desktop automation
or Internet test was performed. The installer is copied into
`userTest/cod4x1khz_setup.exe`; close the game and repair the same output folder
to apply it. The installed executable and player profiles were left untouched.

## October 6 volume slider capture fix

- The user's volume crash reaches `Item_GetEditFieldDef` from
  `Scroll_Slider_SetThumbPos` during menu painting. The capture callback treated
  `scrollInfo_s` as an array of item pointers: index six addressed byte 48 on
  x64, beyond the forty-byte capture, instead of the actual item at byte 24.
  Slider capture now reads the named native member. The list-box drag and
  repeat callbacks had the same pointer-array assumption and additionally
  truncated item pointers; those now use typed members and window geometry.
- A regression links the production callbacks and places capture data against
  a guard page, with the real item allocated above 4 GiB on x64. The old slider
  callback reproducibly faults; the fix passes on x64 and Win32. Coverage also
  checks horizontal/vertical list thumb positions, bounds, cursor visibility,
  unchanged cursor behavior, repeat timing and collapsed list rectangles.
- Native suites passed **71/71 x64** and **69/69 Win32**. The installer regression
  was rebuilt with this shipping CAB and passed extraction, repair and profile
  preservation again. Its twenty payload files exclude retail assets,
  benchmark executables and the benchmark RCON credential.
- Tested and embedded SHA-256 hashes:

  ```text
  client 047374f6243e88f1e307c1dce95ee5cbcdfd8816c202f360f2cea6d6cbd8e068
  server cc86b9b127669735123f6e4c03edbd98bdae982eb3a1aa0e7b2dcc496f81cc3e
  ```

- Both shipping renderers passed stock Sound-menu tests in a live match and the
  disconnected frontend: ten captured drag positions per context, mute/full
  volume and out-of-range clamping, release without further changes, clicking
  the track and archived `snd_volume` persistence. All **4/4 contexts** passed.
  The fixture dispatches native mouse events and holds capture across real
  `Menu_PaintAll` calls; it does not substitute console volume assignments.
- Separate **60-second** Killhouse DM runs with 15 Regular bots measured
  **999.787 Hz** prediction / **1000.012 Hz** server simulation on Vulkan and
  **999.825 Hz** / **999.998 Hz** on DX9, within 0.1%. Final client/server debt
  was **0/0** and **1/0**, respectively. Final event/snapshot backlogs, queue
  failures, GPU presentation failures and Vulkan validation errors were zero.
  Both clients exited normally; Leave stopped their exact owned servers.

Evidence is in `results/volume-slider-feedback/validation-report.json`, the user
crash report, `old-capture-test.log`, suite/package logs, and
`vulkan-mouse`/`dx9-mouse` native reports and menu screenshots. Earlier fixture
failures are retained: the probe's expected server path and Sound-category
selector were corrected before the successful runs. Those corrections changed
the diagnostic script, not the slider implementation. This does not establish
every map/effect or gameplay-stutter cause. No desktop automation or Internet
test was performed. The updated installer is copied into `userTest`; close the
game and repair the same output folder to apply it. The installed executable
and player profiles were left untouched.

The following sections describe earlier builds and their validation.

## October 6 visible Esc menu and Overgrown effects cleanup

- The stock team menu's Back row uses a bottom screen anchor. Cloning that row
  for Game Menu retained its anchor, putting all six labels below the screen.
  Owned rows now use the title's screen placement, with explicit text offsets
  and opaque text/focus colors. Native screenshots from both Vulkan and DX9
  show Resume, Change Team, Change Class, Options, Leave Game and Quit.
- The earlier menu test checked flags and actions but missed actual painting.
  `scripts/native_pause_menu_probe.py` now checks physical text bounds, captures
  the rendered menus, dispatches Esc through the engine, enters Options and
  Resume, and activates the stock Leave confirmation's Yes action. Leave stopped
  the exact owned server and returned to the frontend in both shipping runs.
- The user's Overgrown crash report reaches `FX_FreeElem` through `CG_Fx` and
  effect release. Entity pose cleanup used a retail eight-byte clear; on x64
  the effects pointer lives at offset eight in a sixteen-byte member, so the
  pointer survived after its reference was released. Cleanup now clears each
  native typed pose member, clearing the effects reference before releasing it.
  The production cleanup is linked directly into a regression exercising an
  allocation above 4 GiB, retirement, reuse and prevention of a second release.
  The previous implementation fails that regression; the fix passes x64 and
  Win32, including player, turret, helicopter and vehicle pose resets.
- Native regression suites passed **70/70 x64** and **68/68 Win32**. The installer
  extraction/repair/preservation suite was rebuilt with the shipping embedded
  CAB and passed again. Its twenty payload files exclude retail assets,
  benchmark executables and the benchmark RCON credential.
- The exact shipping binaries completed separate **60-second** Overgrown TDM
  runs with 15 Regular bots and the Performance preset. Vulkan measured
  **999.902 Hz** prediction and **1000.002 Hz** server simulation; DX9 measured
  **999.916 Hz** and **999.997 Hz**, respectively. Both remain within 0.1%.
  Final client/server tick debt was **1/0** in each run. Final event/snapshot
  backlogs, queue failures, GPU presentation failures and Vulkan validation
  errors were zero. Both clients exited normally and both owned servers stopped.
- Tested and embedded SHA-256 hashes:

  ```text
  client 18105c8db71a0d032b8b97cdb234bcd0863b614a36f100b74fc869c2b11a2023
  server e9bdeab515a0866157f39381af0c6b55628dee8198b92b4d88a4dfb4212f28e9
  ```

Evidence is under `results/pause-overgrown-feedback`: `validation-report.json`,
`user-overgrown-crash.txt`, `entity-pose-before.log`, native suite logs,
`shipping-package-tests.log`, and `shipping-vulkan`/`shipping-dx9` reports and
menu screenshots. Fixed-view bot runs did not reproduce the user's exact crash;
the production regression reproduces the stale effects reference defect.
These checks do not establish that every map/effect or gameplay stutter is fixed.
No desktop automation or Internet test was performed. The installer is copied to
`userTest/cod4x1khz_setup.exe`; the installed game and player profiles were left
untouched. Close the game and repair the same output folder to apply this build.

## Earlier October 6 pause, movement, impact and audio fixes

- Esc opens **Game Menu** with Resume, Change Team, Change Class, Options,
  Leave Game and Quit. Native diagnostic key events and stock item actions
  verified Options, Resume and the Leave confirmation. A separate Vulkan Wetwork
  Free-for-All match used that confirmation's Yes action to stop its exact owned
  server and return to the main menu.
- ADS movement's first 1 ms acceleration is below the retail 1 unit/second
  friction cutoff. The cutoff now scales with the simulation step, retaining the
  retail threshold at 50 ms. Tests link production friction and acceleration at
  1/2/5/10/50 ms, including slow ADS speeds and stopping after input release.
- Item-touch prediction cannot know a dropped weapon's remaining ammo. At 1 kHz
  it could repeatedly announce an empty compatible drop. Pickups now use the
  authoritative server event for sound and removal. The user's exact stairs/ammo
  pile still requires a hands-on retest.
- Impact clipping scratch is private to each effects thread; triangle/point
  copying stays under the allocator lock. A synchronized concurrent-impact
  regression reproduced overwritten geometry with shared scratch and passed
  after the change. Additional production tests cover native pool strides,
  context changes, 1/2/3/31/254/255 clipped triangles, index carry between marks,
  LRU eviction and exhausted/oversized mesh reservations. Failed reservations
  leave allocation cursors unchanged.
- Bot commands already run every millisecond. Navigation stays on a timed 50 ms
  script loop. Obstacle recovery now measures elapsed time, avoids exact equality
  and modulo triggers, and starts bounded mantle retries after 500 ms. The ADS
  friction fix also applies to bots. Every map waypoint, stair and ledge has not
  been individually verified.
- Normal startup runs the optional player autoexec instead of the asset bundle's
  developer autoexec. Fresh native matches reported **developer=0** and
  **com_statmon=0**, removing developer warning notifications and the GLOW label.
  Dynamic-light overflow already checks the 32-light bound before writing;
  diagnostics remain in the console/log.
- Disconnect stops map sounds before their aliases unload and clears pending
  physics audio. Wetwork had **33 cgame channels**, including streamed audio,
  before Leave. Five subsequent main-menu samples had **zero cgame channels**;
  the stock UI music remained active. This exercised async stream decoding.
- Native regression suites passed **69/69 x64** and **67/67 Win32**. The installer
  suite was rebuilt with the final embedded CAB and passed extraction/repair/
  preservation checks again.
- A Vulkan Performance Showdown TDM run with 15 Regular bots invoked both allies
  and axis stock seven-kill helicopter rewards, flight, actual explosive damage
  (4,000 damage each), crash paths and final destruction. **2/2** completed with
  no client/server crash, script runtime error, Vulkan validation error or GPU
  presentation failure. The **119.833-second** interval measured **999.868 Hz**
  prediction and **1000.003 Hz** server simulation, within 0.1%. Client/server
  tick debt was **0/0** at both endpoints; final event/snapshot backlogs and
  queue failures were zero.
- The helicopter and Wetwork runs used the exact client/server hashes embedded
  in this installer. Temporary script instrumentation was restored and is
  excluded from the production payload. Native fixture assertion corrections
  (full map asset paths and the expected UI music stream) are recorded alongside
  their original failed reports, rather than represented as engine crashes.

Local evidence is in `results/gameplay-feedback/validation-report.json`,
`gameplay-summary.json`, `helicopter-audio-vulkan-final/report.json`,
`wetwork-audio-verified/report.json`, the regression logs, and `package-tests.log`.
The diagnostic 720p viewpoint is not a matched 1080p player performance benchmark.
Showdown's perceived prop/impact warping, all bot obstacle cases, and gameplay
stutter causes are not fully verified by these tests. No desktop automation or
Internet test was performed. The updated installer is also copied into
`userTest/cod4x1khz_setup.exe`; repairing the user's installed output remains a
manual action that preserves profiles.

## October 6 airstrike and mouse fixes

- The installer description ended below the first folder caption, covering its
  upper edge. Its bounds now leave a gap before that caption, including at the
  125% scaling shown in the user's screenshot.
- The new user crash report identifies `FX_SpawnModelPhysics` during bolted
  airstrike effects. That code read the physics preset using pointer index 53
  from a former 32-bit model layout. Native x64 models store that field at a
  different offset; the code now accesses `XModel::physPreset` by name.
  A guard-page regression links the actual production function and exercises
  successful creation, native body-pointer retention, and allocation exhaustion.
- Focused gameplay confines the Windows cursor to the client area while Raw
  Input continues relative aiming. Menus, the console, focus loss, minimization,
  window sizing, and shutdown release capture. Moving or resizing recomputes
  bounds, including negative coordinates and secondary monitors. Capture stays
  on the presentation thread; it adds no Windows API work to simulation ticks.
  Regression coverage checks those transitions and preserves another window's
  capture/clip ownership during focus loss.
- Native suites passed **67/67 x64** and **65/65 Win32**. The embedded-CAB
  installation suite was rebuilt against this installer and passed again.
- A localhost Vulkan/Performance Showdown TDM match with 15 Regular bots remained
  alive for **123.136 seconds**. A temporary development fixture invoked the
  stock airstrike function three times; all **3/3** completed, with no script
  runtime errors, Vulkan validation errors, or GPU presentation failures.
  This tests the stock effect path without automating the target-selection menu.
- The **119.837-second** telemetry interval measured
  **999.837 Hz** prediction and **1000.003 Hz** server
  simulation, within the 0.1% tolerance. Client/server debt was **0 / 0** at both
  endpoints; final event and snapshot backlogs and queue failures were zero.
  Leave stopped the exact owned server; Quit exited the client normally.
- The tested client/server hashes match the embedded payload. The temporary
  airstrike fixture was removed and the original development bot package restored.
  The installer rebuild uses production bot sources and excludes fixture scripts.

Exact telemetry, binary hashes, and gameplay logs are retained in
`results/airstrike-feedback/gameplay-summary.json` and
`stock-airstrike-final/report.json` under that result folder; test logs are
`x64-tests.log`, `win32-tests.log`, and `package-tests.log` there.
The installer in `userTest/cod4x1khz_setup.exe` has the SHA-256 recorded above.
Repairing the existing output remains a manual user action and preserves profiles.

No new desktop UI automation was performed. Visual confirmation of the revised
caption and hands-on cursor/Alt-Tab testing remain for the user's retest.
The localhost run does not establish every map/effect or perceived-stutter cause.
The earlier user-test and interface measurements below describe prior builds.

## October 6 user-test fixes

- Both installer folder fields are editable immediately. **Install** opens one
  brief confirmation; the staged install/repair behavior is unchanged.
- Installer and Server Manager use a fixed dark palette, including controls,
  tabs, tables, app-owned notices and the tray menu. There is no light-mode option.
  Windows-owned file/folder pickers retain the operating system theme.
- Rendering presets are **Performance** and **Reduced Overlap**. Their saved
  numeric values and scheduling behavior are unchanged. The gameplay build label
  includes `cod4x1khz`, `v0.2.0`, architecture, active renderer, preset and 1,000 Hz.
- The user's renderer crash was an undersized x64 entity-cell allocation:
  `GfxEntCellRefInfo` requires eight bytes per entry, while the old allocation and
  clearing assumed four. Native guard-page coverage exercises all 1,024 IDs.
- Setting a HUD archival flag also stored an eight-byte `VariableUnion` in a
  four-byte field, overwriting the next live HUD element. The native setter now
  writes the actual field width; adjacent-element regression coverage verifies it.
- A fresh localhost Vulkan/Performance Showdown TDM match with 15 Regular bots
  completed **120.936 seconds**, with no process crash or HUD script errors.
  A steady **117.736-second** telemetry interval measured **999.909 Hz** prediction
  and **999.999 Hz** server simulation. Client/server debt was **0 / 0** at both
  interval endpoints, within the existing 0.1% frequency tolerance. Leave and Quit
  stopped the exact owned server and exited the client normally.
- The x64 native suites passed **65/65** and the Win32 core suites **63/63**.
  The final embedded-CAB installation suite was rebuilt against this payload.
  Repair, cancellation, dependency recovery, profile/admin/ban preservation and
  running-process refusal are covered by that suite.

The updated installer is also copied to `userTest/cod4x1khz_setup.exe` for manual
repair/retesting. The user's installed `userTest/output` was not automatically
replaced. Logs and exact gameplay binary hashes are in
`results/user-test-feedback/gameplay-summary.json`, `showdown-final/report.json`,
`x64-tests.log`, `win32-tests.log`, and `package-tests.log` under that result folder.

The dark appearance and revised wizard interactions were source-reviewed and
built; no new GUI automation was performed after the user stopped computer use.
The localhost stress run confirms the two corruption fixes and clock behavior;
it does not establish that every cause of perceived stutter is eliminated.

The following interface checks and matched rendering measurements were performed
on October 5, before these feedback fixes.

## Installation and application checks

- The actual wizard detected the Steam library, copied the installed retail game
  into a fresh folder containing spaces, and exposed the root game and manager.
  Repair through the wizard preserved existing player and server profiles.
- The native installation suite exercised asset/destination validation, overlap,
  cancellation cleanup, actual embedded-CAB extraction, missing-DLL repair,
  corrupted-binary repair, profile/admin/ban/custom-config preservation, locked
  replacement rollback, running-process refusal, and unowned-file protection.
- Normal installed game launch opened multiplayer. Graphics-menu renderer and
  preset changes restarted graphics and persisted. A forced unavailable Vulkan
  ICD opened the normal menu using DX9 instead.
- An old `ui_dedicated=2` selection was forced to localhost. Menu-configured
  Backlot matches used the selected mode, two Hardened bots, and a separate
  loopback-owned server. Leave and Quit stopped that server while a manager-owned
  server remained alive. Manager Join connected the installed client locally.
- Manager Start/Stop, actual loaded-map/port status, graceful Exit, hide-to-tray,
  and reopening the existing hidden instance were exercised. Changing the port
  retained the stable server profile ID. LAN binding to the selected adapter
  reached Running; this alone does not verify another machine can join.
- The native owned-server integration suite exercised live bot counts/difficulty,
  stale admission/session rejection, bot administration restrictions, unsupported
  command rejection, occupied-port isolation, Restart Map, naturally completed
  assigned and shuffled/mixed rotations, per-mode rules, and crash reporting.
  Its independent steady-clock interval measured **1,000 Hz over 60.188 seconds**.
- The production payload has only AMD64 binaries. It excludes retail assets,
  benchmark executables, regression plugins, development fixture scripts, and the
  benchmark RCON password. Binary credential scans and production-bot source
  checks run during packaging. Developer owned-host probes now supply their
  credential explicitly through a child-process environment; that round trip
  was tested successfully. Production RCON defaults to disabled.

The October 5 x64 regression run passed **63/63** checks; the Win32 core run passed
**61/61**. Local logs include `results/installer-final-regression.log`,
`results/installer-win32-regression.log`, `results/hosting-integration/report.txt`,
and `results/installer-credential-report.json`.

## Rendering and clock measurements

These runs used the same fixed broad Backlot view at 1920×1080, two bots,
uncapped rendering, and the throughput settings. Only intervals with the actual
in-match camera and both bots present were counted. Each interval exceeded
60 seconds. The manager was open and idle for the indicated runs.

| Renderer / condition | Measured seconds | Rendered FPS | Prediction Hz | Server Hz | Final client/server tick debt |
| --- | ---: | ---: | ---: | ---: | ---: |
| Vulkan, manager closed | 63.740 | 845.10 | 999.836 | 999.999 | 2 / 0 |
| Vulkan, manager open | 78.506 | 855.63 | 999.911 | 1000.005 | 3 / 0 |
| DX9, manager open | 87.007 | 676.99 | 999.888 | 1000.008 | 2 / 0 |
| Final shipping Vulkan binaries, manager open | 78.551 | 863.53 | 999.878 | 999.995 | 2 / 1 |

The matched Vulkan manager-open/closed comparison changed FPS by **+1.25%**;
it showed no material GUI rendering regression. The final Vulkan follow-up used
the exact packaged game/server hashes after removing the implicit benchmark
credential. The preceding paired Vulkan and DX9 runs used the same renderer code
before that startup-only credential cleanup.

Simulation and prediction stayed within the existing **0.1%** frequency tolerance.
Transient scheduler debt recovered rather than accumulating. GPU utilization in
the paired Vulkan runs averaged approximately **55%**, measured using the game's
busiest Windows GPU engine counter. These scene measurements establish neither
a universal 1,000 FPS guarantee nor full GPU utilization. They do not replace
renderer testing across every map, effect, and graphics setting.

Raw telemetry, camera evidence, query timeouts, exit codes, and binary hashes are
in `results/installer-render-*.json`. Startup/loading and process-shutdown query
timeouts are retained; they are excluded from the sustained camera interval.

## Deferred checks

Internet testing is **deferred at the user's request**, including public-IP
discovery, router mapping, firewall elevation, and an external client connection.
No Internet reachability is claimed. LAN access from a second machine is also
unverified. The network controls and address validation are implemented, with
manual setup and reachability instructions in [INSTALLER.md](INSTALLER.md).

Tray context-menu actions, DPI changes across different monitors, and every
supported administration/plugin action have not each received an individual
end-to-end GUI test. Shared controller/configuration/administration regression
coverage is recorded above. New upstream CoD4x capabilities remain outside this
installer and manager implementation.
