# Build 71: four short match measurements

The full timing gate is **not passed**: two of four 60-second cases meet the
strict server-work p99 limit below 1,000 microseconds. All four pass simulation,
each human's unique command/snapshot and prediction rates within 0.1% of 1,000/s,
native bot controls, collision ownership, script-error, independent-owner,
GPU-present, bounded debt and binary/asset provenance checks. Failed cases are
retained without rounding the threshold or replacing results with a better run.

All cases use the frozen [Build 71 archive](../results/build71-source-and-binary-hashes.json),
Direct3D 9, 640x480, uncapped rendering and VSync off. Tests use isolated benchmark
homes and execute serially. No builds, test suites, hash sweeps or service fixtures
overlap the measured intervals. Every exact owned process exits; no game process
remains after the batch.

| Map | Humans / bots | Server Hz | Work p99, us | Actual GPU presents/s | End / maximum sampled debt | Work gate | Actual report |
|---|---:|---:|---:|---:|---:|---|---|
| Killhouse | 1 / 15 | 999.994867 | 1,017 | 803.40 | 1 / 4 | Fail | [KH 1](../results/mp_killhouse-60s-uncapped-480p-20261003-065035.json) |
| Crash | 1 / 15 | 999.992950 | 935 | 1,038.50 | 0 / 2 | Pass | [Crash 1](../results/mp_crash-60s-uncapped-480p-20261003-065211.json) |
| Killhouse | 4 / 12 | 1,000.000050 | 997 | 744.24 | 0 / 1 | Pass | [KH 4](../results/mp_killhouse-60s-uncapped-480p-20261003-065413.json) |
| Crash | 4 / 12 | 999.999700 | 1,020 | 1,286.99 | 0 / 0 | Fail | [Crash 4](../results/mp_crash-60s-uncapped-480p-20261003-065558.json) |

Each report includes per-human rates, bounded-memory samples, CPU/thread use,
bandwidth, prediction/collision counters and its complete pre-launch/post-cleanup
provenance comparison. The one-human Killhouse report has zero prediction
corrections, 100% snapshot-owned collision use, no legacy collision calls and no
script errors. Those are measured case facts, not comprehensive movement or
combat coverage.

Average busiest GPU-engine utilization is 21.40%, 22.93%, 19.96% and 24.08% in
the table's order. These low-resolution results do not demonstrate GPU saturation.
Rendering reaches above 1,000 FPS on these Crash runs but remains below it on
Killhouse; presentation performance is separate from the 1 ms gameplay contract.
The moving matches differ in inputs, state and viewpoints, so their FPS variation
cannot establish render independence or substitute for full-world replay.

The current archive records a native killcam snapshot every gameplay tick. Its
1,200 descriptors therefore represent only about 1.2 seconds at 1 kHz. A separate
timestamped presentation archive requires committed-descriptor/cache guards,
overflow handling, logical killcam delay preservation and map-lifetime reset;
it must leave authoritative gameplay, human network updates and hit rewind at
1 kHz. That correction is pending, alongside the
[native timestep findings](native-timestep-audit.md), whole-world replay and
complete public capability evidence.
