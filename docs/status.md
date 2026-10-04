# Implementation status

The private **v0.1** GitHub release preserves the original Build 86 workspace
and measured evidence as a development checkpoint. It does not satisfy the
full delivery gate. Repository setup added complete source snapshots, build and
recovery instructions, a checked full-workspace backup and ongoing backup tooling.
A later tooling-only fix creates `results/` for clean-clone native evidence tests;
all forty suites pass after that directory exists. The accepted root/runtime
game binaries retain that DX9 checkpoint. Native Vulkan implementation is now
under development on `vulkan-testing`; see [its status and remaining gates](VULKAN.md).
The experimental Win32 client builds, 41 current native suites pass, and 108
matched GPU image fixtures have SSIM 1.0 with zero validation errors. Checked
stock captures pass the image threshold on two views, but Vulkan throughput
remains below DX9 and the complete delivery gate remains open.

The requested release has **not passed its delivery gate**. Build 86 is the
current deployed client/server pair. All 40 native suites pass. Its camera-verified
sixty-second walking tests with one protected human and fifteen Regular combat
bots pass the 0.1% simulation, prediction, command, snapshot and per-bot control
rate checks on Killhouse and Crash. Server work p99 is 0.897/0.899 ms; neither
run grows its backlog. Graphics/audio/map restarts and normal owned Leave/Quit
also pass. The measured walking averages are 616 FPS at Killhouse 480p and
547 FPS at Crash 720p. These checked routes are not whole-map combat averages.

The renderer now stages frontend geometry in preallocated CPU buffers and uploads
on the backend owner, removing measured main-thread D3D buffer-lock stalls.
The owner barrier also waits for rendering before acquiring the dvar lock,
fixing a reproduced deadlock. The new default-off benchmark camera controls reject
drift and nearby occlusion. Historical mixed-camera FPS averages must not be used
as representative gameplay performance. See the
[current rendering report](render-performance-20261003.md) for native evidence,
hashes, short test commands and remaining frontend/Present costs. Sustained
1,000 FPS, the four-human current-build cases, deterministic full-world replay
and the public-feature release gate remain pending.

The following sections retain earlier implementation and verification history.
Build 75 produces the
root client executable, dedicated server, headless benchmark client, rebuilt SDK
fixture DLL and update helper. Direct3D 9 and custom protocol 10002 remain in use.
Build 72 preserves the accepted custom session through the three native final
reliable disconnect packets, then clears transport before client teardown. The
second-human fixture had shown a real Build 71 ghost client after Leave: bot slot
reclaim worked, but the server ignored the reset-session packets and never
refilled the bot. Its actual rerun passes: slot 15 changes from bot generation 3
to human generation 5 and refills with bot generation 11, with both 1,000-tick
sixteen-slot tapes retained. Strict map/fast/full/graphics/audio restart checks
also pass. The failed results remain in [the bot fixture record](bot-next-native-fixtures.md).

Build 73 moves the stock presentation/killcam archive to 50 ms recording while
retaining 1 ms authoritative simulation and human transport. Timestamp lookup
preserves logical killcam delay, and committed time/kind/base checks reject
phantom overflow caches and unsafe borrowed decoder storage. Archive counters
and work have separate diagnostics. Production policy contracts pass; staged
adapter tests pass 131,568 comparisons against the production adapter with
synthetic native-layout dependencies. Current native restart, Regular DM control
tape and all four sixty-second timing cases pass. Build 75's
[loaded-map native codec fixture](build75-archive-codec-validation.md) passes
full/delta, overflow, cache reuse and byte-ring boundary checks with exact cleanup.
Live spectator/killcam behavior remains pending.

Build 71's reviewed, archive-bound SDK fixtures proved twelve complete contracts:
ten [address/DNS capabilities](evidence/build71-sdk-network/index.json) and two
[identifier formatter exports](evidence/build71-sdk-identity-format/index.json).
Those proofs remain historical because later builds changed both game binaries. The
current [finite-registration integration review](../results/build73-finite-production-integration-review.json)
has 2,117 entries: zero currently proven, 2,117 pending, twenty opaque entries
still unresolved and none excluded. The finite registration correction removes
seven opaque descriptions, adds four concrete dvars and corrects field provenance.
Build 74's source changes invalidate four historical SDK proofs under the
verifier's exact source binding; those proofs are retained without carrying them
forward automatically.
Six header declarations had been mistaken for dvar calls; the reviewed scanner
correction removes exactly those six false entries and preserves actual calls.
Actual Build 73 classifier, GUID and runtime formatter export calls pass. Root
review published the two classifier and two formatter export contracts; GUID
provider failure and formatter-dvar saved startup/reload remain partial. Nine IPv4-only transport,
ban and callback contracts and `sv_usesteam64id` saved-config persistence remain
partial. These proofs bind actual source, native helpers, fixtures and deployed
binary hashes; a matching API name is not semantic regression evidence. They
make no official-service authentication claim. Historical Build 64 proofs remain
under [archived SDK network evidence](evidence/build64-sdk-network/index.json).

