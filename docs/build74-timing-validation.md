# Build 74 timing

All four requested 60-second cases pass on the archived Build 74 pair, with
Direct3D 9 at 640×480, rendering uncapped and VSync disabled. One human renders;
the four-human cases add three benchmark clients. Regular Bot Warfare fills
the remaining slots to sixteen total players.

| Map | Humans / bots | Server work p99 | Actual rendered FPS |
| --- | --- | --- | --- |
| Killhouse | 1 / 15 | 836 µs | 850.69 |
| Crash | 1 / 15 | 770 µs | 1,174.85 |
| Killhouse | 4 / 12 | 857 µs | 711.55 |
| Crash | 4 / 12 | 896 µs | 1,583.13 |

Simulation and every human's unique command/snapshot rate pass the requested
1,000/s ±0.1% limit. Server work p99 is below 1 ms in every case, without growing
sampled debt. Native bot control, the independent client simulation owner,
snapshot collision without a legacy fallback, scripts, rendering and provenance
also pass. Each native sixty-second interval commits 1,200 presentation archive
records at 20 Hz, with no overflow, skipped opportunity or invalid index. Gameplay
and human command/snapshot transport remain at 1 kHz.

The [matrix validation](../results/build74-archive-matrix-validation.json) links
the original reports, individual human rates and archive phase measurements.
The [build record](../results/build74-source-and-binary-hashes.json) binds the
source, assets and deployed binaries; all forty native suites pass. Exact copies
of the binaries and bot package are retained in `results/build74-release-binaries`.

These results apply to Build 74. Build 75 changes the diagnostic archive reader
and has separate codec evidence. Neither result completes public-feature
integration, controlled bot combat/objectives or renderer-active full-world replay.
