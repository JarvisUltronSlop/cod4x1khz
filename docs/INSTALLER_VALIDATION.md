# Windows installer validation

Latest local validation on October 6, 2026, for `vulkan-testing`, using Windows,
Ryzen 7 9700X, Radeon RX 9070 XT, and the MSVC/CMake Release x64 toolchain.

The local artifact is `results/distribution/cod4x1khz_setup.exe` (67,897,856 bytes).
SHA-256:

```text
d1dde092e0a3631bd7cb0d68da986c6c023dd3e9f4a05074f2841793be65da84
```

The package inventory and individual payload hashes are recorded in
`results/distribution/package-report.json`. Installation instructions are in
[INSTALLER.md](INSTALLER.md). No GitHub release was created.

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
