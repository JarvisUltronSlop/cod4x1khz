# Windows installer validation

Local validation on October 5, 2026, for `vulkan-testing`, using Windows,
Ryzen 7 9700X, Radeon RX 9070 XT, and the MSVC/CMake Release x64 toolchain.

The local artifact is `results/distribution/cod4x1khz_setup.exe` (67,871,744 bytes).
SHA-256:

```text
46e8604c65dd67cd83e8d4eb37b803ae56fdf577855b57dcc59947204d1086c5
```

The package inventory and individual payload hashes are recorded in
`results/distribution/package-report.json`. Installation instructions are in
[INSTALLER.md](INSTALLER.md). No GitHub release was created.

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

The final x64 regression run passed **63/63** checks; the Win32 core run passed
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