[Build 75](../results/build75-source-and-binary-hashes.json) passes all 40 native
suites ([build and test log](../results/build75-console.log), 11.95 seconds).
The SDK fixture adds classifier and GUID subcommands without increasing its
twenty registrations; actual current-binary calls pass in the twelve-second
[native fixture](../results/plugin-identity-contract-native-20261003-074025-734500/result.json).
Build 71's client, dedicated
server and headless executable hashes are identical to Build 70. The rebuilt SDK
fixture now uses exactly twenty command registrations: identifier formatting is
an existing command's subcommand. This fixes the rejected final UDP probe without
changing the public command limit. Build 70's failed UDP result remains preserved.
The [Build 71 serial SDK batch](build71-sdk-proof-candidates.md) passes formatter,
address/admission and DNS/UDP scopes with exact owned-process cleanup; its
reviewed proofs are published in the capability configuration.
The evidence verifier also requires independently hashed project-native helpers
and invalidates stale helper proofs. Its packed Huffman
encoder matches the actual native encoder in 243,976 comparisons, including
output padding, partial prefixes and conservative failure boundaries. The
[isolated codec measurement](../results/huffman-native-encoder-evidence.json)
is a codec microbenchmark, not match timing evidence. A retained PID directory
made the first asset-path test fail; each fixture now acquires a fresh directory
atomically. The [first failed log](../results/build67-first-build-asset-fixture-failed.log)
is preserved. The native bit-field replacement passes 666,075 exact comparisons
with the real Win32 message structure, mixed byte/bit operations, every split
seam and partial failure cursors. Its [isolated measurement](../results/msg-bits-native-evidence.json)
does not attest match performance. Native park tests cover stale event signals,
acknowledgement/phase races and 1,024 real Windows event handoffs. The worker's
published phase now verifies acknowledgement; a signal alone cannot release
mutable gameplay to main. Whole-server checkpoint integration remains opt-in and disabled.

The [Build 74 four-case timing matrix](build74-timing-validation.md) passes all
rate, strict p99, backlog, native-bot, independent-owner, collision, script,
renderer and provenance checks. Work p99 is 836/770/857/896 microseconds for
Killhouse one-human, Crash one-human, Killhouse four-human and Crash four-human.
Every sixty-second interval commits exactly 1,200 stock archive records with
no overflow, skipped opportunity or invalid index. Rendering averages
851/1,175/712/1,583 actual FPS at 480p, uncapped with VSync off. These measurements
bind Build 74; Build 75's diagnostic changes have separate codec evidence.

The [Build 75 hardware usage rerun](hardware-usage-20261003.md) also passes one
60-second Killhouse case with one human and fifteen bots: server work p99 is
856 µs, with all rates inside the 1 kHz tolerance and no growing sampled debt.
It renders 753 FPS at 480p. The RX 9070 XT 3D engine averages 19.3% busy time;
the game processes use 13.6% of total logical CPU capacity. Aggregate render
timings indicate useful profiling targets, without proving a function-level
bottleneck. The other three Build 75 timing cases remain unmeasured.

The historical [Build 71 four-case matrix](build71-timing-validation.md) passed all
rate, backlog, native-bot, independent-owner, collision, script and provenance
checks. Work p99 is 1,017/935/997/1,020 microseconds for Killhouse one-human,
Crash one-human, Killhouse four-human and Crash four-human respectively. Two
cases failed the strict sub-millisecond limit; those failures remain preserved.
Actual uncapped presents average 803/1,038/744/1,287 FPS; rendering is independent
of the rate gate. Native item/helicopter/missile/corpse timestep and killcam archive
gaps are recorded in the [focused audit](native-timestep-audit.md). The archive
adaptation is implemented in Build 73. The three reviewed item/helicopter/missile
changes are implemented in Build 74; corpse behavior and further loaded-map
physics acceptance remain pending.

