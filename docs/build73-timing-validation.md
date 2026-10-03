# Build 73 timing and recovery

The four requested 60-second hardware timing cases pass on the frozen Build 73
client/server pair. Direct3D 9 rendering was uncapped at 640×480 with VSync off.
Each case used fifteen or twelve Regular Bot Warfare bots and one rendered
human; the four-human cases added three headless protocol clients. All created
processes exited and provenance checks passed. This is a timing result, not the
complete public-feature or full-world replay release gate.

| Map | Humans / bots | Server work p99 | Actual rendered FPS |
| --- | --- | --- | --- |
| Killhouse | 1 / 15 | 821 µs | 769.47 |
| Crash | 1 / 15 | 878 µs | 1,079.37 |
| Killhouse | 4 / 12 | 858 µs | 828.00 |
| Crash | 4 / 12 | 878 µs | 1,125.62 |

Every case passes simulation and each human's unique command/snapshot rate
within 0.1% of 1,000/s, strict p99 below 1,000 µs, no growing sampled debt,
native bot admission/control, independent client owner, native snapshot
collision with no legacy fallback, script and renderer checks. Each sixty-second
native interval adds exactly 1,200 archive commits at 20 Hz, with no overflow,
skipped recording opportunity or invalid index. The custom gameplay and human
transport clocks remain at 1 kHz. Archive work is separately recorded; its p99
is 134 / 192 / 133 / 182 µs respectively and overlaps the outer snapshot phase.

[Consolidated results](../results/build73-archive-matrix-validation.json) retain
all full reports, per-human rates, debt, exact source/binary/asset provenance and
adjacent archive validations. The immutable
[build archive](../results/build73-source-and-binary-hashes.json) records forty
passing native suites. Build 71's two p99 failures remain historical evidence;
the new result does not overwrite them.

The current strict lifecycle run passes host, Crash map change, fast restart,
full restart, graphics restart, audio restart and explicit Leave to a visible
stock menu with the exact owned server stopped. The initial run failed on one
unguarded post-Leave diagnostic timeout during native asset reload; its report
is retained. The new bounded fixture preserves the original ten-second Leave
and ninety-second overall limits, records retries and binds its executable
script separately from the unchanged archived helper. Its successful rerun
needed no Leave retry. A separate Regular DM fixture retains all 16,000 selected
command records from 1,000 actual ticks and passes native bot identity/skill
inspection and normal cleanup.

Remaining gates include the actual archive full/delta codec and live killcam,
all difficulties/modes and objectives, other timestep sites, fault/service
recovery, current rendering cap stages and renderer-active all-domain world
replay. Classifier/GUID SDK calls have separate scoped evidence. Uncapped FPS is
measured; a universal 1,000 rendered FPS claim is not established.
