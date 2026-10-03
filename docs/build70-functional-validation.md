# Build 70 functional validation

The frozen [Build 70 source/binary archive](../results/build70-source-and-binary-hashes.json)
passes 39 native suites. These additional tests ran the actual root client and
its managed dedicated server, one human and 15 Regular bots, on the local test
machine. Every fixture used a fresh benchmark home, retained the exact owned
server handle, exited both processes normally and verified that interactive
preferences were unchanged. The earlier
[Build 69 command-barrier failure](../results/native-fov-20261003-112220-353864/result.json)
remains preserved.

## Options and FOV

The [Options stress fixture](../results/native-options-20261003-113517-804019/result.json)
passes three rounds over all 12 stock Options menus: 72 real Open/Back calls.
The longest observed call took 2.62 ms. Native owner identity remained stable,
authoritative acknowledgements, prediction, snapshots, pose and GPU presents
advanced, queues/debt remained bounded and the server reported no new rejected
input timestamps. Archived graphics/audio/input selections and bindings stayed
unchanged. These observed command timings are diagnostic, not a match benchmark.

The [extended FOV/Options fixture](../results/native-fov-20261003-113559-343027/result.json)
passes 15 scoped cases in 55 seconds. Actual rendered hip/ADS geometry and
sensitivity match scales 0.2, 0.5, 1.25 and 2 for the current stock weapon;
nonfinite alias input preserves the selection. The archived 1.25 selection
survives an actual Killhouse-to-Crash change and an authoritative natural death
and respawn. Legitimate weapon changes use that weapon's measured stock optics
after proving selection persistence.

All 12 stock categories pass real enabled setting/binding/navigation actions:
Main Options, Graphics, Texture, Sound, Voice, Game, Multiplayer, Look, Move,
Shoot, Miscellaneous and Multiplayer Controls. The actual enabled confirmation
buttons in `vid_restart_popmenu` and `snd_restart_popmenu` execute their compiled
stock actions. Graphics Apply archives VSync 1 and changes owner generation
2 to 3; Sound Apply archives 11 kHz and changes generation 3 to 4. Both retain
FOV 1.25, the same owned server, active native gameplay and advancing accepted
input/prediction/GPU counters. The fixture restores its FOV selection before
leaving; interactive profiles are separate.

## Presentation caps and fixed gameplay

The [480p report](../results/native-render-caps-480p-20261003-113721-743337/result.json)
and [720p report](../results/native-render-caps-720p-20261003-113838-279757/result.json)
each test 125, 240, 1,000 and uncapped rendering on the same joined client. Each
stage has two seconds of cap warmup followed by eight seconds of sampling.
Actual dimensions are 640x480 and 1280x720 respectively; VSync is disabled.

| Resolution | Console cap | Successful presents/s | Accepted input/s | Server work p99 |
| --- | ---: | ---: | ---: | ---: |
| 480p | 125 | 124.95 | 999.725 | 862 us |
| 480p | 240 | 239.92 | 999.776 | 797 us |
| 480p | 1,000 | 753.52 | 999.782 | 904 us |
| 480p | 0 (uncapped) | 641.76 | 999.806 | 883 us |
| 720p | 125 | 124.83 | 999.893 | 931 us |
| 720p | 240 | 237.79 | 1,000.015 | 897 us |
| 720p | 1,000 | 469.36 | 999.821 | 918 us |
| 720p | 0 (uncapped) | 608.33 | 999.942 | 894 us |

All eight stages pass the short rate/debt/collision checks: approximately 1,000
server ticks, unique human commands/snapshots, client prediction dispatches and
all 15 native bot controls per second. Every measured prediction dispatch uses
snapshot collision, with no legacy fallback. Server work includes simulation,
snapshot and compression work; its p99 is below 1 ms in each stage. No new input
timestamp rejection, script runtime error or failed GPU present occurs. Client
and server end debt are at most one tick, with no growing sampled backlog.
The pre-launch/post-cleanup executable hashes match in both reports.

The 125/240 caps pass within the fixture's 2% tolerance; the short logic rate
guard permits a 2 Hz difference. `com_maxfps 0` removes the presentation cap,
while any chosen cap leaves the fixed gameplay clock intact. A 1,000 FPS cap
is an upper limit, not a throughput promise. The separate 1,000 FPS rendering
goal is **not met in these uncapped samples**. Views and gameplay change between
stages, so these FPS values are not controlled comparisons of identical scenes.

Eight-second samples do not establish sustained timing, complete movement/
weapons correctness, or identical authoritative results at different rendering
caps. Full-world checkpoint/replay and that deterministic rendering-independence
test remain unpassed. The current private-client roundtrip and retained input
tape have narrower scopes. These results also do not promote untested public
CoD4x capabilities or replace the separate 60-second population/map matrix.