After the Build 73 fixtures, three reviewed native changes were applied in
Build 74: actual-tick item tracing with a separate support probe, helicopter
think deadlines on the fixed tick, and time-scaled Javelin turn damping.
[319 extracted-production assertions](../staged/timestep/results/20261003-130601-910837/result.json)
pass against retained exact Build 73 source. Their dependencies are controlled
fixtures; loaded-map physics behavior remains unproved, while Build 74 timing
passes as recorded above. Build
73 binaries and the bot package are preserved byte for byte under
`results/build73-release-binaries`.

The [Build 73 short bot matrix](../results/build73-native-bot-matrix-validation.json)
passes all 24 difficulty/mode combinations, desired-zero admission and same-profile
menu persistence. It retains 385,000 validated actual slot records before drain.
Each difficulty uses one retained server across all six modes. Native skill
arrays, control generations, teams, movement/aim and sampled recovery are checked;
objective completion, controlled combat and reaction outcomes remain separate
gates. The initial combined run's menu-only navigation failure is preserved;
only the corrected persistence fixture was rerun.

Build 57's
[normal startup](../results/build57-normal-startup.json) from C:\Windows\Temp opens
directly into the stock multiplayer main menu, with no benchmark inputs or owned
server, uncapped rendering and VSync disabled. The existing interactive config
was backed up and restored byte for byte after the startup check. Source, binaries,
assets and profiles remain beneath the project directory; the installed game is
not modified.

Build 56 passes [72 native frontend checks](../results/build56-native-menus.json),
including disabled Singleplayer and the blocked console path, stock options
inventory and bot settings in all stock mode screens. Build 55's
[nine native profile checks](../results/build55-profile-native.json) verify unlock
backup identity/checksum, combat/challenge preservation, idempotence, corrupt
backup rejection and complete restoration. Its
[344 primary class-menu checks](../results/build55-classes-native-corrected.json)
cover all 22 stock primary weapons, supported attachments and eligible camos.
Build 56 adds [48 sidearm, grenade and perk checks](../results/build56-loadout-menus.json).
These are actual stock menu actions; live server loadouts remain a separate
gate. Build 70 verifies every Options category and both stock Apply paths below.

Build 70's [actual Regular DM bot fixture](../results/native-bots-20261003-113412-364076/result.json)
passes real stock Bot Count/Difficulty key changes and persistence, one-human/
15-bot admission, native VM inspection and 1,000 authoritative input records.
All fifteen actual skill arrays contain Regular base 3, aim time 0.4 seconds,
initial reaction 750 ms and reaction 500 ms. Native session teams are FREE in DM.
The tape records held movement, attack, ADS, sprint and melee, with four observed
spawn-time changes. Reload, frag and smoke are absent in this short sample.
Damage, projectile/reload outcomes, objectives, other difficulties/modes and
human slot reclaim remain separate unpassed gates. Both exact owned processes
exit normally and interactive preferences remain unchanged.

An [intermittent Build 69 Options failure](../results/native-fov-20261003-112220-353864/result.json)
occurred before Look opened, at the gameplay command barrier. The later
[bounded prefix diagnostic](../results/native-options-20261003-112813-417850/result.json)
passes all six attempted categories but cannot establish a race fixed. Build 70
uses compare/exchange for Running-to-Parking and clears old signals before
checking the actual parked phase; it also records Park versus control-access
failure separately. The [Build 70 functional batch](build70-functional-validation.md)
passes 72 actual Open/Back actions, all twelve Options categories, native graphics
and audio confirmation actions/restarts, archived settings/FOV persistence and
normal exact-owned cleanup. Both 480p and 720p render-cap sequences pass their
eight-second command, snapshot, prediction, bot-control, collision, work and
debt checks. At caps 125/240/1,000/uncapped, actual 480p presents average
125.0/239.9/753.5/641.8 FPS; 720p averages 124.8/237.8/469.4/608.3 FPS. These
different moving scenes are not identical-input rendering comparisons. The
independent 1,000 FPS goal and full-world replay gate remain unpassed.

Build 66 completed the four required 60-second population/map cases at 640x480,
uncapped with VSync disabled. Command and snapshot rates, bot populations,
independent prediction, snapshot collision, scripts and backlog checks passed in
all four. Three cases failed the strict p99 work gate:

| Actual match | Server work p99 | Average successful presents | Timing gate |
| --- | ---: | ---: | --- |
| [Killhouse, 1 human/15 bots](../results/mp_killhouse-60s-uncapped-480p-20261003-054346.json) | 991 us | 765.9 FPS | Pass |
| [Crash, 1 human/15 bots](../results/mp_crash-60s-uncapped-480p-20261003-053800.json) | 1,067 us | 1,275.5 FPS | Fail |
| [Killhouse, 4 humans/12 bots](../results/mp_killhouse-60s-uncapped-480p-20261003-054009.json) | 1,018 us | 669.0 FPS | Fail |
| [Crash, 4 humans/12 bots](../results/mp_crash-60s-uncapped-480p-20261003-054142.json) | 1,100 us | 1,031.3 FPS | Fail |

All final server debt readings were zero; no sampled debt grew across a run.
Snapshot work was significant, particularly with four humans. Build 67 replaces
recursive per-bit compression with precomputed immutable codes while retaining
identical packet bytes and adds a nested compression measurement. Its complete
matrix must pass before claiming release performance. Build 69's measured
compression p99 is 1 us, but all four complete cases still fail overall work:

| Actual Build 69 match | Server work p99 | Successful presents | Timing gate |
| --- | ---: | ---: | --- |
| [Killhouse, 1 human/15 bots](../results/mp_killhouse-60s-uncapped-480p-20261003-060913.json) | 1,029 us | 852.7 FPS | Fail |
| [Crash, 1 human/15 bots](../results/mp_crash-60s-uncapped-480p-20261003-061458.json) | 1,015 us | 1,238.2 FPS | Fail |
| [Killhouse, 4 humans/12 bots](../results/mp_killhouse-60s-uncapped-480p-20261003-061816.json) | 1,099 us | 656.3 FPS | Fail |
| [Crash, 4 humans/12 bots](../results/mp_crash-60s-uncapped-480p-20261003-062003.json) | 1,090 us | 1,159.5 FPS | Fail |

Rates, native bot controls, independent prediction and backlog checks pass.
All final server debt readings are zero. These reports retain exact executable, owned
process and asset provenance; failed startup inspections are preserved separately
and do not count as measurements. These archived Build 66
reports predate explicit benchmark binary provenance fields; their build is
identified by the deployment freeze, named console logs and
[Build 66 archive](../results/build66-source-and-binary-hashes.json).

The earlier [corrected Build 64 Killhouse case](../results/mp_killhouse-60s-uncapped-480p-20261003-052021.json)
passed at 991 us and 727.4 FPS; GPU busiest-engine use averaged 19.6%. GPU usage
alone is not a throughput acceptance gate. An earlier
[rejected run](../results/mp_killhouse-60s-uncapped-480p-20261003-051736.json)
had zero actual bots and cannot count toward acceptance. Startup flags now
precede latched dvar registration, and readiness checks verify actual native bot
population, slot generations and controls before measurement.

The archived [Build 55 match](../results/build55-animation-repair-killhouse-1human-15bots-60s.json)
exceeded the timing gate at **1,337 us**. A shorter
[Build 56 phase measurement](../results/build56-short-phase-profile.json) also
fails the timing gate at **1,178 us**; due scripts are its largest observed spike.
Build 57 adds opt-in per-script slice measurements for targeted optimization.

Build 56's [render-cap run](../results/build56-native-render-caps.json) is retained
as a **failed** recovery artifact, not a rendering or timing result. An automatic
map change exposed OOB handshake packets mutating the frontend connection mirror
while the simulation kept stale active state. Its later
[graphics restart](../results/build56-vid-restart-render-recovery.json) exposed a
Raw Input producer still attached to the destroyed window. Build 57 dispatches
trusted server connection controls on the actual owner state, joins before native
asset transitions, retains receipt order, hands accepted presentation state to
same-map restart archives, and safely recreates a parked input producer after
window changes. Build 58's
[native lifecycle measurement](../results/native-lifecycle-20261003-092223-199324-8252/result.json)
verifies the stock menu and a rendered one-human/15-bot Killhouse match. Crash
loads and rejoins the same owned dedicated process, with actual simulation and
GPU counters advancing, but the reliable presentation command queue reports
growing failures. This is a failed recovery gate; the subsequent restart stages
were not run. Both fixture processes exited normally and its profile was archived.
Build 59's diagnostic captured duplicate reliable command 178 at the queue front
when presentation requested 179. A generic control scope could resume a pending
native metadata barrier before committing that command, causing publication twice.
Build 61 preserves pending parks and validates journal entries before consuming
them. Its [actual lifecycle run](../results/native-lifecycle-20261003-093251-107024-18992/result.json)
passes Crash map change, fast restart and full map restart with the same owned
server and zero queue failures. Its graphics restart remains failed: native CG
reloads, but frontend tick debt survives the reload and the private owner does not
recover. Cleanup required terminating only the fixture's client. Build 62 rebases
the frontend startup clock after same-map reload without clearing the accepted
network session. Its actual restart begins a new owner, but a snapshot availability
cursor then decreases at the transport-to-presentation handoff. Build 63 rebases
that reader cursor while retaining the installed current/next snapshot pair.
Its [native lifecycle run](../results/native-lifecycle-20261003-094929-972873-16636/result.json)
passes hosting, Crash map change, both map restarts and graphics restart with the
same owned dedicated process, advancing gameplay/network/pose/GPU counters and
zero journal failures. Audio restart remains failed: the owner initially resumes,
then presentation stops and a subsequent body preparation is sampled in a script
string lookup. That single stack does not establish an abandoned lock; a bounded
diagnostic rerun records actual critical-section ownership and CPU deltas. The
[final typed diagnostic](../results/native-lifecycle-20261003-100029-388434-26592/result.json)
finds input timestamps2248ms behind accepted authoritative command time and
frozen acknowledgements. The server continues snapshots while rejecting fresh
command timestamps; retained prediction grows to997commands. The narrow
[timestamp repair](audio-restart-input-clock.md) aligns new input stamping after
an accepted snapshot without changing sequences/history or one-ms prediction.
Build 64's [actual repaired lifecycle](../results/native-lifecycle-20261003-101014-074279-27756/result.json)
passes hosting, Crash map change, fast/full map restarts, graphics restart, audio
restart, explicit Leave and Quit in 49.2 seconds. The same owned dedicated
process survives all restarts, actual admitted commands/snapshots and client
acknowledgements advance, new timestamp rejections are zero, and both processes
exit normally. This is actual restart evidence, not full-world replay.

Build 62's [12-second native script profile](../results/native-checkpoint-profile-20261003-094510-981608-2444/result.json)
identifies Bot Warfare's `target()` as the largest total script slice (2,672 calls,
1,099,175 us total, 1,021 us maximum). Instrumented server work p99 is **1,191 us**,
with zero debt; this is hotspot evidence, not a passing 60-second acceptance run.

Native services already have scoped controlled evidence: Build 56
[content transfer](../results/native-content-20261003-084027-411313-20032/result.json)
reaches admission, real HTTP download/checksum, mounted marker and native pure
lists; [browser discovery](../results/native-browser-20261003-084039-315618-26068/result.json)
uses the actual local master, native server queries and persisted favorites.
[Native screenshot transfer](../results/native-screenshot-20261003-082934-158154-6376/result.json)
captures, encodes, transfers and saves an actual game image. The
[loaded Build 56 SDK fixture](../results/plugin-native-56-20261003-034151/result.json)
passes 15 scoped cases, including exact command arguments, unchanged complete
stats, actual HTTP POST/owner completion and authoritative 1 ms callbacks.
Build 57 integrates bounded plugin filesystem and TCP services. Their
[deployed DLL fixture](../results/plugin-io-native-20261003-041520-532000/result.json)
passes 28 filesystem and 24 TCP cases, including retained buffer ownership,
tick-time blocking rejection, real binary traffic, cancellation and complete
unload cleanup. Build 58's
[console output fixture](../results/plugin-print-native-20261003-042133-558200/result.json)
verifies synchronous direct RCON output and queued authoritative/worker output.
These are scoped semantic checks, not a claim of complete public SDK parity.
Build 64's [loaded SDK2 address fixture](../results/plugin-net-native-20261003-051551-805200/result.json)
passes 40 native public-address/numeric/mask/format/LAN assertions and eight
transient IPv4 ban assertions. A real human admission exercises the public
32-byte connect/reserved-slot callback layout and granted reserved slot. The
rebuilt host/DLL use SDK revision 2 atomically; the engine's native 20-byte IPv4
address remains unchanged. Its [native UDP/DNS fixture](../results/plugin-udp-native-20261003-051325-019300/result.json)
passes 22 harness cases, actual hostname IPv4/IPv6/TCP resolutions, configured
source-port UDP sends, receive/send callbacks and veto/mutation behavior, bounded
copied injections, module-generation rejection and complete worker/DNS cleanup.
IPv6 game UDP transport and IPv6 bans remain pending.

The new private client checkpoint uses real parked-worker mementos for ten native
domains, including CG/CL/netchan/history, input, scheduler, pose/XAnim, aim,
projectiles and gameplay effects. It is **not** a restored server world or a
full-world replay result. [Build 56's actual capture](../results/build56-native-capture-1000.json)
contains 1,000 valid seven-domain canonical observations with restore conditions
zero and comparison eligibility false. The remaining replay gate requires
actual authoritative consumption timelines and native server game/script/RNG,
allocator and physics restoration under the active renderer.
Build 58 adds a bounded authoritative tape hook at actual command consumption,
including native bot controls and slot generations. Its isolated tests pass;
deployed tape capture and full restored-world replay remain separate gates.
Build 64's actual private roundtrip stopped before capture at a transport fence.
Build 65 corrects the native retained fragment-extent predicate and adds bounded,
authenticated refusal diagnostics. Its first [actual roundtrip](../results/native-checkpoint-checkpoint-20261003-102543-633066-27968/result.json)
restores all ten client providers and seven canonical domains with zero native
failures and unchanged session/generation. The overall fixture is retained as
failed because its explicit Leave polling encountered a menu-reload timeout;
normal cleanup still exited the exact client/server and restored the prior
benchmark home. The corrected [complete fixture](../results/native-checkpoint-checkpoint-20261003-102656-858730-14032/result.json)
passes in 25.7 seconds, including real resumed acknowledgements/prediction/GPU,
normal owned-process cleanup and prior benchmark-home restoration. Build 65's
[strict lifecycle repeat](../results/native-lifecycle-20261003-102739-705318-28636/result.json)
also passes all map/video/audio stages and Leave/Quit in 48.9 seconds.
The 1,000-tick authoritative tape has actual first/last validation; retaining
every serialized record is pending a bounded export and is not inferred from
draining the queue.

The server checkpoint coordinator and public address bridge pass
[two standalone native suites](../results/checkpoint-address-tests.log). They are
included as isolated suites; required server-world provider integration is still
incomplete. Source-owned fragments for the world, scripts/allocators, network,
runtime, administration and animation are staged outside the game build. Native
syntax checks do not establish actual authoritative restoration or replay.

Required remaining gates include deployed map/video/audio recovery, all bot
skills/modes and human admission, the four 60-second match measurements, server
p99 below 1 ms, full restored-world replay, complete stock gameplay/options and
profile persistence coverage, packet faults and bounded service recovery, and
reviewed evidence for every applicable public capability. Official authentication,
legacy compatibility and acceptance by external services are excluded. Public
update/master endpoints remain unconfigured by default. Earlier measurements
below are archived and cannot satisfy the current release gate.

## Archived Build 43 baseline

## Implemented

- MSVC 2022 Release builds, project-local pinned DirectX dependency, copied runtime
  assets, build/launch/benchmark tools, and preserved upstream notices.
- Absolute QPC deadlines, high-resolution waits, bounded final spin, eight-tick
  catch-up limits, retained debt, and work/lateness telemetry.
- Packet validation and bounded sequence queues. Authoritative ticks apply input;
  missing input is held without replaying press edges.
- Shared 1 ms Pmove, fractional velocity/bob/idle phases, and elapsed-time changes
  for turrets, missiles, helicopter and vehicle calculations. See the
  [physics audit](physics-audit.md) for the remaining comparisons.
- 64-bit script deadlines, a fixed-capacity heap/calendar, seconds-based waits,
  next-tick zero waits, current-tick frame-end waits and phase-correct notifications.
- Protocol 10002, session/tick/input/ack headers, incompatible-client rejection,
  unique sequence tracking, map-session recovery and acknowledgement-based prediction.
- Two-second input rings, 512-entry snapshot rings and 250 ms body rewind with
  generation/respawn guards and restoration of origin/stance bounds.
- Timestamped mouse Raw Input and bounded keyboard queues. Catch-up ticks consume
  events through their deadline; keyboard events still use Windows key messages.
- Uncapped presentation can run between simulation deadlines, including more than
  one real frame per millisecond. Console /com_maxfps 240 sets a cap and
  /com_maxfps 0 removes it. The independent simulation owner continues while
  presentation and the renderer backend execute.
- Independent simulation/input owners, private gameplay CG/animation/aim/visibility,
  dedicated CM scratch, three-slot player/world publication, bounded event and
  reliable-command journals, and acknowledged map/asset lifecycle barriers.
  Full-world replay and unsupported dynamic-world parity remain separate gates.
- Fixed-capacity collision records derived from accepted snapshots and real BSP
  geometry. Prediction binds this path for complete supported worlds. Unsupported
  worlds use an explicitly counted legacy path. Binding rejects incomplete/stale
  records and diagnostic dynamic-entity poses. See [thread ownership](thread-ownership.md).
- Quiet benchmark clients, automatic initial team/class selection, scripted input,
  active-gameplay checks and script-error checks. Quiet clients suppress active
  rendering, cosmetic FX and audio, but still initialize Direct3D/client assets.
- Fragmented telemetry reassembly with source/size/deadline checks, corrected RCON
  chunk boundaries, binary hashes, GPU/CPU/private-memory sampling, and frame-stage
  wall timings. Steady ticks do not allocate or log for these counters.

## Verification

The unmodified dedicated foundation loads Killhouse at 20 Hz with the copied game
data. The custom client connects and spawns. Build 43 passes both native suites
(khz_core and khz_publication), as recorded in its
[build log](../results/custom-build-43.log).

Build 43 passes 60-second matches on both target maps with one uncapped rendered
client and three quiet protocol clients at 720p. Each client's prediction and
unique command/snapshot rates satisfy 1,000 ±0.1%. Server simulation plus snapshot
work p99 stays below 1 ms, sampled debt does not grow, and script errors are zero.
Authoritative alive-time and actual prediction activity pass for every player.

| Map | Simulation Hz | Aggregate commands/s | Aggregate snapshots/s | Server work p99 | Successful presents/s |
| --- | ---: | ---: | ---: | ---: | ---: |
| [Killhouse](../results/mp_killhouse-60s-uncapped-720p-20261003-000813.json) | 1000.007 | 3999.993 | 4000.027 | 290 us | 1089.54 |
| [Crash](../results/mp_crash-60s-uncapped-720p-20261003-000958.json) | 1000.011 | 4000.045 | 4000.045 | 351 us | 1450.36 |

All four clients on each map used snapshot collision for 100% of measured
prediction dispatches, with zero legacy collision dispatches. This measures handler
selection, not the number or completeness of collision traces. The sampled maps
had no dynamic-entity brushes; unsupported dynamic worlds still require a fixed
physics owner. The [Build 43 collision probes](../results/collision-43-fourplayer-console.log)
pass 46 capsule sweeps per map with zero trace/pose/coverage mismatches and zero
ordering ambiguities. All nine rejection/binding guards pass (mask 511), with none
skipped. These are bounded probes, not complete movement or world parity tests.

The rendered client's average busiest GPU-engine activity was 22.53% on Killhouse
and 31.25% on Crash. These are WDDM busy-time measurements, not shader throughput.
Successful Present calls count rendered frames independently of frontend requests;
they are not monitor refreshes. CPU-side frame intervals overlap and must not be
added together or interpreted as GPU execution durations. Different spawns/cameras
prevent a controlled speedup or resolution comparison. Vulkan has not been adopted;
finer CPU/GPU timelines are needed to attribute remaining rendering limits.

The [same-session cap probe](../results/render-caps-mp_crash-720p-20261003-001335.json)
passes all four eight-second stages: 125/240/1,000/uncapped yield
124.97/240.03/834.13/1237.45 successful presents per second. Every stage preserves
approximately 1 kHz client/server rates, bounded debt and complete snapshot-collision
dispatch coverage. Short-stage rate tolerance is ±2 Hz. A cap is a maximum.
The separate [20-second 480p Crash sample](../results/mp_crash-20s-uncapped-480p-20261003-001532.json)
passes gameplay checks at 1456.52 presents/s, server work p99 172 us and average
busiest GPU-engine activity 23.27%. It uses one client and a different scene, so
it is not a controlled comparison with the four-player 720p measurement.

The [Build 42 quick regression](../results/quick-42-killhouse-console.log) passes
native publication/clock/queue checks, seven UDP telemetry tests, actual GSC wait
and wrap probes, 10,000-step shared Pmove replay, function-level physics checks,
body rewind/restoration, a 50 ms scheduler stall, map changes/restarts,
kick/replacement and a 30-second impaired-network session. Build 43 adds collision
diagnostic guards; its native suites, collision probes and the matches above were
rerun. Binary hashes identify each build separately in
[latest validation](../results/latest-validation.json).

GSC probes cover 0/1/50/100 ms waits, upward rounding, frame-end behavior, repeated
notifications in one phase and the former 24-bit clock boundary. Pmove replay hashes
match at immutable presentation-read cadences of 125/240/1,000 Hz with loaded BSP.
That replay does not run the actual renderer or hash the full authoritative world.
The physics checks cover turret timers/rotation, rocket trajectory, Javelin
acceleration, helicopter acceleration/jitter and fractional vehicle phases.

Earlier rate-only matches containing stuck spectators are not gameplay acceptance.
Older measurements remain under results as build-specific history. New checks also
require supported snapshot collision, actual prediction activity, alive gameplay,
script health and successful GPU presentation.

## Required before release

- Extend validation of the independent client simulation owner through queue
  pressure, physical input/focus changes, long rendering stalls and native errors.
  Validate wider animation/aim/item parity and map/asset lifetime recovery.
- Supply fixed-tick dynamic-entity physics poses for unsupported collision worlds;
  validate movers, vehicles, ragdolls and wider snapshot/map lifecycle cases.
- Implement a GPU-free protocol benchmark client.
- Finish timestep, rounding and damping comparisons, cosmetic ODE physics and
  moving-world scenarios. Assert acceleration, strafing, jumping, stairs, slopes,
  ladders, mantling, stance changes, weapons/reloads/grenades, movers/triggers,
  damage and respawning against stock tuning.
- Replay identical timestamped inputs with the real renderer at 125/240/1,000 FPS
  and uncapped, comparing the complete authoritative world.
- Validate historical bone poses and actual hit/headshot correctness; current
  rewind stores body positions and stance bounds.
- Audit worst-case baseline entity-pool exhaustion; bounded rings alone do not
  guarantee every changed-entity baseline survives 512 ms.
- Extend fault/population/stall and memory stability coverage. Legacy game-time
  integers need long-duration overflow handling independently of the 64-bit clock.
- Validate physical input, private LAN binding, scene-dependent maximum FPS and
  complete CoD4x feature integration.

Benchmarks default to **60 seconds**, following the revised test preference.
Render-cap stages last eight seconds; separate render samples last 20 seconds.
These short tests establish measured throughput for their scenes and hardware,
not a hard 1 ms latency bound or long-duration stability. Ordinary presentation
executes independently of active gameplay. Explicit native lifecycle/metadata
barriers pause the owner; resulting scheduler debt remains visible.

## CoD4x integration checklist

Each feature group remains incomplete. Four individual current-build SDK exports
have published regression evidence; the remaining entries require integration or
verification. Existing KisakCOD/game behavior is not counted as a ported CoD4x
feature without a checked semantic contract.

- [ ] Fixes and administration
- [ ] Bans and identification
- [ ] Downloads
- [ ] Scripting/plugin extensions
- [ ] Server browser and update functionality
- [ ] Screenshot functionality
- [ ] Authenticated client integration through a separately supported implementation

Default launch scripts bind to localhost. Private LAN needs explicit binding
configuration and separate verification.

Build 66's [retained native tape fixture](../results/native-checkpoint-checkpoint-20261003-103547-407672-12992/result.json)
validates all 1,000 consecutive authoritative ticks and 16,000 real slot input
records. The complete 8,005,788-byte tape has SHA256
`f06dec88249a533fb981627d5efc01d910fc9d3fbc40feba7f0a910562453098`.
Atomic export rejects incomplete/inactive captures and an exclusively created
temporary-file collision without losing the retained ledger or existing output.
Its 144,988 us off-tick diagnostic export is an intentional pause, not steady
match timing evidence. All ten private client providers restore successfully;
whole authoritative-world restoration and renderer-active replay remain pending.

Build 66's [actual FOV fixture](../results/native-fov-20261003-103632-183625/result.json)
passes ten hip/ADS projection and sensitivity cases at scales 0.2, 0.5, 1.25 and 2,
including nonfinite alias rejection. This is stock-weapon geometry evidence;
respawn, map persistence, other weapons and Options Apply need further fixtures.

Build 67 repeats [retained native tape/client restoration](../results/native-checkpoint-checkpoint-20261003-105614-345984-22600/result.json)
and [strict lifecycle recovery](../results/native-lifecycle-20261003-105713-603525-25916/result.json)
successfully on its actual deployed binary. Its
[extended FOV fixture](../results/native-fov-20261003-105512-667315/result.json)
failed because the stock respawn script sends `cg_fovscale 1`, replacing the
archived 1.25 preference on map initialization. The client now treats that scale
as a local preference and ignores the server-script reset; the next build must
verify map/respawn persistence and actual Options Apply before accepting the fix.
Legitimate stock class/weapon changes are measured with their actual hip/ADS
geometry; a reset setting fails before any fixture recalibration.
